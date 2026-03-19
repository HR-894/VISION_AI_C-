// =============================================================================
// VISION AI - AudioCapture.h
// Thread-safe audio capture with lock-free circular buffer + VAD
// Uses PortAudio for cross-device microphone input
// =============================================================================
#pragma once

#include <QObject>
#include <QMutex>

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
// Single-Producer (PortAudio callback) / Single-Consumer (WhisperEngine)
// Uses atomic counters — no mutex needed in the hot path.
// -------------------------------------------------------------------------
class AudioRingBuffer {
public:
    AudioRingBuffer();

    // Producer: called from PortAudio callback (real-time safe)
    void write(const float* data, size_t sampleCount) noexcept;

    // Consumer: copy the last N samples into output buffer
    // Returns actual number of samples copied (may be less than requested)
    size_t readLast(float* output, size_t sampleCount) const noexcept;

    // Consumer: copy all samples since the last call to readNew()
    size_t readNew(float* output, size_t maxSamples) noexcept;

    // Total samples written since construction/clear
    [[nodiscard]] size_t totalWritten() const noexcept;

    // Available unread samples
    [[nodiscard]] size_t available() const noexcept;

    void clear() noexcept;

private:
    std::array<float, RING_BUFFER_SAMPLES> m_buffer{};
    std::atomic<size_t> m_writePos{0};     // Monotonically increasing write cursor
    size_t              m_readPos = 0;      // Consumer read cursor (not atomic — single consumer)
};

// -------------------------------------------------------------------------
// AudioCapture - Main Class
// -------------------------------------------------------------------------
class AudioCapture : public QObject {
    Q_OBJECT

public:
    explicit AudioCapture(QObject* parent = nullptr);
    ~AudioCapture() override;

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
    // Get the last N seconds of audio from the ring buffer
    [[nodiscard]] std::vector<float> getLastNSeconds(float seconds) const;

    // Get all new audio since last call (for streaming consumer)
    [[nodiscard]] std::vector<float> getNewAudio();

    // Get all audio captured in the current recording session
    [[nodiscard]] std::vector<float> getFullRecording() const;

    // === VAD ===
    [[nodiscard]] const VADState& getVADState() const noexcept;
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

signals:
    // Emitted when VAD detects speech onset
    void speechStarted();

    // Emitted when VAD detects speech end (+ silence hold expired)
    void speechEnded(int durationMs);

    // Emitted periodically with current audio level (for VU meter)
    void audioLevelChanged(float rmsEnergy);

    // Emitted on errors
    void captureError(const QString& error);

private:
    // PortAudio stream callback (static, real-time safe)
    static int paStreamCallback(
        const void* inputBuffer,
        void* outputBuffer,
        unsigned long framesPerBuffer,
        const void* timeInfo,
        unsigned long statusFlags,
        void* userData
    );

    // Process a chunk of audio for VAD
    void processVAD(const float* samples, size_t count);

    // Compute RMS energy of a buffer
    [[nodiscard]] static float computeRMSEnergy(
        const float* samples, size_t count) noexcept;

    // Noise floor estimation (exponential moving average)
    void updateNoiseFloor(float energy) noexcept;

    // === State ===
    void*                       m_paStream  = nullptr;  // PaStream* (opaque)
    AudioRingBuffer             m_ringBuffer;

    // Full recording accumulator (for final pass)
    mutable QMutex              m_fullRecMutex;
    std::vector<float>          m_fullRecording;

    // VAD
    VADState                    m_vadState;
    bool                        m_vadEnabled = true;
    int                         m_vadHoldCounter = 0;

    // Flags
    std::atomic<bool>           m_initialized{false};
    std::atomic<bool>           m_recording{false};

    // Device selection
    int                         m_deviceIndex = -1;  // -1 = system default
};

} // namespace vision::voice
