#pragma once
/**
 * @file cloud_backend.h
 * @brief Groq Cloud AI Backend via REST API (libcurl)
 *
 * Implements IAIBackend for Groq's OpenAI-compatible chat completions
 * endpoint. Uses libcurl for HTTP POST requests with JSON payloads.
 * Thread-safe, cancellable, and fully asynchronous-ready.
 */

#include "ai_backend.h"
#include <string>
#include <vector>
#include <mutex>
#include <atomic>

#ifdef VISION_HAS_CLOUD
// Forward declare CURL handle type to avoid pulling in curl headers everywhere
typedef void CURL;
#endif

namespace vision {

class CloudBackend : public IAIBackend {
public:
    /// @param api_key  Groq API key (or empty to read from env GROQ_API_KEY)
    /// @param model    Model identifier (default: llama-3.3-70b-versatile)
    explicit CloudBackend(const std::string& api_key = "",
                          const std::string& model = "llama-3.3-70b-versatile");
    ~CloudBackend() override;

    // Non-copyable
    CloudBackend(const CloudBackend&) = delete;
    CloudBackend& operator=(const CloudBackend&) = delete;

    // ── IAIBackend interface ─────────────────────────────────────
    bool initialize() override;
    void shutdown() override;
    bool isReady() const override;

    std::string generate(const std::string& prompt,
                         const std::vector<Message>& history,
                         StreamCallback stream_cb = nullptr) override;

    std::vector<float> getEmbeddings(const std::string& text) override;

    void cancelGeneration() override;
    BackendType type() const override { return BackendType::Cloud; }
    std::string info() const override;

    // ── Cloud-specific configuration (thread-safe) ───────────────
    void setModel(const std::string& model) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        model_ = model;
    }
    void setTemperature(float temp) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        temperature_ = temp;
    }
    void setMaxTokens(int tokens) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        max_tokens_ = tokens;
    }
    void setApiKey(const std::string& key) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        // Securely overwrite old key before replacing
        if (!api_key_.empty()) {
            volatile char* p = &api_key_[0];
            for (size_t i = 0; i < api_key_.size(); ++i) p[i] = 0;
        }
        api_key_ = key;
    }
    void setEndpoint(const std::string& url) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        endpoint_ = url;
    }

private:
#ifdef VISION_HAS_CLOUD
    CURL* curl_ = nullptr;
#endif

    std::string sse_buffer_;
    StreamCallback active_stream_cb_ = nullptr;

    std::string api_key_;
    std::string model_;
    std::string endpoint_ = "https://api.groq.com/openai/v1/chat/completions";
    float temperature_ = 0.1f;
    int max_tokens_ = 1024;
    int timeout_seconds_ = 30;

    bool initialized_ = false;
    std::atomic<bool> cancel_{false};
    mutable std::recursive_mutex mutex_;

    // ── Internal helpers ─────────────────────────────────────────
    std::string buildRequestJson(const std::string& prompt,
                                 const std::vector<Message>& history) const;
    std::string parseResponseJson(const std::string& response) const;
    std::string resolveApiKey() const;

    /// libcurl write callback (static)
    static size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata);

    /// libcurl progress callback for cancellation (static)
    static int progressCallback(void* clientp, double dltotal, double dlnow,
                                double ultotal, double ulnow);
};

} // namespace vision
