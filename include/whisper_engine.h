#pragma once
/**
 * @file whisper_engine.h
 * @brief Speech-to-text using whisper.cpp
 * 
 * Loads a whisper model and transcribes audio buffers to text.
 * Includes voice text cleanup for common recognition artifacts.
 */

#include <string>
#include <vector>
#include <memory>

#ifdef VISION_HAS_WHISPER
struct whisper_context;
#endif

namespace vision {

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

    /// Transcribe audio samples (float32, 16kHz mono) to text
    std::string transcribe(const std::vector<float>& audio);

    /// Clean up common voice recognition artifacts
    static std::string cleanVoiceText(const std::string& text);

    /// Get model info string
    std::string getModelInfo() const;

private:
#ifdef VISION_HAS_WHISPER
    whisper_context* ctx_ = nullptr;
#endif
    std::string model_size_;
    std::string model_path_;
    bool loaded_ = false;

    std::string findModelPath() const;
};

} // namespace vision
