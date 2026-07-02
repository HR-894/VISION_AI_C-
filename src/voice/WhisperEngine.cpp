// =============================================================================
// VISION AI - WhisperEngine.cpp
// Non-blocking whisper.cpp streaming transcription
// Sliding window + VAD-gated processing + partial/final result emission (No Qt)
// =============================================================================
#include "WhisperEngine.h"
#include "AudioCapture.h"

#include <spdlog/spdlog.h>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <format>
#include <thread>

// whisper.cpp (conditional)
#ifndef VISION_NO_WHISPER
#include <whisper.h>
#endif

namespace vision::voice {

// =========================================================================
// WhisperEngine: Construction / Destruction
// =========================================================================

WhisperEngine::WhisperEngine()
{
}

WhisperEngine::~WhisperEngine()
{
    stopStreaming();
    unloadModel();
    if (m_asyncTask.valid()) {
        m_asyncTask.wait();
    }
}

// =========================================================================
// Model Management
// =========================================================================

bool WhisperEngine::loadModel(const WhisperConfig& config)
{
#ifdef VISION_NO_WHISPER
    spdlog::warn("[WhisperEngine] whisper.cpp not available.");
    notifyError("whisper.cpp not compiled in.");
    return false;
#else
    std::lock_guard<std::mutex> lock(m_ctxMutex);

    if (m_ctx) {
        whisper_free(m_ctx);
        m_ctx = nullptr;
    }

    m_config = config;

    spdlog::info("[WhisperEngine] Loading model: {}", config.modelPath);

    struct whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = config.useGPU;

    m_ctx = whisper_init_from_file_with_params(
        config.modelPath.c_str(), cparams);

    if (!m_ctx) {
        spdlog::critical("[WhisperEngine] Failed to load whisper model: {}", config.modelPath);
        notifyError("Failed to load whisper model.");
        return false;
    }

    m_modelLoaded.store(true, std::memory_order_release);

    spdlog::info("[WhisperEngine] Model loaded successfully.");
    if (onModelLoaded) {
        onModelLoaded(config.modelPath);
    }

    return true;
#endif
}

void WhisperEngine::unloadModel()
{
#ifndef VISION_NO_WHISPER
    stopStreaming();

    std::lock_guard<std::mutex> lock(m_ctxMutex);

    if (m_ctx) {
        whisper_free(m_ctx);
        m_ctx = nullptr;
    }

    m_modelLoaded.store(false, std::memory_order_release);
    if (onModelUnloaded) {
        onModelUnloaded();
    }

    spdlog::info("[WhisperEngine] Model unloaded.");
#endif
}

bool WhisperEngine::isModelLoaded() const noexcept
{
    return m_modelLoaded.load(std::memory_order_acquire);
}

// =========================================================================
// One-Shot Transcription
// =========================================================================

TranscriptionResult WhisperEngine::transcribe(
    const std::vector<float>& audioData,
    bool highAccuracy)
{
    TranscriptionResult result;
    result.isPartial = false;

#ifdef VISION_NO_WHISPER
    result.text = "[Error] whisper.cpp not available.";
    return result;
#else
    std::lock_guard<std::mutex> lock(m_ctxMutex);

    if (!m_ctx) {
        result.text = "[Error] No whisper model loaded.";
        return result;
    }

    if (audioData.empty()) {
        result.text = "";
        return result;
    }

    auto start_time = std::chrono::steady_clock::now();

    struct whisper_full_params wparams =
        whisper_full_default_params(WHISPER_SAMPLING_GREEDY);

    wparams.n_threads       = m_config.threads;
    wparams.language        = m_config.language.c_str();
    wparams.translate       = m_config.translate;
    wparams.no_timestamps   = m_config.noTimestamps;
    wparams.single_segment  = m_config.singleSegment;
    wparams.no_context      = true;  
    wparams.suppress_blank  = true;
    wparams.suppress_nst    = true;

    if (highAccuracy) {
        wparams.strategy    = WHISPER_SAMPLING_BEAM_SEARCH;
        wparams.beam_search.beam_size = std::max(3, m_config.beamSize);
        wparams.greedy.best_of = std::max(3, m_config.bestOf);
    } else {
        wparams.greedy.best_of = m_config.bestOf;
    }

    m_abortRequested.store(false, std::memory_order_release);
    wparams.abort_callback = [](void* userData) -> bool {
        auto* self = static_cast<WhisperEngine*>(userData);
        return self->m_abortRequested.load(std::memory_order_acquire);
    };
    wparams.abort_callback_user_data = this;

    int rc = whisper_full(m_ctx, wparams,
                           audioData.data(),
                           static_cast<int>(audioData.size()));

    auto end_time = std::chrono::steady_clock::now();
    result.latencyMs = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    result.audioChunkMs = static_cast<int>(audioData.size() * 1000 / SAMPLE_RATE);

    if (rc != 0) {
        spdlog::warn("[WhisperEngine] whisper_full failed with rc: {}", rc);
        result.text = "";
        return result;
    }

    int nSegments = whisper_full_n_segments(m_ctx);
    std::string fullText;
    float totalProb = 0.0f;
    int probCount = 0;

    for (int i = 0; i < nSegments; ++i) {
        const char* segText = whisper_full_get_segment_text(m_ctx, i);
        if (segText) {
            fullText += segText;
        }

        int nTokens = whisper_full_n_tokens(m_ctx, i);
        for (int t = 0; t < nTokens; ++t) {
            float prob = whisper_full_get_token_p(m_ctx, i, t);
            if (prob > 0.0f) {
                totalProb += prob;
                probCount++;
            }
        }
    }

    while (!fullText.empty() && fullText.front() == ' ') fullText.erase(0, 1);
    while (!fullText.empty() && fullText.back() == ' ')  fullText.pop_back();

    static const std::vector<std::string> hallucinations = {
        "[BLANK_AUDIO]", "(blank audio)", "[ Silence ]",
        "[silence]", "(silence)", "...", "[Music]", "(music)",
        "Thank you for watching", "Thanks for watching",
        "Subscribe", "Like and subscribe",
    };

    for (const auto& h : hallucinations) {
        if (fullText.find(h) != std::string::npos && fullText.size() < h.size() + 10) {
            fullText.clear();
            break;
        }
    }

    result.text = fullText;
    result.confidence = (probCount > 0) ? (totalProb / probCount) : 0.0f;

    spdlog::debug("[WhisperEngine] Transcribed: {} chars | Confidence: {} | Latency: {}ms | Audio: {}ms",
             result.text.size(), result.confidence, result.latencyMs, result.audioChunkMs);

    return result;
#endif
}

void WhisperEngine::transcribeAsync(
    const std::vector<float>& audioData,
    bool highAccuracy)
{
    if (m_asyncTask.valid()) {
        m_asyncTask.wait();
    }

    m_asyncTask = std::async(std::launch::async, [this, audioData, highAccuracy]() {
        auto result = transcribe(audioData, highAccuracy);

        if (onTranscriptionResult) {
            onTranscriptionResult(result);
        }

        if (result.isPartial) {
            if (onPartialTranscription) onPartialTranscription(result.text);
        } else {
            if (onFinalTranscription) onFinalTranscription(result.text, result.confidence);
        }
    });
}

// =========================================================================
// Streaming Control
// =========================================================================

void WhisperEngine::startStreaming(AudioCapture* audioSource)
{
    if (!m_modelLoaded.load(std::memory_order_acquire)) {
        spdlog::warn("[WhisperEngine] Cannot start streaming: no model loaded.");
        notifyError("No whisper model loaded.");
        return;
    }

    if (m_streaming.load(std::memory_order_acquire)) {
        spdlog::warn("[WhisperEngine] Already streaming.");
        return;
    }

    m_streaming.store(true, std::memory_order_release);
    m_workerRunning.store(true, std::memory_order_release);
    
    m_workerThread = std::make_unique<std::thread>(&WhisperEngine::streamingWorkerLoop, this, audioSource);

    spdlog::info("[WhisperEngine] Streaming started. Chunk interval: {}ms, window: {}ms",
            m_config.chunkMs, m_config.slidingWindowMs);
}

void WhisperEngine::stopStreaming()
{
    if (!m_streaming.load(std::memory_order_acquire)) return;

    m_streaming.store(false, std::memory_order_release);
    m_workerRunning.store(false, std::memory_order_release);

    if (m_workerThread && m_workerThread->joinable()) {
        m_workerThread->join();
    }

    m_workerThread.reset();

    spdlog::info("[WhisperEngine] Streaming stopped.");
}

bool WhisperEngine::isStreaming() const noexcept
{
    return m_streaming.load(std::memory_order_acquire);
}

void WhisperEngine::setConfig(const WhisperConfig& config)
{
    m_config = config;
}

const WhisperConfig& WhisperEngine::getConfig() const noexcept
{
    return m_config;
}

void WhisperEngine::abort() noexcept
{
    m_abortRequested.store(true, std::memory_order_release);
}

// =========================================================================
// StreamingWorker Implementation
// =========================================================================

void WhisperEngine::streamingWorkerLoop(AudioCapture* audioSource)
{
    spdlog::info("[StreamingWorker] Worker thread started.");

    const int chunkMs    = m_config.chunkMs;
    const int windowMs   = m_config.slidingWindowMs;
    const float windowSec = static_cast<float>(windowMs) / 1000.0f;
    std::string lastPartialText;

    while (m_workerRunning.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(chunkMs));

        if (!m_workerRunning.load(std::memory_order_acquire)) break;

        const auto vad = audioSource->getVADState();
        if (!vad.isSpeechActive) {
            continue;
        }

        std::vector<float> audioChunk = audioSource->getLastNSeconds(windowSec);

        if (audioChunk.empty()) continue;

        size_t minSamples = SAMPLE_RATE / 5;  // 200ms
        if (audioChunk.size() < minSamples) continue;

        auto result = transcribe(audioChunk, false);

        if (result.text.empty()) continue;

        if (result.text != lastPartialText) {
            lastPartialText = result.text;
            if (onPartialTranscription) {
                onPartialTranscription(result.text);
            }

            result.isPartial = true;
            if (onTranscriptionResult) {
                onTranscriptionResult(result);
            }
        }
    }

    spdlog::info("[StreamingWorker] Worker thread exiting.");
}

void WhisperEngine::notifyError(const std::string& error) {
    if (onEngineError) {
        onEngineError(error);
    }
}

} // namespace vision::voice
