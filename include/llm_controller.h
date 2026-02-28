#pragma once
/**
 * @file llm_controller.h
 * @brief LLM inference via llama.cpp C API
 *
 * Handles ReAct reasoning steps, one-shot command parsing,
 * response caching, robust JSON parsing, embeddings, idle auto-unload,
 * KV cache shifting, and emergency generation cancellation.
 */

#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <chrono>
#include <mutex>
#include <thread>
#include <random>
#include <atomic>
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

    /// Get text embeddings (lightweight vector memory)
    std::vector<float> getEmbeddings(const std::string& text);

    /// Get model info string
    std::string getModelInfo() const;

    /// Set GPU layers (for dynamic adjustment)
    void setGPULayers(int layers) { gpu_layers_ = layers; }

    /// Set context size
    void setContextSize(int size) { context_size_ = size; }

    /// Set thread count
    void setThreadCount(int count) { thread_count_ = count; }

    // ── Emergency Stop ──────────────────────────────────────────
    /// Cancel any ongoing generation immediately
    void cancelGeneration() { cancel_generation_ = true; }

    /// Reset cancel flag (called after cancel is handled)
    void resetCancel() { cancel_generation_ = false; }

    /// Check idle timer and auto-unload if idle too long
    void checkIdleUnload();

private:
#ifdef VISION_HAS_LLM
    llama_model* model_ = nullptr;
    llama_context* ctx_ = nullptr;
#endif
    std::string model_path_;
    bool loaded_ = false;
    int gpu_layers_ = 0;
    int context_size_ = 2048;
    int thread_count_ = std::max(1, (int)std::thread::hardware_concurrency() / 2);
    float temperature_ = 0.1f;
    float top_p_ = 0.9f;
    int max_tokens_ = 512;
    int timeout_seconds_ = 30;

    // ── Emergency cancel ────────────────────────────────────────
    std::atomic<bool> cancel_generation_{false};

    // ── Idle auto-unload ────────────────────────────────────────
    std::chrono::steady_clock::time_point last_activity_;
    static constexpr int IDLE_TIMEOUT_SECONDS = 300;  // 5 minutes

    // ── Response cache ──────────────────────────────────────────
    struct CacheEntry {
        std::string response;
        std::chrono::steady_clock::time_point time;
    };
    std::unordered_map<std::string, CacheEntry> cache_;
    mutable std::mutex cache_mutex_;
    int cache_ttl_seconds_ = 60;

    // ── Internal helpers ────────────────────────────────────────
    std::string generateResponse(const std::string& prompt);
    nlohmann::json parseJsonStrict(const std::string& text);
    nlohmann::json validateAndFill(nlohmann::json parsed);
    std::string formatHistory(const std::vector<nlohmann::json>& history);
    size_t hashPrompt(const std::string& prompt) const;
    std::string findModelPath() const;
    void touchActivity();

    // ── Cache helpers ───────────────────────────────────────────
    std::optional<std::string> getCachedResponse(const std::string& prompt);
    void cacheResponse(const std::string& prompt, const std::string& response);
};

} // namespace vision
