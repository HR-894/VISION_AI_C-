/**
 * @file whisper_engine.cpp
 * @brief whisper.cpp speech-to-text — batch + live streaming worker
 */

#include "whisper_engine.h"
#include "audio_capture.h"
#include <filesystem>
#include <algorithm>
#include <regex>
#include <chrono>

#ifdef VISION_HAS_WHISPER
#include "whisper.h"
#endif

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

namespace vision {

WhisperEngine::WhisperEngine(const std::string& model_size)
    : model_size_(model_size) {
    model_path_ = findModelPath();
}

WhisperEngine::~WhisperEngine() {
    stopListening();
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
    std::vector<std::string> candidates = {
        "models/ggml-base.bin",
        "models/ggml-base.en.bin",
        "models/ggml-small.bin",
        "models/ggml-" + model_size_ + ".bin",
        "models/ggml-large-v3.bin",
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

// ═══════════════════ Batch Transcription (High Accuracy) ══════════

std::string WhisperEngine::transcribe(const std::vector<float>& audio) {
#ifdef VISION_HAS_WHISPER
    if (!loaded_ && !loadModel()) return "";
    
    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.n_threads = std::max(1, (int)std::thread::hardware_concurrency() / 2);
    wparams.print_progress = false;
    wparams.print_special = false;
    wparams.print_realtime = false;
    wparams.print_timestamps = false;
    wparams.translate = false;
    wparams.language = "auto";
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
    LOG_INFO("Transcribed (final): {}", result);
    return result;
#else
    (void)audio;
    return "";
#endif
}

// ═══════════════════ Fast Partial Transcription ═══════════════════

std::string WhisperEngine::transcribeFast(const std::vector<float>& audio) {
#ifdef VISION_HAS_WHISPER
    if (!loaded_ && !loadModel()) return "";
    if (audio.empty()) return "";

    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    // Use fewer threads and aggressive settings for speed
    wparams.n_threads = std::max(1, (int)std::thread::hardware_concurrency() / 4);
    wparams.print_progress = false;
    wparams.print_special = false;
    wparams.print_realtime = false;
    wparams.print_timestamps = false;
    wparams.translate = false;
    wparams.language = "en";  // Force English for speed on partials
    wparams.single_segment = true;
    wparams.no_context = true;  // Don't use previous context for partials

    if (whisper_full(ctx_, wparams, audio.data(), (int)audio.size()) != 0) {
        return "";  // Silent failure for partials
    }

    std::string result;
    int n_segments = whisper_full_n_segments(ctx_);
    for (int i = 0; i < n_segments; i++) {
        result += whisper_full_get_segment_text(ctx_, i);
    }

    // Light cleanup only (no lowercasing — that's for final pass)
    result.erase(0, result.find_first_not_of(" \t\n\r"));
    result.erase(result.find_last_not_of(" \t\n\r") + 1);

    return result;
#else
    (void)audio;
    return "";
#endif
}

// ═══════════════════ Live Streaming Worker ════════════════════════

void WhisperEngine::startListening(AudioCapture* audio_capture, PartialCallback on_partial) {
    if (listening_.load()) return;  // Already running
    if (!audio_capture) return;

    listening_ = true;
    worker_thread_ = std::thread([this, audio_capture, on_partial]() {
        workerLoop(audio_capture, on_partial);
    });
    LOG_INFO("WhisperEngine: Live streaming worker started");
}

void WhisperEngine::stopListening() {
    listening_ = false;
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    LOG_INFO("WhisperEngine: Live streaming worker stopped");
}

void WhisperEngine::workerLoop(AudioCapture* audio_capture, PartialCallback on_partial) {
    // Poll every 800ms for new audio chunks
    constexpr int POLL_INTERVAL_MS = 800;
    constexpr int CHUNK_DURATION_MS = 2000;  // Grab the last 2 seconds

    std::string last_partial;

    while (listening_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));

        if (!listening_.load()) break;
        if (!audio_capture->isRecording()) continue;

        // Only transcribe if VAD detects voice
        if (!audio_capture->isVoiceActive()) continue;

        // Grab the latest 2 seconds of audio
        auto chunk = audio_capture->getLatestAudio(CHUNK_DURATION_MS);
        if (chunk.empty() || chunk.size() < 1600) continue;  // Need at least 100ms of audio

        // Run fast transcription
        std::string partial = transcribeFast(chunk);
        if (partial.empty()) continue;

        // Only fire callback if text changed (avoid flickering)
        if (partial != last_partial) {
            last_partial = partial;
            if (on_partial) {
                on_partial(partial);
            }
        }
    }
}

// ═══════════════════ Text Cleanup ═════════════════════════════════

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
