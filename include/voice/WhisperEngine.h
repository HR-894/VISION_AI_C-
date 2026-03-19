// =============================================================================
// VISION AI - WhisperEngine.h
// Non-blocking streaming transcription worker using whisper.cpp
// Runs on a dedicated QThread, processes audio chunks via sliding window,
// emits partial transcriptions in real-time
// =============================================================================
#pragma once

#include <QObject>
#include <QThread>
#include <QMutex>
#include <QWaitCondition>

#include <string>
#include <vector>
#include <atomic>
#include <memory>
#include <functional>
#include <chrono>

// Forward declaration for whisper.cpp opaque type
struct whisper_context;

namespace vision::voice {

// Forward declaration
class AudioCapture;

// -------------------------------------------------------------------------
// Transcription Result
// -------------------------------------------------------------------------
struct TranscriptionResult {
    std::string text;               // The transcribed text
    bool        isPartial = true;   // true = interim, false = final
    float       confidence = 0.0f;  // Aggregate confidence [0.0 - 1.0]
    int64_t     latencyMs  = 0;     // Time from audio chunk to result
    int         audioChunkMs = 0;   // Duration of audio processed
};

// -------------------------------------------------------------------------
// Whisper Configuration
// -------------------------------------------------------------------------
struct WhisperConfig {
    std::string modelPath;          // Path to .bin whisper model file
    std::string language = "en";    // Language code or "auto"
    bool        translate = false;  // Translate to English?

    // Streaming parameters
    int     chunkMs         = 1000;     // Process audio every N ms
    int     slidingWindowMs = 2000;     // Sliding window size for partial decode
    int     finalWindowMs   = 0;        // 0 = use full recording for final pass

    // Inference parameters (tuned for speed vs accuracy tradeoff)
    int     beamSize        = 2;        // Beam search width (2 = fast, 5 = accurate)
    int     bestOf          = 1;        // Best-of-N sampling
    int     threads         = 4;        // CPU threads for inference
    bool    useGPU          = false;    // Use GPU acceleration if available

    // Speed optimization: reduce audio processing overhead
    bool    noTimestamps    = true;     // Don't compute word-level timestamps
    bool    singleSegment   = true;     // Force single segment output
    float   noSpeechThreshold = 0.6f;   // Suppress non-speech segments
};

// -------------------------------------------------------------------------
// WhisperEngine - Streaming Transcription Worker
// -------------------------------------------------------------------------
class WhisperEngine : public QObject {
    Q_OBJECT

public:
    explicit WhisperEngine(QObject* parent = nullptr);
    ~WhisperEngine() override;

    // Non-copyable
    WhisperEngine(const WhisperEngine&) = delete;
    WhisperEngine& operator=(const WhisperEngine&) = delete;

    // === Model Management ===
    [[nodiscard]] bool loadModel(const WhisperConfig& config);
    void unloadModel();
    [[nodiscard]] bool isModelLoaded() const noexcept;

    // === Streaming Control ===
    // Start the background worker thread that polls AudioCapture
    void startStreaming(AudioCapture* audioSource);

    // Stop the streaming worker
    void stopStreaming();

    [[nodiscard]] bool isStreaming() const noexcept;

    // === One-Shot Transcription ===
    // Transcribe a buffer of PCM float samples (blocking)
    [[nodiscard]] TranscriptionResult transcribe(
        const std::vector<float>& audioData,
        bool highAccuracy = false
    );

    // Transcribe asynchronously (non-blocking, emits signal when done)
    void transcribeAsync(
        const std::vector<float>& audioData,
        bool highAccuracy = false
    );

    // === Configuration ===
    void setConfig(const WhisperConfig& config);
    [[nodiscard]] const WhisperConfig& getConfig() const noexcept;

    // === Abort ===
    void abort() noexcept;

signals:
    // Emitted for each partial (interim) transcription during streaming
    void partialTranscription(const QString& text);

    // Emitted when final transcription is available
    void finalTranscription(const QString& text, float confidence);

    // Emitted on each transcription result (partial or final)
    void transcriptionResult(const TranscriptionResult& result);

    // Emitted on errors
    void engineError(const QString& error);

    // Model lifecycle
    void modelLoaded(const QString& modelPath);
    void modelUnloaded();

private:
    // === Worker Thread ===
    class StreamingWorker;
    std::unique_ptr<QThread>          m_workerThread;
    StreamingWorker*                  m_worker = nullptr;

    // === Whisper Context ===
    whisper_context*                  m_ctx = nullptr;
    WhisperConfig                     m_config;

    // === State ===
    std::atomic<bool>                 m_modelLoaded{false};
    std::atomic<bool>                 m_streaming{false};
    std::atomic<bool>                 m_abortRequested{false};

    mutable QMutex                    m_ctxMutex;  // Guards m_ctx access
};

// -------------------------------------------------------------------------
// StreamingWorker - Internal worker that runs on a QThread
// -------------------------------------------------------------------------
class WhisperEngine::StreamingWorker : public QObject {
    Q_OBJECT

public:
    StreamingWorker(WhisperEngine* engine, AudioCapture* audioSource);

    void stop() noexcept;

public slots:
    void process();

signals:
    void partialResult(const QString& text);
    void finished();

private:
    WhisperEngine*  m_engine;
    AudioCapture*   m_audioSource;
    std::atomic<bool> m_running{true};
    std::string     m_lastPartialText;
};

} // namespace vision::voice
