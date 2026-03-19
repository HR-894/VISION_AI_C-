// =============================================================================
// VISION AI - WhisperEngine.cpp
// Non-blocking whisper.cpp streaming transcription
// Sliding window + VAD-gated processing + partial/final result emission
// =============================================================================
#include "WhisperEngine.h"
#include "AudioCapture.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QCoreApplication>

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

WhisperEngine::WhisperEngine(QObject* parent)
    : QObject(parent)
{
}

WhisperEngine::~WhisperEngine()
{
    stopStreaming();
    unloadModel();
}

// =========================================================================
// Model Management
// =========================================================================

bool WhisperEngine::loadModel(const WhisperConfig& config)
{
#ifdef VISION_NO_WHISPER
    qWarning() << "[WhisperEngine] whisper.cpp not available.";
    emit engineError("whisper.cpp not compiled in.");
    return false;
#else
    QMutexLocker lock(&m_ctxMutex);

    // Unload existing model
    if (m_ctx) {
        whisper_free(m_ctx);
        m_ctx = nullptr;
    }

    m_config = config;

    qInfo() << "[WhisperEngine] Loading model:"
            << QString::fromStdString(config.modelPath);

    // Initialize whisper context from model file
    struct whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = config.useGPU;

    m_ctx = whisper_init_from_file_with_params(
        config.modelPath.c_str(), cparams);

    if (!m_ctx) {
        qCritical() << "[WhisperEngine] Failed to load whisper model:"
                     << QString::fromStdString(config.modelPath);
        emit engineError("Failed to load whisper model.");
        return false;
    }

    m_modelLoaded.store(true, std::memory_order_release);

    qInfo() << "[WhisperEngine] Model loaded successfully.";
    emit modelLoaded(QString::fromStdString(config.modelPath));

    return true;
#endif
}

void WhisperEngine::unloadModel()
{
#ifndef VISION_NO_WHISPER
    stopStreaming();

    QMutexLocker lock(&m_ctxMutex);

    if (m_ctx) {
        whisper_free(m_ctx);
        m_ctx = nullptr;
    }

    m_modelLoaded.store(false, std::memory_order_release);
    emit modelUnloaded();

    qInfo() << "[WhisperEngine] Model unloaded.";
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
    QMutexLocker lock(&m_ctxMutex);

    if (!m_ctx) {
        result.text = "[Error] No whisper model loaded.";
        return result;
    }

    if (audioData.empty()) {
        result.text = "";
        return result;
    }

    QElapsedTimer timer;
    timer.start();

    // Configure whisper parameters
    struct whisper_full_params wparams =
        whisper_full_default_params(WHISPER_SAMPLING_GREEDY);

    wparams.n_threads       = m_config.threads;
    wparams.language        = m_config.language.c_str();
    wparams.translate       = m_config.translate;
    wparams.no_timestamps   = m_config.noTimestamps;
    wparams.single_segment  = m_config.singleSegment;
    wparams.no_context      = true;  // Don't use previous context for one-shot
    wparams.suppress_blank  = true;
    wparams.suppress_nst    = true;

    if (highAccuracy) {
        // Higher accuracy settings for final pass
        wparams.strategy    = WHISPER_SAMPLING_BEAM_SEARCH;
        wparams.beam_search.beam_size = std::max(3, m_config.beamSize);
        wparams.greedy.best_of = std::max(3, m_config.bestOf);
    } else {
        // Fast settings for partial transcription
        wparams.greedy.best_of = m_config.bestOf;
    }

    // Abort callback
    m_abortRequested.store(false, std::memory_order_release);
    wparams.abort_callback = [](void* userData) -> bool {
        auto* self = static_cast<WhisperEngine*>(userData);
        return self->m_abortRequested.load(std::memory_order_acquire);
    };
    wparams.abort_callback_user_data = this;

    // Run inference
    int rc = whisper_full(m_ctx, wparams,
                           audioData.data(),
                           static_cast<int>(audioData.size()));

    result.latencyMs = timer.elapsed();
    result.audioChunkMs = static_cast<int>(
        audioData.size() * 1000 / SAMPLE_RATE);

    if (rc != 0) {
        qWarning() << "[WhisperEngine] whisper_full failed with rc:" << rc;
        result.text = "";
        return result;
    }

    // Extract segments
    int nSegments = whisper_full_n_segments(m_ctx);
    std::string fullText;
    float totalProb = 0.0f;
    int probCount = 0;

    for (int i = 0; i < nSegments; ++i) {
        const char* segText = whisper_full_get_segment_text(m_ctx, i);
        if (segText) {
            fullText += segText;
        }

        // Collect token probabilities for confidence estimate
        int nTokens = whisper_full_n_tokens(m_ctx, i);
        for (int t = 0; t < nTokens; ++t) {
            float prob = whisper_full_get_token_p(m_ctx, i, t);
            if (prob > 0.0f) {
                totalProb += prob;
                probCount++;
            }
        }
    }

    // Trim whitespace
    while (!fullText.empty() && fullText.front() == ' ') fullText.erase(0, 1);
    while (!fullText.empty() && fullText.back() == ' ')  fullText.pop_back();

    // Filter out whisper hallucination patterns
    // Common false positives when processing silence/noise
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

    qDebug() << "[WhisperEngine] Transcribed:" << result.text.size() << "chars"
             << "| Confidence:" << result.confidence
             << "| Latency:" << result.latencyMs << "ms"
             << "| Audio:" << result.audioChunkMs << "ms";

    return result;
#endif
}

void WhisperEngine::transcribeAsync(
    const std::vector<float>& audioData,
    bool highAccuracy)
{
    // Run on a temporary thread to not block the caller
    auto* thread = QThread::create([this, audioData, highAccuracy]() {
        auto result = transcribe(audioData, highAccuracy);

        emit transcriptionResult(result);

        if (result.isPartial) {
            emit partialTranscription(QString::fromStdString(result.text));
        } else {
            emit finalTranscription(QString::fromStdString(result.text),
                                     result.confidence);
        }
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

// =========================================================================
// Streaming Control
// =========================================================================

void WhisperEngine::startStreaming(AudioCapture* audioSource)
{
    if (!m_modelLoaded.load(std::memory_order_acquire)) {
        qWarning() << "[WhisperEngine] Cannot start streaming: no model loaded.";
        emit engineError("No whisper model loaded.");
        return;
    }

    if (m_streaming.load(std::memory_order_acquire)) {
        qWarning() << "[WhisperEngine] Already streaming.";
        return;
    }

    // Create worker thread
    m_workerThread = std::make_unique<QThread>();
    m_worker = new StreamingWorker(this, audioSource);  // Parented to thread
    m_worker->moveToThread(m_workerThread.get());

    // Wire signals
    connect(m_workerThread.get(), &QThread::started,
            m_worker, &StreamingWorker::process);
    connect(m_worker, &StreamingWorker::finished,
            m_workerThread.get(), &QThread::quit);
    connect(m_worker, &StreamingWorker::partialResult,
            this, &WhisperEngine::partialTranscription);
    connect(m_workerThread.get(), &QThread::finished,
            m_worker, &QObject::deleteLater);

    m_streaming.store(true, std::memory_order_release);
    m_workerThread->start();

    qInfo() << "[WhisperEngine] Streaming started. Chunk interval:"
            << m_config.chunkMs << "ms, window:" << m_config.slidingWindowMs << "ms";
}

void WhisperEngine::stopStreaming()
{
    if (!m_streaming.load(std::memory_order_acquire)) return;

    m_streaming.store(false, std::memory_order_release);

    if (m_worker) {
        m_worker->stop();
    }

    if (m_workerThread && m_workerThread->isRunning()) {
        m_workerThread->quit();
        if (!m_workerThread->wait(5000)) {
            qWarning() << "[WhisperEngine] Worker thread did not stop in time. Terminating.";
            m_workerThread->terminate();
            m_workerThread->wait(2000);
        }
    }

    m_workerThread.reset();
    m_worker = nullptr;

    qInfo() << "[WhisperEngine] Streaming stopped.";
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

WhisperEngine::StreamingWorker::StreamingWorker(
    WhisperEngine* engine, AudioCapture* audioSource)
    : QObject(nullptr)
    , m_engine(engine)
    , m_audioSource(audioSource)
{
}

void WhisperEngine::StreamingWorker::stop() noexcept
{
    m_running.store(false, std::memory_order_release);
}

void WhisperEngine::StreamingWorker::process()
{
    qInfo() << "[StreamingWorker] Worker thread started.";

    const int chunkMs    = m_engine->m_config.chunkMs;
    const int windowMs   = m_engine->m_config.slidingWindowMs;
    const float windowSec = static_cast<float>(windowMs) / 1000.0f;

    while (m_running.load(std::memory_order_acquire)) {
        // Sleep for the chunk interval
        QThread::msleep(chunkMs);

        if (!m_running.load(std::memory_order_acquire)) break;

        // Check if audio source has speech (VAD gate)
        const auto& vad = m_audioSource->getVADState();
        if (!vad.isSpeechActive) {
            // No speech — skip processing to save CPU
            continue;
        }

        // Grab the last N seconds of audio (sliding window)
        std::vector<float> audioChunk = m_audioSource->getLastNSeconds(windowSec);

        if (audioChunk.empty()) continue;

        // Minimum audio length: 200ms to avoid hallucinations
        size_t minSamples = SAMPLE_RATE / 5;  // 200ms
        if (audioChunk.size() < minSamples) continue;

        // Run fast transcription (low beam, greedy)
        auto result = m_engine->transcribe(audioChunk, false);

        if (result.text.empty()) continue;

        // Deduplicate: only emit if text actually changed
        if (result.text != m_lastPartialText) {
            m_lastPartialText = result.text;
            emit partialResult(QString::fromStdString(result.text));

            result.isPartial = true;
            emit m_engine->transcriptionResult(result);
        }
    }

    qInfo() << "[StreamingWorker] Worker thread exiting.";
    emit finished();
}

} // namespace vision::voice
