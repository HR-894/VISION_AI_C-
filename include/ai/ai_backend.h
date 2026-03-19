#pragma once
/**
 * @file ai_backend.h
 * @brief Abstract AI Backend Interface for Dual-Inference Engine
 *
 * Defines the IAIBackend interface that both CloudBackend (Groq REST)
 * and LocalBackend (llama.cpp) implement. Also defines shared types:
 * Message (conversation entry) and BackendType enum.
 */

#include <string>
#include <vector>
#include <memory>
#include <functional>

namespace vision {

// ═══════════════════ Shared Types ═══════════════════════════════════

/// A single message in the conversation history
struct Message {
    std::string role;     // "system", "user", or "assistant"
    std::string content;  // The message text

    Message() = default;
    Message(std::string r, std::string c)
        : role(std::move(r)), content(std::move(c)) {}
};

/// Which inference backend to use
enum class BackendType {
    Local,   // llama.cpp (offline, GGUF models)
    Cloud    // Groq REST API (online, fast cloud inference)
};

// ═══════════════════ Abstract Interface ═════════════════════════════

/**
 * @class IAIBackend
 * @brief Pure virtual interface for AI inference backends
 *
 * Both CloudBackend and LocalBackend implement this interface so
 * LLMController can switch between them transparently.
 */
class IAIBackend {
public:
    virtual ~IAIBackend() = default;

    // ── Lifecycle ────────────────────────────────────────────────

    /// Initialize the backend (load model / init curl handle)
    /// @return true if ready to accept generate() calls
    virtual bool initialize() = 0;

    /// Shutdown and release all resources (VRAM, RAM, handles)
    /// Must be safe to call multiple times (idempotent)
    virtual void shutdown() = 0;

    /// @return true if the backend is initialized and ready
    virtual bool isReady() const = 0;

    // ── Inference ────────────────────────────────────────────────

    /// Generate a response given a prompt and conversation history
    /// @param prompt   The current user message / formatted prompt
    /// @param history  Full conversation history for context reconstruction
    /// @param stream_cb Optional callback invoked when a new text chunk is generated
    /// @return Generated text response (empty string on failure)
    using StreamCallback = std::function<void(const std::string&)>;
    virtual std::string generate(const std::string& prompt,
                                 const std::vector<Message>& history,
                                 StreamCallback stream_cb = nullptr) = 0;

    /// Get text embeddings (lightweight vector for semantic memory)
    /// @return Embedding vector (may be empty if not supported)
    virtual std::vector<float> getEmbeddings(const std::string& text) = 0;

    // ── Control ──────────────────────────────────────────────────

    /// Cancel any ongoing generation immediately
    virtual void cancelGeneration() = 0;

    // ── Info ─────────────────────────────────────────────────────

    /// @return The backend type enum
    virtual BackendType type() const = 0;

    /// @return Human-readable info string about this backend
    virtual std::string info() const = 0;
};

} // namespace vision
