/**
 * @file whisper_engine.cpp
 * @brief whisper.cpp speech-to-text integration
 */

#include "whisper_engine.h"
#include <filesystem>
#include <algorithm>
#include <regex>

#ifdef VISION_HAS_WHISPER
#include "whisper.h"
#endif

#ifdef VISION_HAS_SPDLOG
#include <spdlog/spdlog.h>
#define LOG_INFO(...) spdlog::info(__VA_ARGS__)
#define LOG_ERROR(...) spdlog::error(__VA_ARGS__)
#else
#define LOG_INFO(...) (void)0
#define LOG_ERROR(...) (void)0
#endif

namespace fs = std::filesystem;

namespace vision {

WhisperEngine::WhisperEngine(const std::string& model_size)
    : model_size_(model_size) {
    model_path_ = findModelPath();
}

WhisperEngine::~WhisperEngine() {
    unloadModel();
}

bool WhisperEngine::isAvailable() {
#ifdef VISION_HAS_WHISPER
    return true;
#else
    return false;
#endif
}

std::string WhisperEngine::findModelPath() const {
    // Search common locations — prefer smaller models for fast voice commands
    // Order: base (142MB, ~1s) > small (466MB) > medium > large-v3 (3GB, slow)
    std::vector<std::string> candidates = {
        "models/ggml-base.bin",
        "models/ggml-base.en.bin",
        "models/ggml-small.bin",
        "models/ggml-" + model_size_ + ".bin",
        "models/ggml-large-v3.bin",
        // PRD Fix 2: Removed hardcoded M:/AI/MODELS/VOICE/ developer paths
        "data/models/ggml-base.bin",
        "data/models/ggml-" + model_size_ + ".bin",
        "../models/ggml-base.bin",
        "../models/ggml-" + model_size_ + ".bin",
    };
    for (const auto& path : candidates) {
        if (fs::exists(path)) {
            LOG_INFO("Whisper model found: {}", path);
            return path;
        }
    }
    // Fallback — will fail gracefully in loadModel() if not found
    return "models/ggml-base.bin";
}

bool WhisperEngine::loadModel() {
#ifdef VISION_HAS_WHISPER
    if (loaded_) return true;
    
    if (!fs::exists(model_path_)) {
        LOG_ERROR("Whisper model not found: {}", model_path_);
        return false;
    }
    
    struct whisper_context_params params = whisper_context_default_params();
    ctx_ = whisper_init_from_file_with_params(model_path_.c_str(), params);
    
    if (!ctx_) {
        LOG_ERROR("Failed to load whisper model: {}", model_path_);
        return false;
    }
    
    loaded_ = true;
    LOG_INFO("Whisper model loaded: {} ({})", model_size_, model_path_);
    return true;
#else
    return false;
#endif
}

void WhisperEngine::setModelPath(const std::string& path) {
    if (!path.empty()) {
        model_path_ = path;
        LOG_INFO("Whisper model path set to: {}", path);
    }
}

void WhisperEngine::unloadModel() {
#ifdef VISION_HAS_WHISPER
    if (ctx_) {
        whisper_free(ctx_);
        ctx_ = nullptr;
    }
    loaded_ = false;
#endif
}

bool WhisperEngine::isLoaded() const { return loaded_; }

std::string WhisperEngine::transcribe(const std::vector<float>& audio) {
#ifdef VISION_HAS_WHISPER
    if (!loaded_ && !loadModel()) return "";
    
    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.n_threads = std::max(1, (int)std::thread::hardware_concurrency() / 2);  // Use half cores to leave room for LLM
    wparams.print_progress = false;
    wparams.print_special = false;
    wparams.print_realtime = false;
    wparams.print_timestamps = false;
    wparams.translate = false;
    wparams.language = "auto";  // Req 3: Auto-detect language instead of forcing English
    wparams.single_segment = true;
    
    if (whisper_full(ctx_, wparams, audio.data(), (int)audio.size()) != 0) {
        LOG_ERROR("Whisper transcription failed");
        return "";
    }
    
    std::string result;
    int n_segments = whisper_full_n_segments(ctx_);
    for (int i = 0; i < n_segments; i++) {
        result += whisper_full_get_segment_text(ctx_, i);
    }
    
    result = cleanVoiceText(result);
    LOG_INFO("Transcribed: {}", result);
    return result;
#else
    (void)audio;
    return "";
#endif
}

std::string WhisperEngine::cleanVoiceText(const std::string& text) {
    std::string cleaned = text;
    
    // Trim whitespace
    cleaned.erase(0, cleaned.find_first_not_of(" \t\n\r"));
    cleaned.erase(cleaned.find_last_not_of(" \t\n\r") + 1);
    
    // Remove common whisper artifacts
    static const std::vector<std::string> artifacts = {
        "[BLANK_AUDIO]", "(inaudible)", "[silence]",
        "(music)", "[Music]", "(noise)", "[noise]",
    };
    for (const auto& art : artifacts) {
        size_t pos;
        while ((pos = cleaned.find(art)) != std::string::npos) {
            cleaned.erase(pos, art.size());
        }
    }
    
    // Remove leading/trailing punctuation artifacts
    while (!cleaned.empty() && (cleaned.front() == '.' || cleaned.front() == ',')) {
        cleaned.erase(0, 1);
    }
    
    // Trim again
    cleaned.erase(0, cleaned.find_first_not_of(" \t"));
    cleaned.erase(cleaned.find_last_not_of(" \t") + 1);
    
    // Lowercase for command processing
    std::transform(cleaned.begin(), cleaned.end(), cleaned.begin(), ::tolower);
    
    return cleaned;
}

std::string WhisperEngine::getModelInfo() const {
    return "Whisper " + model_size_ + (loaded_ ? " (loaded)" : " (not loaded)");
}

} // namespace vision
