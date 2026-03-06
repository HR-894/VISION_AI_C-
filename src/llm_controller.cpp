/**
 * @file llm_controller.cpp
 * @brief Dual-Inference Engine Orchestrator
 *
 * Manages CloudBackend (Groq REST) and LocalBackend (llama.cpp).
 * Handles backend switching with context persistence, async execution,
 * response caching, JSON parsing, and anti-hallucination fallback.
 *
 * All original public methods are preserved for backward compatibility
 * with ReActAgent, AgentMemory, and VisionAI.
 */

#include "llm_controller.h"
#include "local_backend.h"
#include "cloud_backend.h"
#include "instruction_translator.h"

#include <filesystem>
#include <functional>
#include <regex>
#include <sstream>
#include <thread>
#include <cmath>
#include <algorithm>

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

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace vision {

// ═══════════════════ Constructor / Destructor ═══════════════════════

LLMController::LLMController(const std::string& model_path) {
    // Create both backends
    local_backend_ = std::make_unique<LocalBackend>(model_path);
    cloud_backend_ = std::make_unique<CloudBackend>();  // Reads GROQ_API_KEY from env

    // Default to local backend
    active_backend_ = local_backend_.get();
    active_type_ = BackendType::Local;

    // ── Auto-detect model families for Instruction Translator ──
    // Detect local model family from GGUF filename
    local_model_family_ = InstructionTranslator::detectFamily(
        local_backend_->info());  // info() contains the model path
    active_family_ = local_model_family_;

    // Cloud default is Llama-3 (Groq's default model)
    cloud_model_family_ = ModelFamily::Llama3;

    LOG_INFO("LLMController: Dual-engine initialized (Local: {} | Cloud: {})",
             static_cast<int>(local_model_family_),
             static_cast<int>(cloud_model_family_));
}

LLMController::~LLMController() {
    // FIX C1: Wait for any outstanding async generation to complete
    // before destroying backends — prevents use-after-free
    if (async_future_.valid()) {
        try { async_future_.wait(); } catch (...) {}
    }
    // Safely shutdown both backends
    if (local_backend_) local_backend_->shutdown();
    if (cloud_backend_) cloud_backend_->shutdown();
    LOG_INFO("LLMController: Both backends shut down — all resources freed");
}

// ═══════════════════ Original Public API (backward compatible) ══════

bool LLMController::isAvailable() {
#ifdef VISION_HAS_LLM
    return true;
#else
    return false;
#endif
}

bool LLMController::loadModel() {
    std::lock_guard<std::recursive_mutex> lock(llm_mutex_);
    if (!active_backend_) return false;
    return active_backend_->initialize();
}

void LLMController::unloadModel() {
    std::lock_guard<std::recursive_mutex> lock(llm_mutex_);
    if (active_backend_) {
        active_backend_->shutdown();
    }
}

bool LLMController::isModelLoaded() const {
    std::lock_guard<std::recursive_mutex> lock(llm_mutex_);  // FIX: was unprotected
    if (!active_backend_) return false;
    return active_backend_->isReady();
}

void LLMController::setGPULayers(int layers) {
    if (local_backend_) local_backend_->setGPULayers(layers);
}

void LLMController::setContextSize(int size) {
    if (local_backend_) local_backend_->setContextSize(size);
}

void LLMController::setThreadCount(int count) {
    if (local_backend_) local_backend_->setThreadCount(count);
}

void LLMController::cancelGeneration() {
    if (active_backend_) active_backend_->cancelGeneration();
}

void LLMController::resetCancel() {
    // Cancel flags are auto-reset in backends after generation
}

void LLMController::checkIdleUnload() {
    if (local_backend_) local_backend_->checkIdleUnload();
}

// ═══════════════════ Core Generation ═══════════════════════════════

std::string LLMController::generateResponse(const std::string& prompt) {
    std::lock_guard<std::recursive_mutex> lock(llm_mutex_);
    if (!active_backend_) return "";

    // Initialize if needed
    if (!active_backend_->isReady() && !active_backend_->initialize()) {
        LOG_ERROR("LLMController: Failed to initialize active backend");
        return "";
    }

    // FIX B5: Use canonical cache key (strips volatile parts from ReAct prompts)
    std::string cache_key = canonicalCacheKey(prompt);

    // Check cache (inline to avoid nested lock on cache_mutex_)
    {
        std::lock_guard<std::mutex> clock(cache_mutex_);
        auto it = cache_.find(cache_key);
        if (it != cache_.end()) {
            auto elapsed = std::chrono::steady_clock::now() - it->second.time;
            if (elapsed < std::chrono::seconds(cache_ttl_seconds_)) {
                return it->second.response;
            }
            cache_.erase(it);
        }
    }

    // Delegate to the active backend with conversation context
    std::string result = active_backend_->generate(prompt, conversation_);

    if (!result.empty()) {
        std::lock_guard<std::mutex> clock(cache_mutex_);
        cache_[cache_key] = {result, std::chrono::steady_clock::now()};

        // FIX: O(1) eviction instead of O(n) scan
        if (cache_.size() > 100) {
            cache_.clear();  // Simple and safe — no O(n) scan on hot path
        }
    }

    return result;
}

std::string LLMController::generateReactResponse(const std::string& prompt) {
    return generateResponse(prompt);
}

// ═══════════════════ Embeddings ════════════════════════════════════

std::vector<float> LLMController::getEmbeddings(const std::string& text) {
    std::lock_guard<std::recursive_mutex> lock(llm_mutex_);  // FIX: was unprotected
    if (!local_backend_) return {};

    // WARNING: If cloud is active, local was shut down to free VRAM.
    // Re-initializing here loads ~4GB back into VRAM alongside the cloud usage.
    if (!local_backend_->isReady()) {
        LOG_WARN("LLMController: Re-initializing local backend for embeddings "
                 "(cloud is active — this will use extra VRAM)");
        if (!local_backend_->initialize()) return {};
    }
    return local_backend_->getEmbeddings(text);
}

// ═══════════════════ ReAct Step ════════════════════════════════════

std::optional<json> LLMController::reactStep(
    const std::string& task,
    const json& observation,
    const std::vector<json>& history) {

    std::string prompt = "You are a Windows AI assistant. "
        "Respond with a JSON object containing 'thought' and 'action'.\n\n"
        "Task: " + task + "\n\n"
        "Current observation:\n" + observation.dump(2) + "\n\n";

    if (!history.empty()) {
        // Strict sliding window: last 3 entries
        std::vector<json> recent_history;
        int start_idx = std::max(0, (int)history.size() - 3);
        for (int i = start_idx; i < (int)history.size(); ++i) {
            recent_history.push_back(history[i]);
        }
        prompt += "Recent actions:\n" + formatHistory(recent_history) + "\n\n";
    }

    prompt += "Respond with ONLY a JSON object:\n"
              "{\"thought\": \"your reasoning\", "
              "\"action\": \"action_name\", "
              "\"params\": {\"key\": \"value\"}}";

    std::string response = generateResponse(prompt);
    if (response.empty()) return std::nullopt;

    try {
        auto parsed = parseJsonStrict(response);
        return validateAndFill(parsed);
    } catch (...) {
        LOG_WARN("Failed to parse ReAct response: {}", response);
        return std::nullopt;
    }
}

std::optional<json> LLMController::parseAmbiguousCommand(
    const std::string& command, const std::string& context) {

    // FIX S1: Escape quotes to prevent prompt injection
    std::string safe_cmd = command;
    for (size_t i = 0; i < safe_cmd.size(); i++) {
        if (safe_cmd[i] == '"') safe_cmd.insert(i++, "\\");
    }

    std::string prompt = "Parse this command into a JSON action. Command: \"" + safe_cmd + "\"";
    if (!context.empty()) prompt += "\nContext: " + context;
    prompt += "\n\nRespond with ONLY: {\"action\": \"name\", \"params\": {}}";

    std::string response = generateResponse(prompt);
    if (response.empty()) return std::nullopt;

    try { return parseJsonStrict(response); }
    catch (...) { return std::nullopt; }
}

// ═══════════════════ Dual-Engine API ══════════════════════════════

void LLMController::setBackend(BackendType type) {
    std::lock_guard<std::recursive_mutex> lock(llm_mutex_);

    if (type == active_type_ && active_backend_) {
        LOG_INFO("LLMController: Already using {} backend",
                 type == BackendType::Local ? "Local" : "Cloud");
        return;
    }

    LOG_INFO("LLMController: Switching backend from {} to {}",
             active_type_ == BackendType::Local ? "Local" : "Cloud",
             type == BackendType::Local ? "Local" : "Cloud");

    // ── Memory Safety: Shutdown the old backend to free VRAM/RAM ──
    if (active_backend_) {
        LOG_INFO("LLMController: Shutting down old backend to free resources...");
        active_backend_->shutdown();
    }

    // ── Instruction Translation: Adapt persona for new model family ──
    ModelFamily old_family = active_family_;
    ModelFamily new_family = (type == BackendType::Cloud)
                             ? cloud_model_family_
                             : local_model_family_;

    if (old_family != new_family && !conversation_.empty()) {
        LOG_INFO("LLMController: Translating {} conversation messages from {} to {}",
                 conversation_.size(),
                 static_cast<int>(old_family),
                 static_cast<int>(new_family));

        // FIX: Always translate from ORIGINAL system prompt to prevent drift.
        // Previously, repeated Local→Cloud→Local cycles would compound
        // translation artifacts (expand→compress→expand) until garbled.
        for (auto& msg : conversation_) {
            if (msg.role == "system" && !system_prompt_.empty()) {
                // Use the original system_prompt_ as source of truth
                msg.content = translator_.translateSystemPrompt(
                    system_prompt_, ModelFamily::Generic, new_family);
            }
        }

        // Update the working system_prompt_ for consistency
        if (!system_prompt_.empty()) {
            // Note: system_prompt_ stores the ORIGINAL, untranslated version.
            // We only translate the copy inside conversation_.
        }
    }

    active_family_ = new_family;

    // Switch to new backend
    if (type == BackendType::Cloud) {
        active_backend_ = cloud_backend_.get();
    } else {
        active_backend_ = local_backend_.get();
    }
    active_type_ = type;

    // Initialize the new backend
    if (!active_backend_->initialize()) {
        LOG_WARN("LLMController: New backend failed to initialize, falling back to Local");
        if (type == BackendType::Cloud) {
            active_backend_ = local_backend_.get();
            active_type_ = BackendType::Local;
            active_family_ = local_model_family_;
            active_backend_->initialize();
        }
    }

    LOG_INFO("LLMController: Backend switch complete. "
             "Conversation ({} messages) translated and preserved.",
             conversation_.size());
}

BackendType LLMController::getActiveBackend() const {
    return active_type_;
}

std::future<std::string> LLMController::generateResponseAsync(const std::string& prompt) {
    // FIX C1: Wait for any previous async work before launching new one
    if (async_future_.valid()) {
        try { async_future_.wait(); } catch (...) {}
    }
    // Store future so destructor can wait on it (prevents use-after-free)
    async_future_ = std::async(std::launch::async, [this, prompt]() {
        return generateResponse(prompt);
    });
    return std::async(std::launch::async, [this]() {
        if (async_future_.valid()) return async_future_.get();
        return std::string{};
    });
}

void LLMController::addUserMessage(const std::string& content) {
    std::lock_guard<std::recursive_mutex> lock(llm_mutex_);
    conversation_.emplace_back("user", content);
}

void LLMController::addAssistantMessage(const std::string& content) {
    std::lock_guard<std::recursive_mutex> lock(llm_mutex_);
    conversation_.emplace_back("assistant", content);
}

void LLMController::setSystemPrompt(const std::string& prompt) {
    std::lock_guard<std::recursive_mutex> lock(llm_mutex_);
    system_prompt_ = prompt;

    // Update or insert the system message at the front of conversation
    if (!conversation_.empty() && conversation_.front().role == "system") {
        conversation_.front().content = prompt;
    } else {
        conversation_.insert(conversation_.begin(), Message("system", prompt));
    }
}

void LLMController::clearConversation() {
    std::lock_guard<std::recursive_mutex> lock(llm_mutex_);
    conversation_.clear();

    // Re-add system prompt if one was set
    if (!system_prompt_.empty()) {
        conversation_.emplace_back("system", system_prompt_);
    }
}

// ── Cloud-specific configuration ─────────────────────────────────
void LLMController::setCloudApiKey(const std::string& key) {
    if (cloud_backend_) cloud_backend_->setApiKey(key);
}

void LLMController::setCloudModel(const std::string& model) {
    if (cloud_backend_) cloud_backend_->setModel(model);
    // Auto-detect model family for instruction translation
    cloud_model_family_ = InstructionTranslator::detectFamily(model);
    LOG_INFO("LLMController: Cloud model family detected: {}",
             static_cast<int>(cloud_model_family_));
}

void LLMController::setCloudEndpoint(const std::string& url) {
    if (cloud_backend_) cloud_backend_->setEndpoint(url);
}

// ═══════════════════ Model Info ═══════════════════════════════════

std::string LLMController::getModelInfo() const {
    if (!active_backend_) return "No backend available";

    std::ostringstream ss;
    ss << "[" << (active_type_ == BackendType::Local ? "LOCAL" : "CLOUD") << "] "
       << active_backend_->info()
       << " | conversation: " << conversation_.size() << " messages";
    return ss.str();
}

// ═══════════════════ Robust JSON Parsing + Anti-Hallucination ══════

json LLMController::parseJsonStrict(const std::string& text) {
    // Try direct parse
    try { return json::parse(text); }
    catch (...) {}

    // Try to extract JSON from text
    auto start = text.find('{');
    auto end = text.rfind('}');
    if (start != std::string::npos && end != std::string::npos && end > start) {
        try { return json::parse(text.substr(start, end - start + 1)); }
        catch (...) {}
    }

    // Try to extract from markdown code block
    std::regex json_block(R"(```(?:json)?\s*\n?(\{[\s\S]*?\})\s*\n?```)");
    std::smatch m;
    if (std::regex_search(text, m, json_block)) {
        try { return json::parse(m[1].str()); }
        catch (...) {}
    }

    // ── Anti-Hallucination Fallback ──
    LOG_WARN("LLM hallucinated plain text instead of JSON — wrapping as action");

    std::string trimmed = text;
    trimmed.erase(0, trimmed.find_first_not_of(" \t\r\n"));
    trimmed.erase(trimmed.find_last_not_of(" \t\r\n") + 1);

    if (trimmed.empty()) {
        return {{"thought", "No response from LLM"}, {"action", "task_complete"},
                {"params", {{"message", "Unable to process — no output"}}}};
    }

    return {{"thought", "LLM produced plain text instead of JSON — safely completing task"},
            {"action", "task_complete"},
            {"params", {{"message", "I am unable to execute this command. " + trimmed}}}};
}

json LLMController::validateAndFill(json parsed) {
    if (!parsed.contains("action")) parsed["action"] = "task_complete";
    if (!parsed.contains("params")) parsed["params"] = json::object();
    if (!parsed.contains("thought")) parsed["thought"] = "";
    return parsed;
}

std::string LLMController::formatHistory(const std::vector<json>& history) {
    std::ostringstream ss;
    for (size_t i = 0; i < history.size(); i++) {
        ss << (i + 1) << ". " << history[i].dump() << "\n";
    }
    return ss.str();
}

size_t LLMController::hashPrompt(const std::string& prompt) const {
    return std::hash<std::string>{}(prompt);
}

// FIX B5: Canonical cache key — strips volatile parts from ReAct prompts
// ReAct prompts contain timestamps, observations (active window title),
// and scratchpad (Thought/Action/Observation loops) that change every call.
// Only the semantic core (user goal + tool list) should form the cache key.
std::string LLMController::canonicalCacheKey(const std::string& prompt) const {
    std::string key = prompt;

    // 1. Strip timestamps (ISO-8601: 2026-03-07T01:06:07, Unix: 1741299967)
    //    Pattern: YYYY-MM-DDTHH:MM:SS or 10-digit numbers
    std::string result;
    result.reserve(key.size());
    for (size_t i = 0; i < key.size(); i++) {
        // Skip 10+ digit sequences (Unix timestamps)
        if (std::isdigit(key[i])) {
            size_t j = i;
            while (j < key.size() && std::isdigit(key[j])) j++;
            if (j - i >= 10) { i = j - 1; continue; }  // Skip the timestamp
        }
        // Skip ISO datetime patterns (YYYY-MM-DD)
        if (i + 9 < key.size() && std::isdigit(key[i]) &&
            std::isdigit(key[i+1]) && std::isdigit(key[i+2]) && std::isdigit(key[i+3]) &&
            key[i+4] == '-') {
            i += 9;  // Skip past YYYY-MM-DD
            if (i + 1 < key.size() && key[i+1] == 'T') i += 9;  // Skip THH:MM:SS too
            continue;
        }
        result += key[i];
    }

    // 2. Strip Observation/Scratchpad blocks ("Observation: ..." to next "Thought:" or end)
    //    These contain live window titles, process lists, etc.
    size_t obs_pos = result.find("Observation:");
    while (obs_pos != std::string::npos) {
        size_t next_thought = result.find("Thought:", obs_pos);
        if (next_thought != std::string::npos) {
            result.erase(obs_pos, next_thought - obs_pos);
        } else {
            result.erase(obs_pos);  // Erase to end
        }
        obs_pos = result.find("Observation:", obs_pos);
    }

    // 3. Strip "Active window: ..." lines
    size_t aw_pos = result.find("Active window:");
    while (aw_pos != std::string::npos) {
        size_t eol = result.find('\n', aw_pos);
        if (eol != std::string::npos) result.erase(aw_pos, eol - aw_pos + 1);
        else result.erase(aw_pos);
        aw_pos = result.find("Active window:", aw_pos);
    }

    return result;
}

// ═══════════════════ Response Cache ═══════════════════════════════

std::optional<std::string> LLMController::getCachedResponse(const std::string& prompt) {
    std::lock_guard lock(cache_mutex_);
    auto it = cache_.find(prompt);
    if (it != cache_.end()) {
        auto elapsed = std::chrono::steady_clock::now() - it->second.time;
        if (elapsed < std::chrono::seconds(cache_ttl_seconds_)) {
            return it->second.response;
        }
        cache_.erase(it);
    }
    return std::nullopt;
}

void LLMController::cacheResponse(const std::string& prompt, const std::string& response) {
    std::lock_guard lock(cache_mutex_);
    cache_[prompt] = {response, std::chrono::steady_clock::now()};

    // Limit cache size
    if (cache_.size() > 100) {
        auto oldest = cache_.begin();
        for (auto it = cache_.begin(); it != cache_.end(); ++it) {
            if (it->second.time < oldest->second.time) oldest = it;
        }
        cache_.erase(oldest);
    }
}

} // namespace vision
