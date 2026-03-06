/**
 * @file cloud_backend.cpp
 * @brief Groq Cloud AI Backend implementation via libcurl REST API
 *
 * Makes HTTP POST requests to Groq's OpenAI-compatible chat/completions
 * endpoint. Reconstructs conversation context from std::vector<Message>.
 * Thread-safe, cancellable via progress callback.
 */

#include "cloud_backend.h"
#include <nlohmann/json.hpp>

#ifdef VISION_HAS_CLOUD
#include <curl/curl.h>
#endif

#include <cstdlib>
#include <sstream>
#include <mutex>

#ifdef VISION_HAS_SPDLOG
#include <spdlog/spdlog.h>
#define LOG_INFO(...) spdlog::info(__VA_ARGS__)
#define LOG_ERROR(...) spdlog::error(__VA_ARGS__)
#define LOG_WARN(...) spdlog::warn(__VA_ARGS__)
#else
#define LOG_INFO(...) (void)0
#define LOG_ERROR(...) (void)0
#define LOG_WARN(...) (void)0
#endif

using json = nlohmann::json;

namespace vision {

// ═══════════════════ Constructor / Destructor ═══════════════════════

CloudBackend::CloudBackend(const std::string& api_key, const std::string& model)
    : api_key_(api_key), model_(model) {
    if (api_key_.empty()) {
        api_key_ = resolveApiKey();
    }
}

CloudBackend::~CloudBackend() {
    shutdown();
}

// ═══════════════════ Lifecycle ═════════════════════════════════════

bool CloudBackend::initialize() {
#ifdef VISION_HAS_CLOUD
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) return true;

    // Global curl init — must only happen ONCE per process (thread-safe)
    static std::once_flag curl_once;
    std::call_once(curl_once, []() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    });

    curl_ = curl_easy_init();
    if (!curl_) {
        LOG_ERROR("CloudBackend: Failed to initialize libcurl handle");
        return false;
    }

    if (api_key_.empty()) {
        LOG_ERROR("CloudBackend: No API key set. Set GROQ_API_KEY env var or call setApiKey()");
        curl_easy_cleanup(curl_);
        curl_ = nullptr;
        return false;
    }

    initialized_ = true;
    LOG_INFO("CloudBackend initialized: model={}, endpoint={}", model_, endpoint_);
    return true;
#else
    LOG_WARN("CloudBackend: libcurl not available (compiled without VISION_HAS_CLOUD)");
    return false;
#endif
}

void CloudBackend::shutdown() {
#ifdef VISION_HAS_CLOUD
    std::lock_guard<std::mutex> lock(mutex_);
    if (curl_) {
        curl_easy_cleanup(curl_);
        curl_ = nullptr;
    }
    initialized_ = false;
    LOG_INFO("CloudBackend shutdown complete");
#endif
}

bool CloudBackend::isReady() const {
    return initialized_;
}

// ═══════════════════ Inference ═════════════════════════════════════

std::string CloudBackend::generate(const std::string& prompt,
                                   const std::vector<Message>& history) {
#ifdef VISION_HAS_CLOUD
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ && !initialize()) return "";

    cancel_ = false;

    // Build request JSON
    std::string request_body = buildRequestJson(prompt, history);

    // Prepare response buffer
    std::string response_data;

    // Reset curl handle for reuse
    curl_easy_reset(curl_);

    // Set URL
    curl_easy_setopt(curl_, CURLOPT_URL, endpoint_.c_str());

    // POST method with JSON body
    curl_easy_setopt(curl_, CURLOPT_POST, 1L);
    curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, request_body.c_str());
    curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, (long)request_body.size());

    // Headers: Authorization + Content-Type
    struct curl_slist* headers = nullptr;
    std::string auth_header = "Authorization: Bearer " + api_key_;
    headers = curl_slist_append(headers, auth_header.c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);

    // Write callback to capture response
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response_data);

    // Progress callback for cancellation support
    curl_easy_setopt(curl_, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl_, CURLOPT_PROGRESSFUNCTION, progressCallback);
    curl_easy_setopt(curl_, CURLOPT_PROGRESSDATA, this);

    // Timeout
    curl_easy_setopt(curl_, CURLOPT_TIMEOUT, (long)timeout_seconds_);
    curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, 10L);

    // SSL verification (enable for production)
    curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYHOST, 2L);

    // Perform the request
    CURLcode res = curl_easy_perform(curl_);

    // Cleanup headers
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        if (res == CURLE_ABORTED_BY_CALLBACK) {
            LOG_WARN("CloudBackend: Request cancelled by user");
        } else {
            LOG_ERROR("CloudBackend: curl request failed: {}", curl_easy_strerror(res));
        }
        return "";
    }

    // Check HTTP status code
    long http_code = 0;
    curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code != 200) {
        LOG_ERROR("CloudBackend: HTTP {} from Groq API. Response: {}",
                  http_code, response_data.substr(0, 500));
        return "";
    }

    // Parse the JSON response and extract content
    std::string result = parseResponseJson(response_data);
    if (result.empty()) {
        LOG_WARN("CloudBackend: Empty response from Groq API");
    }

    return result;
#else
    (void)prompt;
    (void)history;
    return "";
#endif
}

std::vector<float> CloudBackend::getEmbeddings(const std::string& text) {
    // Cloud embeddings not supported via Groq — return empty
    // LLMController orchestrator will fall back to LocalBackend for embeddings
    (void)text;
    LOG_INFO("CloudBackend: Embeddings not supported in cloud mode, use LocalBackend");
    return {};
}

void CloudBackend::cancelGeneration() {
    cancel_ = true;
}

std::string CloudBackend::info() const {
    std::ostringstream ss;
    ss << "Groq Cloud (" << model_ << ") "
       << (initialized_ ? "[READY]" : "[NOT INITIALIZED]")
       << " | endpoint: " << endpoint_
       << " | timeout: " << timeout_seconds_ << "s";
    return ss.str();
}

// ═══════════════════ Internal Helpers ══════════════════════════════

std::string CloudBackend::buildRequestJson(const std::string& prompt,
                                           const std::vector<Message>& history) const {
    json messages = json::array();

    // Reconstruct the full conversation from history
    // This is the Context Persistence / Synchronization logic:
    // when switching from Local → Cloud, the cloud backend receives
    // the same std::vector<Message> and rebuilds the API request
    for (const auto& msg : history) {
        messages.push_back({
            {"role", msg.role},
            {"content", msg.content}
        });
    }

    // Add the current prompt as the latest user message
    // (only if it's not already the last message in history)
    bool already_in_history = false;
    if (!history.empty() && history.back().role == "user"
        && history.back().content == prompt) {
        already_in_history = true;
    }
    if (!already_in_history) {
        messages.push_back({
            {"role", "user"},
            {"content", prompt}
        });
    }

    json request = {
        {"model", model_},
        {"messages", messages},
        {"temperature", temperature_},
        {"max_tokens", max_tokens_},
        {"stream", false}
    };

    return request.dump();
}

std::string CloudBackend::parseResponseJson(const std::string& response) const {
    try {
        auto j = json::parse(response);

        // Standard OpenAI-compatible response format
        if (j.contains("choices") && !j["choices"].empty()) {
            auto& choice = j["choices"][0];
            if (choice.contains("message") && choice["message"].contains("content")) {
                return choice["message"]["content"].get<std::string>();
            }
        }

        // Error response
        if (j.contains("error")) {
            std::string err_msg = j["error"].value("message", "Unknown error");
            LOG_ERROR("CloudBackend: API error: {}", err_msg);
            return "";
        }
    } catch (const json::exception& e) {
        LOG_ERROR("CloudBackend: Failed to parse API response: {}", e.what());
    }
    return "";
}

std::string CloudBackend::resolveApiKey() const {
    // Try environment variable first
    const char* env_key = std::getenv("GROQ_API_KEY");
    if (env_key && env_key[0] != '\0') {
        return std::string(env_key);
    }
    return "";
}

#ifdef VISION_HAS_CLOUD
size_t CloudBackend::writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* response = static_cast<std::string*>(userdata);
    size_t total = size * nmemb;
    response->append(ptr, total);
    return total;
}

int CloudBackend::progressCallback(void* clientp, double /*dltotal*/, double /*dlnow*/,
                                   double /*ultotal*/, double /*ulnow*/) {
    auto* self = static_cast<CloudBackend*>(clientp);
    // Return non-zero to abort the transfer
    return self->cancel_.load() ? 1 : 0;
}
#endif

} // namespace vision
