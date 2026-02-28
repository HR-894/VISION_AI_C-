#pragma once
/**
 * @file llm_controller.h
 * @brief LLM inference via llama.cpp C API
 * 
 * Handles ReAct reasoning steps, one-shot command parsing,
 * response caching, and robust JSON parsing from small models.
 */

#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <chrono>
#include <mutex>
#include <random>
#include <nlohmann/json.hpp>

#ifdef VISION_HAS_LLM
struct llama_model;
struct llama_context;
#endif

namespace vision {

class LLMController {
public:
    explicit LLMController(const std::string& model_path = "");
    ~LLMController();

    // Non-copyable
    LLMController(const LLMController&) = delete;
    LLMController& operator=(const LLMController&) = delete;

    /// Check if llama.cpp is available (compiled in)
    static bool isAvailable();

    /// Load GGUF model (lazy)
    bool loadModel();

    /// Unload model to free VRAM/RAM
    void unloadModel();

    /// Check if model is loaded
    bool isModelLoaded() const;

    /// Single ReAct iteration: given task, observation, history → next action
    std::optional<nlohmann::json> reactStep(
        const std::string& task,
        const nlohmann::json& observation,
        const std::vector<nlohmann::json>& history);

    /// Legacy one-shot command parsing (fallback)
    std::optional<nlohmann::json> parseAmbiguousCommand(
        const std::string& command,
        const std::string& context = "");

    /// Generate raw response for the ReAct prompt
    std::string generateReactResponse(const std::string& prompt);

    /// Get model info string
    std::string getModelInfo() const;

    /// Set GPU layers (for dynamic adjustment)
    void setGPULayers(int layers) { gpu_layers_ = layers; }

    /// Set context size
    void setContextSize(int size) { context_size_ = size; }

private:
#ifdef VISION_HAS_LLM
    llama_model* model_ = nullptr;
    llama_context* ctx_ = nullptr;
#endif
    std::string model_path_;
    bool loaded_ = false;
    int gpu_layers_ = 0;
    int context_size_ = 2048;
    float temperature_ = 0.1f;
    float top_p_ = 0.9f;
    int max_tokens_ = 512;
    int timeout_seconds_ = 30;

    // Response cache
    struct CacheEntry {
        std::string response;
        std::chrono::steady_clock::time_point time;
    };
    std::unordered_map<std::string, CacheEntry> cache_;  // Full prompt as key (no hash collisions)
    mutable std::mutex cache_mutex_;
    int cache_ttl_seconds_ = 60;

    // ── Internal helpers ─────────────────────────────────────────
    std::string generateResponse(const std::string& prompt);
    nlohmann::json parseJsonStrict(const std::string& text);
    nlohmann::json validateAndFill(nlohmann::json parsed);
    std::string formatHistory(const std::vector<nlohmann::json>& history);
    size_t hashPrompt(const std::string& prompt) const;
    std::string findModelPath() const;

    // ── Cache helpers ────────────────────────────────────────────
    std::optional<std::string> getCachedResponse(const std::string& prompt);
    void cacheResponse(const std::string& prompt, const std::string& response);
};

} // namespace vision
