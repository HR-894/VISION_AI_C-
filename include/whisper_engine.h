#pragma once
/**
 * @file whisper_engine.h
 * @brief Speech-to-text using whisper.cpp — streaming background worker
 * 
 * Loads a whisper model and provides both batch transcription and
 * a continuous background worker that polls AudioCapture for live
 * partial transcriptions. Emits Qt signals for real-time UI updates.
 */

#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <functional>

#ifdef VISION_HAS_WHISPER
struct whisper_context;
#endif

namespace vision {

class AudioCapture;  // Forward declaration

class WhisperEngine {
public:
    /// Construct with model size ("tiny", "base", "small", "medium")
    explicit WhisperEngine(const std::string& model_size = "base");
    ~WhisperEngine();

    // Non-copyable
    WhisperEngine(const WhisperEngine&) = delete;
    WhisperEngine& operator=(const WhisperEngine&) = delete;

    /// Load the whisper model (lazy load on first use)
    bool loadModel();

    /// Set explicit model path (PRD Fix 2: from Settings UI)
    void setModelPath(const std::string& path);

    /// Unload model to free memory
    void unloadModel();

    /// Check if model is loaded
    bool isLoaded() const;

    /// Check if whisper is available (compiled in)
    static bool isAvailable();

    /// Transcribe audio samples (float32, 16kHz mono) to text — batch/blocking
    std::string transcribe(const std::vector<float>& audio);

    /// Fast partial transcribe — lower accuracy, faster speed for live preview
    std::string transcribeFast(const std::vector<float>& audio);

    /// Clean up common voice recognition artifacts
    static std::string cleanVoiceText(const std::string& text);

    /// Get model info string
    std::string getModelInfo() const;

    // ── Live Streaming API ───────────────────────────────────────

    /// Callback type for partial transcription results
    using PartialCallback = std::function<void(const std::string& partial_text)>;

    /// Start the background polling worker — polls audio_capture for chunks
    /// @param audio_capture  Pointer to the active AudioCapture instance
    /// @param on_partial     Callback invoked on the worker thread with partial text
    void startListening(AudioCapture* audio_capture, PartialCallback on_partial);

    /// Stop the background polling worker
    void stopListening();

    /// Check if the background worker is running
    bool isListening() const { return listening_.load(); }

private:
#ifdef VISION_HAS_WHISPER
    whisper_context* ctx_ = nullptr;
#endif
    std::string model_size_;
    std::string model_path_;
    bool loaded_ = false;

    // ── Background worker state ──────────────────────────────────
    std::atomic<bool> listening_{false};
    std::thread worker_thread_;

    /// Worker loop: polls AudioCapture, runs fast whisper, fires callback
    void workerLoop(AudioCapture* audio_capture, PartialCallback on_partial);

    /// Find model on disk
    std::string findModelPath() const;
};

} // namespace vision
