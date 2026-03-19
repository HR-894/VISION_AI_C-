#pragma once
/**
 * @file llm_controller.h
 * @brief Dual-Inference Engine Orchestrator
 *
 * Manages two AI backends: CloudBackend (Groq REST API via libcurl)
 * and LocalBackend (llama.cpp GGUF). Handles backend switching with
 * context persistence (std::vector<Message>), async execution
 * (std::future), response caching, JSON parsing, and memory-safe
 * cleanup on backend transitions.
 *
 * PUBLIC API IS BACKWARD COMPATIBLE — all existing consumers
 * (ReActAgent, AgentMemory, VisionAI) work without changes.
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
#include <future>
#include <memory>
#include <nlohmann/json.hpp>

#include "ai_backend.h"
#include "instruction_translator.h"

// Forward declarations for backend classes
namespace vision {
class LocalBackend;
class CloudBackend;
}

namespace vision {

class LLMController {
public:
    explicit LLMController(const std::string& model_path = "");
    ~LLMController();

    // Non-copyable
    LLMController(const LLMController&) = delete;
    LLMController& operator=(const LLMController&) = delete;

    // ═══════════════════ Original Public API (unchanged) ═══════════

    /// Check if llama.cpp is available (compiled in)
    static bool isAvailable();

    /// Load model on active backend (lazy)
    bool loadModel();

    /// Unload model to free VRAM/RAM
    void unloadModel();

    /// Check if active backend is loaded/ready
    bool isModelLoaded() const;

    using StreamCallback = std::function<void(const std::string&)>;

    /// Single ReAct iteration: given task, observation, history    // ── ReAct orchestration ──────────────────────────────────────
    std::optional<nlohmann::json> reactStep(
        const std::string& task,
        const nlohmann::json& observation,
        const std::vector<nlohmann::json>& history,
        StreamCallback stream_cb = nullptr);

    /// Legacy one-shot command parsing (fallback)
    std::optional<nlohmann::json> parseAmbiguousCommand(
        const std::string& command,
        const std::string& context = "");

    /// Generate raw response for the ReAct prompt
    std::string generateReactResponse(const std::string& prompt, StreamCallback stream_cb = nullptr);

    /// Get text embeddings (always uses LocalBackend)
    std::vector<float> getEmbeddings(const std::string& text);

    /// Get model/backend info string
    std::string getModelInfo() const;

    /// Set GPU layers (for local backend)
    void setGPULayers(int layers);

    /// Set context size (for local backend)
    void setContextSize(int size);

    /// Set thread count (for local backend)
    void setThreadCount(int count);

    /// Generate a single text response (main inference entry point)
    std::string generateResponse(const std::string& prompt, StreamCallback stream_cb = nullptr);

    // ── Emergency Stop ──────────────────────────────────────────
    /// Cancel any ongoing generation immediately
    void cancelGeneration();

    /// Reset cancel flag
    void resetCancel();

    /// Check idle timer and auto-unload (local backend only)
    void checkIdleUnload();

    /// Enable/disable automatic backend failover
    void enableFailover(bool enabled) { failover_enabled_ = enabled; }
    bool isFailoverEnabled() const { return failover_enabled_; }
    int getFailoverCount() const { return failover_count_; }

    // ═══════════════════ NEW Dual-Engine API ═══════════════════════

    /// Switch the active inference backend
    /// Safely shuts down the old backend (frees VRAM/RAM) before initializing new one
    void setBackend(BackendType type);

    /// Get the currently active backend type
    BackendType getActiveBackend() const;

    /// Async generation — returns a future that doesn't block the caller
    std::future<std::string> generateResponseAsync(const std::string& prompt, StreamCallback stream_cb = nullptr);

    /// Add a user message to the persistent conversation
    void addUserMessage(const std::string& content);

    /// Add an assistant message to the persistent conversation
    void addAssistantMessage(const std::string& content);

    /// Set the system prompt (replaces any existing system message)
    void setSystemPrompt(const std::string& prompt);

    /// Dynamic Parameters
    void setTemperature(float temp);
    void setTopP(float top_p);

    /// Clear the entire conversation history
    void clearConversation();

    /// Get the current conversation history (thread-safe copy)
    std::vector<Message> getConversation() const {
        std::lock_guard<std::recursive_mutex> lock(llm_mutex_);
        return conversation_;
    }

    // ── Cloud-specific configuration ─────────────────────────────
    void setCloudApiKey(const std::string& key);
    void setCloudModel(const std::string& model);
    void setCloudEndpoint(const std::string& url);

private:
    // ── Dual backends ────────────────────────────────────────────
    std::unique_ptr<LocalBackend> local_backend_;
    std::unique_ptr<CloudBackend> cloud_backend_;
    IAIBackend* active_backend_ = nullptr;  // Points to one of the above
    BackendType active_type_ = BackendType::Local;

    // ── Instruction Translator ───────────────────────────────────
    InstructionTranslator translator_;
    ModelFamily local_model_family_ = ModelFamily::Qwen;    // Default: Qwen GGUF
    ModelFamily cloud_model_family_ = ModelFamily::Llama3;  // Default: Groq Llama-3
    ModelFamily active_family_ = ModelFamily::Qwen;

    // ── Context persistence ──────────────────────────────────────
    std::vector<Message> conversation_;
    std::string system_prompt_;
    
    // ── Generation parameters ────────────────────────────────────
    float temperature_ = 0.7f;
    float top_p_ = 0.9f;

    // ── Thread safety ────────────────────────────────────────────
    mutable std::recursive_mutex llm_mutex_;

    // ── Response cache ───────────────────────────────────────────
    struct CacheEntry {
        std::string response;
        std::chrono::steady_clock::time_point time;
    };
    std::unordered_map<std::string, CacheEntry> cache_;
    mutable std::mutex cache_mutex_;
    int cache_ttl_seconds_ = 60;

    // ── Async generation tracking ────────────────────────────────
    std::shared_future<std::string> async_future_;  // Track outstanding async work

    // ── Internal helpers (shared across backends) ────────────────
    nlohmann::json parseJsonStrict(const std::string& text);
    nlohmann::json validateAndFill(nlohmann::json parsed);
    std::string formatHistory(const std::vector<nlohmann::json>& history);
    size_t hashPrompt(const std::string& prompt) const;
    std::string canonicalCacheKey(const std::string& prompt) const;  // B5: semantic cache
    std::string tryFailover(const std::string& prompt);              // Auto-failover
    void pruneConversation();                                         // Session pruning

    // ── Failover state ──────────────────────────────────────────
    bool failover_enabled_ = true;     // Auto-switch on backend failure
    int failover_count_ = 0;           // Number of failovers in this session
    
    // ── Session pruning ─────────────────────────────────────────
    int max_conversation_messages_ = 20; // Auto-prune beyond this

    // ── Cache helpers ───────────────────────────────────────────
    std::optional<std::string> getCachedResponse(const std::string& prompt);
    void cacheResponse(const std::string& prompt, const std::string& response);
};

} // namespace vision
