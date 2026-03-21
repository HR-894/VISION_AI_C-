// =============================================================================
// VISION AI - VoiceManager.h
// High-level orchestrator: AudioCapture -> WhisperEngine -> UI
// Manages the full voice input lifecycle with hotkey support
// =============================================================================
#pragma once

#include <QObject>
#include <QString>

#include <memory>
#include <atomic>
#include <string>
#include <functional>

namespace vision::voice {

// Forward declarations
class AudioCapture;
class WhisperEngine;
struct WhisperConfig;
struct TranscriptionResult;

// -------------------------------------------------------------------------
// Voice Input State Machine
// -------------------------------------------------------------------------
enum class VoiceState : uint8_t {
    Idle,           // Not listening
    Listening,      // Recording + streaming partial transcriptions
    Processing,     // Final transcription pass running
    Error,          // Something went wrong
};

// -------------------------------------------------------------------------
// VoiceManager - Orchestrator
// -------------------------------------------------------------------------
class VoiceManager : public QObject {
    Q_OBJECT

public:
    explicit VoiceManager(QObject* parent = nullptr);
    ~VoiceManager() override;

    // Non-copyable
    VoiceManager(const VoiceManager&) = delete;
    VoiceManager& operator=(const VoiceManager&) = delete;

    // === Initialization ===
    // Must be called before any voice operations.
    // whisperModelPath: path to .bin whisper model file
    [[nodiscard]] bool initialize(const std::string& whisperModelPath);
    void shutdown();

    // === Voice Control ===
    // Start listening (press-and-hold or toggle mode)
    void startListening();

    // Stop listening and trigger final transcription
    void stopListening();

    // Toggle mode: start if idle, stop if listening
    void toggleListening();

    // Abort everything and return to idle
    void abortListening();

    // === Wake Word ===
    void startWakeWordListener();
    void stopWakeWordListener();

    // === State ===
    [[nodiscard]] VoiceState state() const noexcept;
    [[nodiscard]] bool isListening() const noexcept;
    [[nodiscard]] bool isProcessing() const noexcept;

    // === Configuration ===
    void setWhisperConfig(const WhisperConfig& config);
    void setInputDevice(int deviceIndex);

    // === Component Access (for advanced use) ===
    [[nodiscard]] AudioCapture* audioCapture() const noexcept;
    [[nodiscard]] WhisperEngine* whisperEngine() const noexcept;

signals:
    // === UI Signals ===

    // Voice state changed (for status indicator)
    void stateChanged(VoiceState newState);

    // Partial transcription while speaking (live update for text field)
    void partialText(const QString& text);

    // Final transcription ready (submit to command pipeline)
    void finalText(const QString& text);

    // Audio level for VU meter visualization
    void audioLevel(float rmsEnergy);

    // VAD events
    void speechDetected();
    void silenceDetected(int speechDurationMs);

    // Errors
    void error(const QString& message);

private slots:
    // Wired to AudioCapture signals
    void onSpeechStarted();
    void onSpeechEnded(int durationMs);
    void onAudioLevel(float rms);

    // Wired to WhisperEngine signals
    void onPartialTranscription(const QString& text);
    void onFinalTranscription(const QString& text, float confidence);
    void onEngineError(const QString& err);

private:
    void setState(VoiceState newState);
    void runFinalPass();
    void wakeWordLoop();

    // === Components ===
    std::unique_ptr<AudioCapture>   m_audioCapture;
    std::unique_ptr<WhisperEngine>  m_whisperEngine;

    // === State ===
    std::atomic<VoiceState>         m_state{VoiceState::Idle};
    std::string                     m_accumulatedPartialText;
    bool                            m_initialized = false;
    
    // === Wake Word ===
    std::atomic<bool>               m_wakeWordRunning{false};
    std::unique_ptr<std::thread>    m_wakeWordThread;
};

} // namespace vision::voice
