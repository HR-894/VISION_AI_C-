// =============================================================================
// VISION AI - AudioCapture.cpp
// PortAudio-based microphone capture with lock-free ring buffer + VAD (No Qt)
// =============================================================================
#include "AudioCapture.h"

#include <spdlog/spdlog.h>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

// PortAudio (conditional)
#ifndef VISION_NO_PORTAUDIO
#include <portaudio.h>
#endif

namespace vision::voice {

// =========================================================================
// AudioRingBuffer Implementation
// =========================================================================

AudioRingBuffer::AudioRingBuffer()
{
    m_buffer.fill(0.0f);
}

void AudioRingBuffer::write(const float* data, size_t sampleCount) noexcept
{
    size_t writePos = m_writePos.load(std::memory_order_relaxed);

    for (size_t i = 0; i < sampleCount; ++i) {
        m_buffer[(writePos + i) % RING_BUFFER_SAMPLES] = data[i];
    }

    m_writePos.store(writePos + sampleCount, std::memory_order_release);
}

size_t AudioRingBuffer::readLast(float* output, size_t sampleCount) const noexcept
{
    size_t writePos = m_writePos.load(std::memory_order_acquire);

    size_t available = std::min(sampleCount, writePos);
    if (available == 0) return 0;

    size_t startPos = writePos - available;

    for (size_t i = 0; i < available; ++i) {
        output[i] = m_buffer[(startPos + i) % RING_BUFFER_SAMPLES];
    }

    return available;
}

size_t AudioRingBuffer::readNew(float* output, size_t maxSamples) noexcept
{
    size_t writePos = m_writePos.load(std::memory_order_acquire);

    if (writePos <= m_readPos) return 0;

    size_t newSamples = writePos - m_readPos;
    size_t toRead = std::min(newSamples, maxSamples);

    if (newSamples > RING_BUFFER_SAMPLES) {
        m_readPos = writePos - RING_BUFFER_SAMPLES;
        newSamples = RING_BUFFER_SAMPLES;
        toRead = std::min(newSamples, maxSamples);
    }

    for (size_t i = 0; i < toRead; ++i) {
        output[i] = m_buffer[(m_readPos + i) % RING_BUFFER_SAMPLES];
    }

    m_readPos += toRead;
    return toRead;
}

size_t AudioRingBuffer::totalWritten() const noexcept
{
    return m_writePos.load(std::memory_order_acquire);
}

size_t AudioRingBuffer::available() const noexcept
{
    size_t writePos = m_writePos.load(std::memory_order_acquire);
    return (writePos > m_readPos) ? (writePos - m_readPos) : 0;
}

void AudioRingBuffer::clear() noexcept
{
    m_writePos.store(0, std::memory_order_release);
    m_readPos = 0;
}

// =========================================================================
// AudioCapture: Construction / Destruction
// =========================================================================

AudioCapture::AudioCapture()
{
}

AudioCapture::~AudioCapture()
{
    shutdown();
}

// =========================================================================
// Lifecycle
// =========================================================================

bool AudioCapture::initialize()
{
#ifdef VISION_NO_PORTAUDIO
    spdlog::warn("[AudioCapture] PortAudio not available.");
    notifyError("PortAudio library not available.");
    return false;
#else
    if (m_initialized.load(std::memory_order_acquire)) return true;

    PaError err = Pa_Initialize();
    if (err != paNoError) {
        spdlog::critical("[AudioCapture] Pa_Initialize failed: {}", Pa_GetErrorText(err));
        notifyError(std::string("PortAudio init failed: ") + Pa_GetErrorText(err));
        return false;
    }

    m_initialized.store(true, std::memory_order_release);
    spdlog::info("[AudioCapture] PortAudio initialized. Version: {}", Pa_GetVersionText());

    return true;
#endif
}

void AudioCapture::shutdown()
{
#ifndef VISION_NO_PORTAUDIO
    stopRecording();

    if (m_initialized.load(std::memory_order_acquire)) {
        Pa_Terminate();
        m_initialized.store(false, std::memory_order_release);
        spdlog::info("[AudioCapture] PortAudio terminated.");
    }
#endif
}

// =========================================================================
// Recording Control
// =========================================================================

bool AudioCapture::startRecording()
{
#ifdef VISION_NO_PORTAUDIO
    return false;
#else
    if (!m_initialized.load(std::memory_order_acquire)) {
        if (!initialize()) return false;
    }

    if (m_recording.load(std::memory_order_acquire)) {
        spdlog::warn("[AudioCapture] Already recording.");
        return true;
    }

    m_ringBuffer.clear();

    m_vadIsSpeechActive.store(false, std::memory_order_relaxed);
    m_vadHoldCounter = 0;

    PaStreamParameters inputParams{};
    inputParams.device = (m_deviceIndex >= 0)
        ? m_deviceIndex
        : Pa_GetDefaultInputDevice();

    if (inputParams.device == paNoDevice) {
        notifyError("No input audio device found.");
        return false;
    }

    inputParams.channelCount = CHANNELS;
    inputParams.sampleFormat = paFloat32;
    inputParams.suggestedLatency = Pa_GetDeviceInfo(inputParams.device)->defaultLowInputLatency;
    inputParams.hostApiSpecificStreamInfo = nullptr;

    PaError err = Pa_OpenStream(
        reinterpret_cast<PaStream**>(&m_paStream),
        &inputParams,
        nullptr,            // No output
        SAMPLE_RATE,
        FRAMES_PER_CHUNK,
        paClipOff,          // Don't clip
        reinterpret_cast<PaStreamCallback*>(&AudioCapture::paStreamCallback),
        this
    );

    if (err != paNoError) {
        spdlog::critical("[AudioCapture] Pa_OpenStream failed: {}", Pa_GetErrorText(err));
        notifyError(std::string("Failed to open audio stream: ") + Pa_GetErrorText(err));
        return false;
    }

    err = Pa_StartStream(reinterpret_cast<PaStream*>(m_paStream));
    if (err != paNoError) {
        Pa_CloseStream(reinterpret_cast<PaStream*>(m_paStream));
        m_paStream = nullptr;
        spdlog::critical("[AudioCapture] Pa_StartStream failed: {}", Pa_GetErrorText(err));
        notifyError(std::string("Failed to start audio stream: ") + Pa_GetErrorText(err));
        return false;
    }

    m_recording.store(true, std::memory_order_release);
    spdlog::info("[AudioCapture] Recording started on device {}", inputParams.device);

    return true;
#endif
}

void AudioCapture::stopRecording()
{
#ifndef VISION_NO_PORTAUDIO
    if (!m_recording.load(std::memory_order_acquire)) return;

    m_recording.store(false, std::memory_order_release);

    if (m_paStream) {
        Pa_StopStream(reinterpret_cast<PaStream*>(m_paStream));
        Pa_CloseStream(reinterpret_cast<PaStream*>(m_paStream));
        m_paStream = nullptr;
    }

    if (m_vadIsSpeechActive.load(std::memory_order_relaxed)) {
        int duration = static_cast<int>(m_vadSpeechDurationMs.load(std::memory_order_relaxed));
        m_vadIsSpeechActive.store(false, std::memory_order_relaxed);
        if (onSpeechEnded) {
            onSpeechEnded(duration);
        }
    }

    spdlog::info("[AudioCapture] Recording stopped.");
#endif
}

bool AudioCapture::isRecording() const noexcept
{
    return m_recording.load(std::memory_order_acquire);
}

// =========================================================================
// Data Access
// =========================================================================

std::vector<float> AudioCapture::getLastNSeconds(float seconds) const
{
    size_t samples = static_cast<size_t>(seconds * SAMPLE_RATE);
    std::vector<float> output(samples);

    size_t actual = m_ringBuffer.readLast(output.data(), samples);
    output.resize(actual);
    return output;
}

std::vector<float> AudioCapture::getNewAudio()
{
    size_t avail = m_ringBuffer.available();
    if (avail == 0) return {};

    std::vector<float> output(avail);
    size_t actual = m_ringBuffer.readNew(output.data(), avail);
    output.resize(actual);
    return output;
}

// =========================================================================
// VAD
// =========================================================================

VADState AudioCapture::getVADState() const noexcept
{
    VADState state;
    state.isSpeechActive = m_vadIsSpeechActive.load(std::memory_order_relaxed);
    state.currentEnergy = m_vadCurrentEnergy.load(std::memory_order_relaxed);
    state.peakEnergy = m_vadPeakEnergy.load(std::memory_order_relaxed);
    state.speechStartMs = m_vadSpeechStartMs.load(std::memory_order_relaxed);
    state.speechDurationMs = m_vadSpeechDurationMs.load(std::memory_order_relaxed);
    state.noiseFloor = m_vadNoiseFloor.load(std::memory_order_relaxed);
    state.threshold = m_vadThreshold.load(std::memory_order_relaxed);
    return state;
}

void AudioCapture::setVADThreshold(float threshold)
{
    m_vadThreshold.store(std::max(0.0001f, threshold), std::memory_order_relaxed);
}

void AudioCapture::setVADEnabled(bool enabled)
{
    m_vadEnabled = enabled;
}

// =========================================================================
// Device Enumeration
// =========================================================================

std::vector<AudioCapture::AudioDevice> AudioCapture::listInputDevices()
{
    std::vector<AudioDevice> devices;

#ifndef VISION_NO_PORTAUDIO
    bool wasInit = (Pa_Initialize() == paNoError);

    int defaultDevice = Pa_GetDefaultInputDevice();
    int numDevices = Pa_GetDeviceCount();

    for (int i = 0; i < numDevices; ++i) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (!info || info->maxInputChannels <= 0) continue;

        AudioDevice dev;
        dev.index             = i;
        dev.name              = info->name ? info->name : "Unknown";
        dev.maxInputChannels  = info->maxInputChannels;
        dev.defaultSampleRate = info->defaultSampleRate;
        dev.isDefault         = (i == defaultDevice);
        devices.push_back(std::move(dev));
    }

    if (wasInit) Pa_Terminate();
#endif

    return devices;
}

void AudioCapture::setInputDevice(int deviceIndex)
{
    m_deviceIndex = deviceIndex;
}

// =========================================================================
// PortAudio Stream Callback (REAL-TIME)
// =========================================================================

int AudioCapture::paStreamCallback(
    const void* inputBuffer,
    void* /*outputBuffer*/,
    unsigned long framesPerBuffer,
    const void* /*timeInfo*/,
    unsigned long /*statusFlags*/,
    void* userData)
{
    auto* self = static_cast<AudioCapture*>(userData);

    if (!inputBuffer) return paContinue;

    const float* samples = static_cast<const float*>(inputBuffer);
    size_t count = static_cast<size_t>(framesPerBuffer);

    self->m_ringBuffer.write(samples, count);
    self->processVAD(samples, count);

    return paContinue;
}

void AudioCapture::processVAD(const float* samples, size_t count)
{
    if (!m_vadEnabled) return;

    float energy = computeRMSEnergy(samples, count);
    m_vadCurrentEnergy.store(energy, std::memory_order_relaxed);

    updateNoiseFloor(energy);

    float noiseFloor = m_vadNoiseFloor.load(std::memory_order_relaxed);
    float configuredThreshold = m_vadThreshold.load(std::memory_order_relaxed);
    float adaptiveThreshold = std::max(configuredThreshold, noiseFloor * 3.0f);

    if (onAudioLevelChanged) {
        onAudioLevelChanged(energy);
    }

    bool speechDetected = (energy > adaptiveThreshold);

    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    if (speechDetected) {
        m_vadHoldCounter = VAD_HOLD_FRAMES;

        if (!m_vadIsSpeechActive.load(std::memory_order_relaxed)) {
            m_vadIsSpeechActive.store(true, std::memory_order_relaxed);
            m_vadSpeechStartMs.store(now, std::memory_order_relaxed);
            m_vadPeakEnergy.store(energy, std::memory_order_relaxed);
            if (onSpeechStarted) {
                onSpeechStarted();
            }
        } else {
            float currentPeak = m_vadPeakEnergy.load(std::memory_order_relaxed);
            m_vadPeakEnergy.store(std::max(currentPeak, energy), std::memory_order_relaxed);
            m_vadSpeechDurationMs.store(now - m_vadSpeechStartMs.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
    } else if (m_vadIsSpeechActive.load(std::memory_order_relaxed)) {
        m_vadHoldCounter--;
        m_vadSpeechDurationMs.store(now - m_vadSpeechStartMs.load(std::memory_order_relaxed), std::memory_order_relaxed);

        if (m_vadHoldCounter <= 0) {
            int duration = static_cast<int>(m_vadSpeechDurationMs.load(std::memory_order_relaxed));
            m_vadIsSpeechActive.store(false, std::memory_order_relaxed);
            m_vadPeakEnergy.store(0.0f, std::memory_order_relaxed);
            if (onSpeechEnded) {
                onSpeechEnded(duration);
            }
        }
    }
}

float AudioCapture::computeRMSEnergy(const float* samples, size_t count) noexcept
{
    if (count == 0) return 0.0f;

    float sum = 0.0f;
    for (size_t i = 0; i < count; ++i) {
        sum += samples[i] * samples[i];
    }
    return std::sqrt(sum / static_cast<float>(count));
}

void AudioCapture::updateNoiseFloor(float energy) noexcept
{
    float currentFloor = m_vadNoiseFloor.load(std::memory_order_relaxed);
    if (energy < currentFloor * 2.0f) {
        constexpr float alpha = 0.005f; 
        m_vadNoiseFloor.store(currentFloor * (1.0f - alpha) + energy * alpha, std::memory_order_relaxed);
    }
}

void AudioCapture::notifyError(const std::string& err) {
    if (onCaptureError) {
        onCaptureError(err);
    }
}

} // namespace vision::voice
