#pragma once
/**
 * @file audio_capture.h
 * @brief Microphone audio capture using PortAudio — with VAD and sliding window
 * 
 * Records audio from the default input device into a continuous buffer.
 * Provides Voice Activity Detection (VAD) via RMS amplitude thresholds,
 * and a sliding-window API for the transcription worker to grab recent audio.
 * Thread-safe start/stop with configurable sample rate.
 */

#include <string>
#include <vector>
#include <mutex>
#include <atomic>

#ifdef VISION_HAS_AUDIO
typedef void PaStream;
#include <portaudio.h>
#endif

namespace vision {

class AudioCapture {
public:
    AudioCapture(int sample_rate = 16000, int channels = 1);
    ~AudioCapture();

    // Non-copyable
    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    /// Start recording from the default input device
    bool startRecording();

    /// Stop recording
    void stopRecording();

    /// Get ALL recorded audio data (float32 PCM) — for final pass
    std::vector<float> getAudioData();

    /// Get the latest N milliseconds of audio — for live partial transcription
    std::vector<float> getLatestAudio(int duration_ms);

    /// Clear the audio buffer
    void clearBuffer();

    /// Check if currently recording
    bool isRecording() const { return recording_.load(); }

    /// Check if Voice Activity is currently detected
    bool isVoiceActive() const { return voice_active_.load(); }

    /// Check if PortAudio is available (compiled in)
    static bool isAvailable();

    /// Get the sample rate
    int getSampleRate() const { return sample_rate_; }

    /// Get total recorded samples count (thread-safe)
    size_t getSampleCount() const;

    // ── VAD Configuration ────────────────────────────────────────
    void setVadThreshold(float rms) { vad_threshold_ = rms; }
    float getVadThreshold() const { return vad_threshold_; }

private:
#ifdef VISION_HAS_AUDIO
    PaStream* stream_ = nullptr;
#endif
    std::vector<float> buffer_;
    std::mutex buffer_mutex_;
    std::atomic<bool> recording_{false};
    std::atomic<bool> voice_active_{false};
    int sample_rate_;
    int channels_;
    bool pa_initialized_ = false;

    // VAD: RMS threshold — typical human voice range 0.01-0.05
    float vad_threshold_ = 0.015f;

    // VAD: rolling RMS computed in the PortAudio callback
    std::atomic<float> current_rms_{0.0f};

#ifdef VISION_HAS_AUDIO
    /// PortAudio callback (static — must match PaStreamCallback signature)
    static int paCallback(const void* input, void* output,
                          unsigned long frame_count,
                          const PaStreamCallbackTimeInfo* time_info,
                          PaStreamCallbackFlags status_flags,
                          void* user_data);
#endif
};

} // namespace vision
