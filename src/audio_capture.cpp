/**
 * @file audio_capture.cpp
 * @brief PortAudio microphone recording
 */

#include "audio_capture.h"

#ifdef VISION_HAS_AUDIO
#include <portaudio.h>
#endif

#ifdef VISION_HAS_SPDLOG
#include <spdlog/spdlog.h>
#define LOG_INFO(...) spdlog::info(__VA_ARGS__)
#define LOG_ERROR(...) spdlog::error(__VA_ARGS__)
#else
#define LOG_INFO(...) (void)0
#define LOG_ERROR(...) (void)0
#endif

namespace vision {

AudioCapture::AudioCapture(int sample_rate, int channels)
    : sample_rate_(sample_rate), channels_(channels) {
#ifdef VISION_HAS_AUDIO
    PaError err = Pa_Initialize();
    if (err == paNoError) {
        pa_initialized_ = true;
    } else {
        LOG_ERROR("PortAudio init failed: {}", Pa_GetErrorText(err));
    }
#endif
}

AudioCapture::~AudioCapture() {
    stopRecording();
#ifdef VISION_HAS_AUDIO
    if (pa_initialized_) {
        Pa_Terminate();
    }
#endif
}

bool AudioCapture::isAvailable() {
#ifdef VISION_HAS_AUDIO
    return true;
#else
    return false;
#endif
}

bool AudioCapture::startRecording() {
#ifdef VISION_HAS_AUDIO
    if (!pa_initialized_) return false;
    if (recording_.load()) return true; // Already recording
    
    clearBuffer();
    
    PaError err = Pa_OpenDefaultStream(
        &stream_, channels_, 0, paFloat32,
        sample_rate_, 256, paCallback, this);
    
    if (err != paNoError) {
        LOG_ERROR("Failed to open audio stream: {}", Pa_GetErrorText(err));
        return false;
    }
    
    err = Pa_StartStream(stream_);
    if (err != paNoError) {
        LOG_ERROR("Failed to start audio stream: {}", Pa_GetErrorText(err));
        Pa_CloseStream(stream_);
        stream_ = nullptr;
        return false;
    }
    
    recording_ = true;
    LOG_INFO("Audio recording started ({}Hz, {} ch)", sample_rate_, channels_);
    return true;
#else
    return false;
#endif
}

void AudioCapture::stopRecording() {
#ifdef VISION_HAS_AUDIO
    if (!recording_.load()) return;
    recording_ = false;
    
    if (stream_) {
        Pa_StopStream(stream_);
        Pa_CloseStream(stream_);
        stream_ = nullptr;
    }
    
    LOG_INFO("Audio recording stopped ({} samples captured)",
             buffer_.size());
#endif
}

std::vector<float> AudioCapture::getAudioData() {
    std::lock_guard lock(buffer_mutex_);
    return buffer_;
}

void AudioCapture::clearBuffer() {
    std::lock_guard lock(buffer_mutex_);
    buffer_.clear();
}

int AudioCapture::paCallback(const void* input, void* /*output*/,
                               unsigned long frame_count,
                               const PaStreamCallbackTimeInfo* /*time_info*/,
                               PaStreamCallbackFlags /*status_flags*/,
                               void* user_data) {
    auto* self = static_cast<AudioCapture*>(user_data);
    const float* in = static_cast<const float*>(input);
    
    if (in && self->recording_.load()) {
        std::lock_guard lock(self->buffer_mutex_);
        self->buffer_.insert(self->buffer_.end(), in, in + frame_count * self->channels_);
    }
    
    return self->recording_.load() ? 0 : 1; // paContinue : paComplete
}

} // namespace vision
