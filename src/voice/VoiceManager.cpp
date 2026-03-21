// =============================================================================
// VISION AI - VoiceManager.cpp
// Orchestrates AudioCapture -> WhisperEngine -> UI pipeline
// Manages state machine: Idle -> Listening -> Processing -> Idle
// =============================================================================
#include "VoiceManager.h"
#include "AudioCapture.h"
#include "WhisperEngine.h"

#include <QDebug>
#include <QMetaObject>
#include <thread>
#include <chrono>
#include <algorithm>

namespace vision::voice {

// =========================================================================
// Construction / Destruction
// =========================================================================

VoiceManager::VoiceManager(QObject* parent)
    : QObject(parent)
    , m_audioCapture(std::make_unique<AudioCapture>(this))
    , m_whisperEngine(std::make_unique<WhisperEngine>(this))
{
    // Wire AudioCapture signals
    connect(m_audioCapture.get(), &AudioCapture::speechStarted,
            this, &VoiceManager::onSpeechStarted);
    connect(m_audioCapture.get(), &AudioCapture::speechEnded,
            this, &VoiceManager::onSpeechEnded);
    connect(m_audioCapture.get(), &AudioCapture::audioLevelChanged,
            this, &VoiceManager::onAudioLevel);
    connect(m_audioCapture.get(), &AudioCapture::captureError,
            this, [this](const QString& err) {
                qWarning() << "[VoiceManager] Capture error:" << err;
                emit error(err);
                setState(VoiceState::Error);
            });

    // Wire WhisperEngine signals
    connect(m_whisperEngine.get(), &WhisperEngine::partialTranscription,
            this, &VoiceManager::onPartialTranscription);
    connect(m_whisperEngine.get(), &WhisperEngine::finalTranscription,
            this, &VoiceManager::onFinalTranscription);
    connect(m_whisperEngine.get(), &WhisperEngine::engineError,
            this, &VoiceManager::onEngineError);
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
    qInfo() << "[VoiceManager] Initializing with model:"
            << QString::fromStdString(whisperModelPath);

    // Initialize audio capture (PortAudio)
    if (!m_audioCapture->initialize()) {
        emit error("Failed to initialize audio capture.");
        return false;
    }

    // Load whisper model
    WhisperConfig config;
    config.modelPath    = whisperModelPath;
    config.language     = "en";
    config.chunkMs      = 800;       // Process every 800ms during streaming
    config.slidingWindowMs = 2000;   // 2-second sliding window for context
    config.beamSize     = 2;         // Fast beam for partials
    config.bestOf       = 1;
    config.threads      = std::max(1u, std::thread::hardware_concurrency() / 4);
    config.noTimestamps = true;
    config.singleSegment = true;

    if (!m_whisperEngine->loadModel(config)) {
        emit error("Failed to load whisper model.");
        return false;
    }

    m_initialized = true;
    qInfo() << "[VoiceManager] Initialization complete.";
    
    // Auto-start wake-word listener
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

    qInfo() << "[VoiceManager] Shutdown complete.";
}

// =========================================================================
// Voice Control
// =========================================================================

void VoiceManager::startListening()
{
    if (!m_initialized) {
        emit error("VoiceManager not initialized. Call initialize() first.");
        return;
    }

    if (m_state.load(std::memory_order_acquire) == VoiceState::Listening) {
        qDebug() << "[VoiceManager] Already listening.";
        return;
    }

    if (m_state.load(std::memory_order_acquire) == VoiceState::Processing) {
        qDebug() << "[VoiceManager] Still processing previous input. Aborting.";
        m_whisperEngine->abort();
    }

    // Clear previous state
    m_accumulatedPartialText.clear();

    // Start recording
    if (!m_audioCapture->startRecording()) {
        emit error("Failed to start audio recording.");
        setState(VoiceState::Error);
        return;
    }

    // Start streaming transcription worker
    m_whisperEngine->startStreaming(m_audioCapture.get());

    setState(VoiceState::Listening);
    qInfo() << "[VoiceManager] Listening started.";
}

void VoiceManager::stopListening()
{
    if (m_state.load(std::memory_order_acquire) != VoiceState::Listening) {
        return;
    }

    qInfo() << "[VoiceManager] Stop listening -> running final pass...";

    // Stop streaming worker first
    m_whisperEngine->stopStreaming();

    // Stop recording (this also captures the final VAD state)
    m_audioCapture->stopRecording();

    // Run final high-accuracy transcription pass
    setState(VoiceState::Processing);
    runFinalPass();
}

void VoiceManager::toggleListening()
{
    VoiceState current = m_state.load(std::memory_order_acquire);
    if (current == VoiceState::Idle || current == VoiceState::Error) {
        startListening();
    } else if (current == VoiceState::Listening) {
        stopListening();
    }
    // If Processing, do nothing — let it finish
}

void VoiceManager::abortListening()
{
    VoiceState current = m_state.load(std::memory_order_acquire);
    if (current == VoiceState::Idle) return;

    m_whisperEngine->abort();
    m_whisperEngine->stopStreaming();
    m_audioCapture->stopRecording();
    m_accumulatedPartialText.clear();

    setState(VoiceState::Idle);
    qInfo() << "[VoiceManager] Listening aborted.";
    
    // Resume background audio if wake word is active
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
    
    // Ensure capture is running
    if (!m_audioCapture->isRecording() && state() == VoiceState::Idle) {
        m_audioCapture->startRecording();
    }
    
    m_wakeWordThread = std::make_unique<std::thread>(&VoiceManager::wakeWordLoop, this);
    qInfo() << "[VoiceManager] Wake-Word listener started.";
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
        
        // Skip wake word check if already actively listening or processing
        if (state() != VoiceState::Idle) continue;
        
        // We only care about the very recent audio tail
        auto audio = m_audioCapture->getLastNSeconds(1.2f);
        if (audio.size() < 16000 * 0.8) continue; // Minimum 0.8s
        
        // Fast decode without high accuracy
        auto result = m_whisperEngine->transcribe(audio, false);
        std::string text = result.text;
        std::transform(text.begin(), text.end(), text.begin(), ::tolower);
        
        if (text.find("hey vision") != std::string::npos ||
            text.find("vision awake") != std::string::npos ||
            text.find("hey listen") != std::string::npos) {
            
            qInfo() << "[VoiceManager] Wake-Word Detected! Text: " << QString::fromStdString(text);
            
            // Hop to main thread to start listening cleanly
            QMetaObject::invokeMethod(this, [this]() {
                if (state() == VoiceState::Idle) {
                    startListening();
                }
            }, Qt::QueuedConnection);
            
            // Sleep to debounce
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
        qDebug() << "[VoiceManager] State:" << static_cast<int>(old)
                  << "->" << static_cast<int>(newState);
        emit stateChanged(newState);
    }
}

// =========================================================================
// Private: Final Transcription Pass
// =========================================================================

void VoiceManager::runFinalPass()
{
    // Get the full recording from the AudioCapture buffer
    std::vector<float> fullAudio = m_audioCapture->getFullRecording();

    if (fullAudio.empty()) {
        qInfo() << "[VoiceManager] No audio captured. Returning to idle.";
        setState(VoiceState::Idle);
        return;
    }

    float durationSec = static_cast<float>(fullAudio.size()) / SAMPLE_RATE;
    qInfo() << "[VoiceManager] Running final transcription pass on"
            << durationSec << "seconds of audio...";

    // Minimum 300ms of audio for final pass
    if (fullAudio.size() < SAMPLE_RATE * 3 / 10) {
        qInfo() << "[VoiceManager] Audio too short (" << durationSec
                << "s). Using partial text.";
        if (!m_accumulatedPartialText.empty()) {
            emit finalText(QString::fromStdString(m_accumulatedPartialText));
        }
        setState(VoiceState::Idle);
        return;
    }

    // Run async high-accuracy transcription
    // The signal will come back via onFinalTranscription()
    m_whisperEngine->transcribeAsync(fullAudio, true /* highAccuracy */);
}

// =========================================================================
// Slots: AudioCapture Events
// =========================================================================

void VoiceManager::onSpeechStarted()
{
    emit speechDetected();
}

void VoiceManager::onSpeechEnded(int durationMs)
{
    emit silenceDetected(durationMs);

    // In future: could auto-stop listening after prolonged silence
    // For now, we rely on explicit stop (hotkey release)
}

void VoiceManager::onAudioLevel(float rms)
{
    emit audioLevel(rms);
}

// =========================================================================
// Slots: WhisperEngine Events
// =========================================================================

void VoiceManager::onPartialTranscription(const QString& text)
{
    m_accumulatedPartialText = text.toStdString();

    // Forward to UI for live text field update
    emit partialText(text);
}

void VoiceManager::onFinalTranscription(const QString& text, float confidence)
{
    qInfo() << "[VoiceManager] Final transcription received:"
            << text.size() << "chars, confidence:" << confidence;

    // Use the final text if it's non-empty, otherwise fall back to last partial
    if (!text.isEmpty()) {
        emit finalText(text);
    } else if (!m_accumulatedPartialText.empty()) {
        emit finalText(QString::fromStdString(m_accumulatedPartialText));
    }

    m_accumulatedPartialText.clear();
    setState(VoiceState::Idle);
    
    if (m_wakeWordRunning) {
        m_audioCapture->startRecording();
    }
}

void VoiceManager::onEngineError(const QString& err)
{
    qWarning() << "[VoiceManager] Engine error:" << err;
    emit error(err);

    // If we were processing, fall back to partial text
    if (m_state.load(std::memory_order_acquire) == VoiceState::Processing) {
        if (!m_accumulatedPartialText.empty()) {
            emit finalText(QString::fromStdString(m_accumulatedPartialText));
        }
    }

    m_accumulatedPartialText.clear();
    setState(VoiceState::Idle);

    if (m_wakeWordRunning) {
        m_audioCapture->startRecording();
    }
}

} // namespace vision::voice
