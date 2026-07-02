// =============================================================================
// VISION AI - VoiceManager.h
// High-level orchestrator: AudioCapture -> WhisperEngine -> UI
// Manages the full voice input lifecycle with hotkey support (No Qt)
// =============================================================================
#pragma once

#include <memory>
#include <atomic>
#include <string>
#include <functional>
#include <thread>
#include <mutex>

namespace vision::voice {

class AudioCapture;
class WhisperEngine;
struct WhisperConfig;
struct TranscriptionResult;

// -------------------------------------------------------------------------
// Voice Input State Machine
// -------------------------------------------------------------------------
enum class VoiceState : uint8_t {
    Idle,           
    Listening,      
    Processing,     
    Error,          
};

// -------------------------------------------------------------------------
// VoiceManager - Orchestrator
// -------------------------------------------------------------------------
class VoiceManager {
public:
    explicit VoiceManager();
    ~VoiceManager();

    VoiceManager(const VoiceManager&) = delete;
    VoiceManager& operator=(const VoiceManager&) = delete;

    // === Initialization ===
    [[nodiscard]] bool initialize(const std::string& whisperModelPath);
    void shutdown();

    // === Voice Control ===
    void startListening();
    void stopListening();
    void toggleListening();
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

    // === Component Access ===
    [[nodiscard]] AudioCapture* audioCapture() const noexcept;
    [[nodiscard]] WhisperEngine* whisperEngine() const noexcept;

    // === Callbacks ===
    std::function<void(VoiceState)> onStateChanged;
    std::function<void(const std::string&)> onPartialText;
    std::function<void(const std::string&)> onFinalText;
    std::function<void(float)> onAudioLevel;
    std::function<void()> onSpeechDetected;
    std::function<void(int)> onSilenceDetected;
    std::function<void(const std::string&)> onError;

private:
    void onSpeechStartedCallback();
    void onSpeechEndedCallback(int durationMs);
    void onAudioLevelCallback(float rms);

    void onPartialTranscriptionCallback(const std::string& text);
    void onFinalTranscriptionCallback(const std::string& text, float confidence);
    void onEngineErrorCallback(const std::string& err);

    void setState(VoiceState newState);
    void runFinalPass();
    void wakeWordLoop();

    mutable std::mutex              m_stateMutex;

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
