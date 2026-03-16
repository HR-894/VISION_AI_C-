#pragma once
/**
 * @file local_backend.h
 * @brief Local AI Backend via llama.cpp
 *
 * Implements IAIBackend for local GGUF model inference using the llama.cpp
 * C API. Extracted from the original monolithic LLMController.
 * Handles model loading, KV cache shifting, token sampling, idle unload,
 * embeddings, and graceful VRAM/RAM cleanup.
 */

#include "ai_backend.h"
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <chrono>
#include <thread>
#include <random>

#ifdef VISION_HAS_LLM
struct llama_model;
struct llama_context;
#endif

namespace vision {

class LocalBackend : public IAIBackend {
public:
    /// @param model_path  Path to GGUF model file (empty = auto-detect)
    explicit LocalBackend(const std::string& model_path = "");
    ~LocalBackend() override;

    // Non-copyable
    LocalBackend(const LocalBackend&) = delete;
    LocalBackend& operator=(const LocalBackend&) = delete;

    // ── IAIBackend interface ─────────────────────────────────────
    bool initialize() override;
    void shutdown() override;
    bool isReady() const override;

    std::string generate(const std::string& prompt,
                         const std::vector<Message>& history) override;

    std::vector<float> getEmbeddings(const std::string& text) override;

    void cancelGeneration() override;
    BackendType type() const override { return BackendType::Local; }
    std::string info() const override;

    // ── Local-specific configuration ─────────────────────────────
    void setGPULayers(int layers) { gpu_layers_ = layers; }
    void setContextSize(int size) { context_size_ = size; }
    void setThreadCount(int count) { thread_count_ = count; }
    void setTemperature(float temp) { temperature_ = temp; }
    void setTopP(float p) { top_p_ = p; }
    void setMaxTokens(int tokens) { max_tokens_ = tokens; }

    /// Check idle timer and auto-unload if idle too long
    void checkIdleUnload();

    /// Reset the activity timer
    void touchActivity();

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
    int n_past_ = 0;  // PRD Fix 4: KV cache cursor for rolling window

    std::atomic<bool> cancel_generation_{false};
    mutable std::recursive_mutex llm_mutex_;

    // ── Idle auto-unload ─────────────────────────────────────────
    std::chrono::steady_clock::time_point last_activity_;
    static constexpr int IDLE_TIMEOUT_SECONDS = 300;  // 5 minutes

    // ── Internal helpers ─────────────────────────────────────────
    std::string findModelPath() const;
    std::string buildFullPrompt(const std::string& prompt,
                                const std::vector<Message>& history) const;
};

} // namespace vision
