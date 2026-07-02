// =============================================================================
// VISION AI - VoiceManager.cpp
// Orchestrates AudioCapture -> WhisperEngine -> UI pipeline
// Manages state machine: Idle -> Listening -> Processing -> Idle (No Qt)
// =============================================================================
#include "VoiceManager.h"
#include "AudioCapture.h"
#include "WhisperEngine.h"

#include <spdlog/spdlog.h>
#include <chrono>
#include <algorithm>
#include <cctype>

namespace vision::voice {

// =========================================================================
// Construction / Destruction
// =========================================================================

VoiceManager::VoiceManager()
    : m_audioCapture(std::make_unique<AudioCapture>())
    , m_whisperEngine(std::make_unique<WhisperEngine>())
{
    // Wire AudioCapture callbacks
    m_audioCapture->onSpeechStarted = [this]() { onSpeechStartedCallback(); };
    m_audioCapture->onSpeechEnded = [this](int durationMs) { onSpeechEndedCallback(durationMs); };
    m_audioCapture->onAudioLevelChanged = [this](float rms) { onAudioLevelCallback(rms); };
    m_audioCapture->onCaptureError = [this](const std::string& err) {
        spdlog::warn("[VoiceManager] Capture error: {}", err);
        if (onError) onError(err);
        setState(VoiceState::Error);
    };

    // Wire WhisperEngine callbacks
    m_whisperEngine->onPartialTranscription = [this](const std::string& text) { onPartialTranscriptionCallback(text); };
    m_whisperEngine->onFinalTranscription = [this](const std::string& text, float conf) { onFinalTranscriptionCallback(text, conf); };
    m_whisperEngine->onEngineError = [this](const std::string& err) { onEngineErrorCallback(err); };
}

VoiceManager::~VoiceManager()
{
    shutdown();
}

// =========================================================================
// Initialization
// =========================================================================

bool VoiceManager::initialize(const std::string& whisperModelPath)
{
    spdlog::info("[VoiceManager] Initializing with model: {}", whisperModelPath);

    if (!m_audioCapture->initialize()) {
        if (onError) onError("Failed to initialize audio capture.");
        return false;
    }

    WhisperConfig config;
    config.modelPath    = whisperModelPath;
    config.language     = "en";
    config.chunkMs      = 800;       
    config.slidingWindowMs = 2000;   
    config.beamSize     = 2;         
    config.bestOf       = 1;
    config.threads      = std::max(1u, std::thread::hardware_concurrency() / 4);
    config.noTimestamps = true;
    config.singleSegment = true;

    if (!m_whisperEngine->loadModel(config)) {
        if (onError) onError("Failed to load whisper model.");
        return false;
    }

    m_initialized = true;
    spdlog::info("[VoiceManager] Initialization complete.");
    
    startWakeWordListener();
    return true;
}

void VoiceManager::shutdown()
{
    stopWakeWordListener();
    abortListening();

    if (m_whisperEngine) m_whisperEngine->unloadModel();
    if (m_audioCapture)  m_audioCapture->shutdown();

    m_initialized = false;
    setState(VoiceState::Idle);

    spdlog::info("[VoiceManager] Shutdown complete.");
}

// =========================================================================
// Voice Control
// =========================================================================

void VoiceManager::startListening()
{
    std::lock_guard<std::mutex> lock(m_stateMutex);

    if (!m_initialized) {
        if (onError) onError("VoiceManager not initialized. Call initialize() first.");
        return;
    }

    if (m_state.load(std::memory_order_acquire) == VoiceState::Listening) {
        spdlog::debug("[VoiceManager] Already listening.");
        return;
    }

    if (m_state.load(std::memory_order_acquire) == VoiceState::Processing) {
        spdlog::debug("[VoiceManager] Still processing previous input. Aborting.");
        m_whisperEngine->abort();
    }

    m_accumulatedPartialText.clear();

    if (!m_audioCapture->startRecording()) {
        if (onError) onError("Failed to start audio recording.");
        setState(VoiceState::Error);
        return;
    }

    m_whisperEngine->startStreaming(m_audioCapture.get());

    setState(VoiceState::Listening);
    spdlog::info("[VoiceManager] Listening started.");
}

void VoiceManager::stopListening()
{
    std::lock_guard<std::mutex> lock(m_stateMutex);

    if (m_state.load(std::memory_order_acquire) != VoiceState::Listening) {
        return;
    }

    spdlog::info("[VoiceManager] Stop listening -> running final pass...");

    m_whisperEngine->stopStreaming();
    m_audioCapture->stopRecording();

    setState(VoiceState::Processing);
    runFinalPass();
}

void VoiceManager::toggleListening()
{
    VoiceState current = state(); // Doesn't lock, safe for atomics
    if (current == VoiceState::Idle || current == VoiceState::Error) {
        startListening();
    } else if (current == VoiceState::Listening) {
        stopListening();
    }
}

void VoiceManager::abortListening()
{
    std::lock_guard<std::mutex> lock(m_stateMutex);

    VoiceState current = m_state.load(std::memory_order_acquire);
    if (current == VoiceState::Idle) return;

    m_whisperEngine->abort();
    m_whisperEngine->stopStreaming();
    m_audioCapture->stopRecording();
    m_accumulatedPartialText.clear();

    setState(VoiceState::Idle);
    spdlog::info("[VoiceManager] Listening aborted.");
    
    if (m_wakeWordRunning) {
        m_audioCapture->startRecording();
    }
}

// =========================================================================
// Wake Word
// =========================================================================

void VoiceManager::startWakeWordListener()
{
    if (m_wakeWordRunning.exchange(true)) return;
    
    if (!m_audioCapture->isRecording() && state() == VoiceState::Idle) {
        m_audioCapture->startRecording();
    }
    
    m_wakeWordThread = std::make_unique<std::thread>(&VoiceManager::wakeWordLoop, this);
    spdlog::info("[VoiceManager] Wake-Word listener started.");
}

void VoiceManager::stopWakeWordListener()
{
    m_wakeWordRunning = false;
    if (m_wakeWordThread && m_wakeWordThread->joinable()) {
        m_wakeWordThread->join();
    }
    m_wakeWordThread.reset();
}

void VoiceManager::wakeWordLoop()
{
    while (m_wakeWordRunning) {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        
        if (state() != VoiceState::Idle) continue;
        
        auto audio = m_audioCapture->getLastNSeconds(1.2f);
        if (audio.size() < 16000 * 0.8) continue;
        
        auto result = m_whisperEngine->transcribe(audio, false);
        std::string text = result.text;
        std::transform(text.begin(), text.end(), text.begin(), ::tolower);
        
        if (text.find("hey vision") != std::string::npos ||
            text.find("vision awake") != std::string::npos ||
            text.find("hey listen") != std::string::npos) {
            
            spdlog::info("[VoiceManager] Wake-Word Detected! Text: {}", text);
            
            if (state() == VoiceState::Idle) {
                startListening();
            }
            
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }
}

// =========================================================================
// State
// =========================================================================

VoiceState VoiceManager::state() const noexcept
{
    return m_state.load(std::memory_order_acquire);
}

bool VoiceManager::isListening() const noexcept
{
    return m_state.load(std::memory_order_acquire) == VoiceState::Listening;
}

bool VoiceManager::isProcessing() const noexcept
{
    return m_state.load(std::memory_order_acquire) == VoiceState::Processing;
}

// =========================================================================
// Configuration
// =========================================================================

void VoiceManager::setWhisperConfig(const WhisperConfig& config)
{
    m_whisperEngine->setConfig(config);
}

void VoiceManager::setInputDevice(int deviceIndex)
{
    m_audioCapture->setInputDevice(deviceIndex);
}

AudioCapture* VoiceManager::audioCapture() const noexcept
{
    return m_audioCapture.get();
}

WhisperEngine* VoiceManager::whisperEngine() const noexcept
{
    return m_whisperEngine.get();
}

// =========================================================================
// Private: State Machine
// =========================================================================

void VoiceManager::setState(VoiceState newState)
{
    VoiceState old = m_state.exchange(newState, std::memory_order_acq_rel);
    if (old != newState) {
        spdlog::debug("[VoiceManager] State: {} -> {}", static_cast<int>(old), static_cast<int>(newState));
        if (onStateChanged) onStateChanged(newState);
    }
}

// =========================================================================
// Private: Final Transcription Pass
// =========================================================================

void VoiceManager::runFinalPass()
{
    auto vadState = m_audioCapture->getVADState();
    float durationSec = vadState.speechDurationMs / 1000.0f + 1.0f; // Add 1s padding
    durationSec = std::min(durationSec, static_cast<float>(vision::voice::RING_BUFFER_SECONDS));
    
    std::vector<float> fullAudio = m_audioCapture->getLastNSeconds(durationSec);

    if (fullAudio.empty()) {
        spdlog::info("[VoiceManager] No audio captured. Returning to idle.");
        setState(VoiceState::Idle);
        return;
    }

    float actualDurationSec = static_cast<float>(fullAudio.size()) / vision::voice::SAMPLE_RATE;
    spdlog::info("[VoiceManager] Running final transcription pass on {} seconds of audio...", actualDurationSec);

    if (fullAudio.size() < vision::voice::SAMPLE_RATE * 3 / 10) {
        spdlog::info("[VoiceManager] Audio too short ({}s). Using partial text.", actualDurationSec);
        if (!m_accumulatedPartialText.empty()) {
            if (onFinalText) onFinalText(m_accumulatedPartialText);
        }
        setState(VoiceState::Idle);
        return;
    }

    m_whisperEngine->transcribeAsync(fullAudio, true);
}

// =========================================================================
// Callbacks
// =========================================================================

void VoiceManager::onSpeechStartedCallback()
{
    if (onSpeechDetected) onSpeechDetected();
}

void VoiceManager::onSpeechEndedCallback(int durationMs)
{
    if (onSilenceDetected) onSilenceDetected(durationMs);
}

void VoiceManager::onAudioLevelCallback(float rms)
{
    if (onAudioLevel) onAudioLevel(rms);
}

void VoiceManager::onPartialTranscriptionCallback(const std::string& text)
{
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_accumulatedPartialText = text;
    if (onPartialText) onPartialText(text);
}

void VoiceManager::onFinalTranscriptionCallback(const std::string& text, float confidence)
{
    std::lock_guard<std::mutex> lock(m_stateMutex);
    spdlog::info("[VoiceManager] Final transcription received: {} chars, confidence: {}", text.size(), confidence);

    if (!text.empty()) {
        if (onFinalText) onFinalText(text);
    } else if (!m_accumulatedPartialText.empty()) {
        if (onFinalText) onFinalText(m_accumulatedPartialText);
    }

    m_accumulatedPartialText.clear();
    setState(VoiceState::Idle);
    
    if (m_wakeWordRunning) {
        m_audioCapture->startRecording();
    }
}

void VoiceManager::onEngineErrorCallback(const std::string& err)
{
    std::lock_guard<std::mutex> lock(m_stateMutex);
    spdlog::warn("[VoiceManager] Engine error: {}", err);
    if (onError) onError(err);

    if (m_state.load(std::memory_order_acquire) == VoiceState::Processing) {
        if (!m_accumulatedPartialText.empty()) {
            if (onFinalText) onFinalText(m_accumulatedPartialText);
        }
    }

    m_accumulatedPartialText.clear();
    setState(VoiceState::Idle);

    if (m_wakeWordRunning) {
        m_audioCapture->startRecording();
    }
}

} // namespace vision::voice
