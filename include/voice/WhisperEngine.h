// =============================================================================
// VISION AI - WhisperEngine.h
// Non-blocking streaming transcription worker using whisper.cpp
// Runs on a dedicated std::thread, processes audio chunks via sliding window,
// invokes partial transcriptions in real-time (No Qt)
// =============================================================================
#pragma once

#include <mutex>
#include <thread>
#include <string>
#include <vector>
#include <atomic>
#include <memory>
#include <functional>
#include <chrono>
#include <future>

struct whisper_context;

namespace vision::voice {

class AudioCapture;

// -------------------------------------------------------------------------
// Transcription Result
// -------------------------------------------------------------------------
struct TranscriptionResult {
    std::string text;               
    bool        isPartial = true;   
    float       confidence = 0.0f;  
    int64_t     latencyMs  = 0;     
    int         audioChunkMs = 0;   
};

// -------------------------------------------------------------------------
// Whisper Configuration
// -------------------------------------------------------------------------
struct WhisperConfig {
    std::string modelPath;          
    std::string language = "en";    
    bool        translate = false;  

    int     chunkMs         = 1000;     
    int     slidingWindowMs = 2000;     
    int     finalWindowMs   = 0;        

    int     beamSize        = 2;        
    int     bestOf          = 1;        
    int     threads         = 4;        
    bool    useGPU          = false;    

    bool    noTimestamps    = true;     
    bool    singleSegment   = true;     
    float   noSpeechThreshold = 0.6f;   
};

// -------------------------------------------------------------------------
// WhisperEngine - Streaming Transcription Worker
// -------------------------------------------------------------------------
class WhisperEngine {
public:
    explicit WhisperEngine();
    ~WhisperEngine();

    WhisperEngine(const WhisperEngine&) = delete;
    WhisperEngine& operator=(const WhisperEngine&) = delete;

    // === Model Management ===
    [[nodiscard]] bool loadModel(const WhisperConfig& config);
    void unloadModel();
    [[nodiscard]] bool isModelLoaded() const noexcept;

    // === Streaming Control ===
    void startStreaming(AudioCapture* audioSource);
    void stopStreaming();
    [[nodiscard]] bool isStreaming() const noexcept;

    // === One-Shot Transcription ===
    [[nodiscard]] TranscriptionResult transcribe(
        const std::vector<float>& audioData,
        bool highAccuracy = false
    );

    void transcribeAsync(
        const std::vector<float>& audioData,
        bool highAccuracy = false
    );

    // === Configuration ===
    void setConfig(const WhisperConfig& config);
    [[nodiscard]] const WhisperConfig& getConfig() const noexcept;

    void abort() noexcept;

    // === Callbacks ===
    std::function<void(const std::string&)> onPartialTranscription;
    std::function<void(const std::string&, float confidence)> onFinalTranscription;
    std::function<void(const TranscriptionResult&)> onTranscriptionResult;
    std::function<void(const std::string& error)> onEngineError;
    std::function<void(const std::string& modelPath)> onModelLoaded;
    std::function<void()> onModelUnloaded;

private:
    void streamingWorkerLoop(AudioCapture* audioSource);
    void notifyError(const std::string& error);

    // === Async Task ===
    std::future<void>                 m_asyncTask;

    // === Worker Thread ===
    std::unique_ptr<std::thread>      m_workerThread;
    std::atomic<bool>                 m_workerRunning{false};

    // === Whisper Context ===
    whisper_context*                  m_ctx = nullptr;
    WhisperConfig                     m_config;

    // === State ===
    std::atomic<bool>                 m_modelLoaded{false};
    std::atomic<bool>                 m_streaming{false};
    std::atomic<bool>                 m_abortRequested{false};

    mutable std::mutex                m_ctxMutex;  
};

} // namespace vision::voice
