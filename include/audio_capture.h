#pragma once
/**
 * @file audio_capture.h
 * @brief Microphone audio capture using PortAudio
 * 
 * Records audio from the default input device into a float buffer.
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

    /// Get recorded audio data (float32 PCM)
    std::vector<float> getAudioData();

    /// Clear the audio buffer
    void clearBuffer();

    /// Check if currently recording
    bool isRecording() const { return recording_.load(); }

    /// Check if PortAudio is available (compiled in)
    static bool isAvailable();

    /// Get the sample rate
    int getSampleRate() const { return sample_rate_; }

private:
#ifdef VISION_HAS_AUDIO
    PaStream* stream_ = nullptr;
#endif
    std::vector<float> buffer_;
    std::mutex buffer_mutex_;
    std::atomic<bool> recording_{false};
    int sample_rate_;
    int channels_;
    bool pa_initialized_ = false;

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
