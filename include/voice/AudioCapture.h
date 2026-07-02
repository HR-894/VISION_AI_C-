// =============================================================================
// VISION AI - AudioCapture.h
// Thread-safe audio capture with lock-free circular buffer + VAD
// Uses PortAudio for cross-device microphone input (No Qt)
// =============================================================================
#pragma once

#include <string>
#include <vector>
#include <array>
#include <atomic>
#include <memory>
#include <cstdint>
#include <span>
#include <functional>

namespace vision::voice {

// -------------------------------------------------------------------------
// Audio Format Constants
// -------------------------------------------------------------------------
static constexpr int    SAMPLE_RATE       = 16000;   // 16 kHz (whisper.cpp native)
static constexpr int    CHANNELS          = 1;        // Mono
static constexpr int    FRAMES_PER_CHUNK  = 512;      // ~32ms per callback at 16kHz
static constexpr int    BYTES_PER_SAMPLE  = sizeof(float);  // 32-bit float PCM

// Circular buffer: 30 seconds of audio at 16kHz mono float
static constexpr size_t RING_BUFFER_SECONDS  = 30;
static constexpr size_t RING_BUFFER_SAMPLES  = SAMPLE_RATE * RING_BUFFER_SECONDS;

// VAD defaults
static constexpr float  VAD_ENERGY_THRESHOLD = 0.001f; // RMS energy threshold
static constexpr int    VAD_HOLD_FRAMES      = 20;     // ~640ms hold after speech

// -------------------------------------------------------------------------
// Voice Activity Detection State
// -------------------------------------------------------------------------
struct VADState {
    bool    isSpeechActive  = false;
    float   currentEnergy   = 0.0f;     // Current frame RMS energy
    float   peakEnergy      = 0.0f;     // Peak energy in this utterance
    int64_t speechStartMs   = 0;        // When speech started (epoch ms)
    int64_t speechDurationMs = 0;       // How long speech has been active
    
    // Adaptive threshold (adjusts to ambient noise over time)
    float   noiseFloor      = 0.001f;
    float   threshold       = VAD_ENERGY_THRESHOLD;
};

// -------------------------------------------------------------------------
// Circular Buffer (Lock-Free SPSC for audio callback -> reader thread)
// -------------------------------------------------------------------------
class AudioRingBuffer {
public:
    AudioRingBuffer();

    void write(const float* data, size_t sampleCount) noexcept;
    size_t readLast(float* output, size_t sampleCount) const noexcept;
    size_t readNew(float* output, size_t maxSamples) noexcept;
    
    [[nodiscard]] size_t totalWritten() const noexcept;
    [[nodiscard]] size_t available() const noexcept;
    void clear() noexcept;

private:
    std::array<float, RING_BUFFER_SAMPLES> m_buffer{};
    std::atomic<size_t> m_writePos{0};     
    size_t              m_readPos = 0;      
};

// -------------------------------------------------------------------------
// AudioCapture - Main Class
// -------------------------------------------------------------------------
class AudioCapture {
public:
    explicit AudioCapture();
    ~AudioCapture();

    // Non-copyable
    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    // === Lifecycle ===
    [[nodiscard]] bool initialize();
    void shutdown();

    // === Recording Control ===
    [[nodiscard]] bool startRecording();
    void stopRecording();
    [[nodiscard]] bool isRecording() const noexcept;

    // === Data Access ===
    [[nodiscard]] std::vector<float> getLastNSeconds(float seconds) const;
    // Get all new audio since last call (for streaming consumer)
    [[nodiscard]] std::vector<float> getNewAudio();

    // === VAD ===
    [[nodiscard]] VADState getVADState() const noexcept;
    void setVADThreshold(float threshold);
    void setVADEnabled(bool enabled);

    // === Device Enumeration ===
    struct AudioDevice {
        int         index;
        std::string name;
        int         maxInputChannels;
        double      defaultSampleRate;
        bool        isDefault;
    };
    [[nodiscard]] static std::vector<AudioDevice> listInputDevices();
    void setInputDevice(int deviceIndex);

    // === Callbacks ===
    std::function<void()> onSpeechStarted;
    std::function<void(int durationMs)> onSpeechEnded;
    std::function<void(float rmsEnergy)> onAudioLevelChanged;
    std::function<void(const std::string& error)> onCaptureError;

private:
    static int paStreamCallback(
        const void* inputBuffer, void* outputBuffer,
        unsigned long framesPerBuffer, const void* timeInfo,
        unsigned long statusFlags, void* userData
    );

    void processVAD(const float* samples, size_t count);
    [[nodiscard]] static float computeRMSEnergy(const float* samples, size_t count) noexcept;
    void updateNoiseFloor(float energy) noexcept;

    void notifyError(const std::string& err);

    // === State ===
    void*                       m_paStream  = nullptr;  // PaStream* (opaque)
    AudioRingBuffer             m_ringBuffer;

    // VAD (Atomic primitives for cross-thread safety)
    std::atomic<bool>           m_vadIsSpeechActive{false};
    std::atomic<float>          m_vadCurrentEnergy{0.0f};
    std::atomic<float>          m_vadPeakEnergy{0.0f};
    std::atomic<int64_t>        m_vadSpeechStartMs{0};
    std::atomic<int64_t>        m_vadSpeechDurationMs{0};
    std::atomic<float>          m_vadNoiseFloor{0.001f};
    std::atomic<float>          m_vadThreshold{VAD_ENERGY_THRESHOLD};

    bool                        m_vadEnabled = true;
    int                         m_vadHoldCounter = 0;

    // Flags
    std::atomic<bool>           m_initialized{false};
    std::atomic<bool>           m_recording{false};

    // Device selection
    int                         m_deviceIndex = -1;  
};

} // namespace vision::voice
