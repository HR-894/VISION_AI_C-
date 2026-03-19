// =============================================================================
// VISION AI - AudioCapture.cpp
// PortAudio-based microphone capture with lock-free ring buffer + VAD
// =============================================================================
#include "AudioCapture.h"

#include <QDebug>
#include <QDateTime>

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
    // Called from PortAudio real-time thread — must be lock-free.
    size_t writePos = m_writePos.load(std::memory_order_relaxed);

    for (size_t i = 0; i < sampleCount; ++i) {
        m_buffer[(writePos + i) % RING_BUFFER_SAMPLES] = data[i];
    }

    // Single atomic store — makes all writes visible to consumer
    m_writePos.store(writePos + sampleCount, std::memory_order_release);
}

size_t AudioRingBuffer::readLast(float* output, size_t sampleCount) const noexcept
{
    size_t writePos = m_writePos.load(std::memory_order_acquire);

    // Clamp to available data
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

    // If consumer fell behind by more than the buffer size, skip ahead
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

AudioCapture::AudioCapture(QObject* parent)
    : QObject(parent)
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
    qWarning() << "[AudioCapture] PortAudio not available.";
    emit captureError("PortAudio library not available.");
    return false;
#else
    if (m_initialized.load(std::memory_order_acquire)) return true;

    PaError err = Pa_Initialize();
    if (err != paNoError) {
        qCritical() << "[AudioCapture] Pa_Initialize failed:"
                     << Pa_GetErrorText(err);
        emit captureError(QString("PortAudio init failed: %1")
                              .arg(Pa_GetErrorText(err)));
        return false;
    }

    m_initialized.store(true, std::memory_order_release);
    qInfo() << "[AudioCapture] PortAudio initialized. Version:"
            << Pa_GetVersionText();

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
        qInfo() << "[AudioCapture] PortAudio terminated.";
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
        qWarning() << "[AudioCapture] Already recording.";
        return true;
    }

    // Clear buffers
    m_ringBuffer.clear();
    {
        QMutexLocker lock(&m_fullRecMutex);
        m_fullRecording.clear();
        m_fullRecording.reserve(SAMPLE_RATE * 60);  // Pre-alloc 60 seconds
    }

    // Reset VAD
    m_vadState = VADState{};
    m_vadHoldCounter = 0;

    // Open stream
    PaStreamParameters inputParams{};
    inputParams.device = (m_deviceIndex >= 0)
        ? m_deviceIndex
        : Pa_GetDefaultInputDevice();

    if (inputParams.device == paNoDevice) {
        emit captureError("No input audio device found.");
        return false;
    }

    inputParams.channelCount = CHANNELS;
    inputParams.sampleFormat = paFloat32;
    inputParams.suggestedLatency =
        Pa_GetDeviceInfo(inputParams.device)->defaultLowInputLatency;
    inputParams.hostApiSpecificStreamInfo = nullptr;

    PaError err = Pa_OpenStream(
        reinterpret_cast<PaStream**>(&m_paStream),
        &inputParams,
        nullptr,            // No output
        SAMPLE_RATE,
        FRAMES_PER_CHUNK,
        paClipOff,          // Don't clip
        reinterpret_cast<PaStreamCallback*>(&AudioCapture::paStreamCallback),
        this                // userData = this
    );

    if (err != paNoError) {
        qCritical() << "[AudioCapture] Pa_OpenStream failed:"
                     << Pa_GetErrorText(err);
        emit captureError(QString("Failed to open audio stream: %1")
                              .arg(Pa_GetErrorText(err)));
        return false;
    }

    err = Pa_StartStream(reinterpret_cast<PaStream*>(m_paStream));
    if (err != paNoError) {
        Pa_CloseStream(reinterpret_cast<PaStream*>(m_paStream));
        m_paStream = nullptr;
        qCritical() << "[AudioCapture] Pa_StartStream failed:"
                     << Pa_GetErrorText(err);
        emit captureError(QString("Failed to start audio stream: %1")
                              .arg(Pa_GetErrorText(err)));
        return false;
    }

    m_recording.store(true, std::memory_order_release);
    qInfo() << "[AudioCapture] Recording started on device"
            << inputParams.device;

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

    // If speech was active, emit speechEnded
    if (m_vadState.isSpeechActive) {
        int duration = static_cast<int>(m_vadState.speechDurationMs);
        m_vadState.isSpeechActive = false;
        emit speechEnded(duration);
    }

    qInfo() << "[AudioCapture] Recording stopped.";
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

std::vector<float> AudioCapture::getFullRecording() const
{
    QMutexLocker lock(&m_fullRecMutex);
    return m_fullRecording;  // Copy
}

// =========================================================================
// VAD
// =========================================================================

const VADState& AudioCapture::getVADState() const noexcept
{
    return m_vadState;
}

void AudioCapture::setVADThreshold(float threshold)
{
    m_vadState.threshold = std::max(0.0001f, threshold);
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
    // Ensure PA is initialized temporarily
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
// PortAudio Stream Callback (REAL-TIME — no allocations, no locks, no Qt)
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

    // Write to lock-free ring buffer (real-time safe)
    self->m_ringBuffer.write(samples, count);

    // Accumulate into full recording buffer
    // NOTE: This mutex is the only non-RT-safe operation. In practice the
    // consumer (getFullRecording) is only called once at the end, so contention
    // is near-zero. For absolute RT safety, this could be replaced with a
    // second lock-free queue, but the complexity isn't worth it for this use case.
    {
        QMutexLocker lock(&self->m_fullRecMutex);
        self->m_fullRecording.insert(
            self->m_fullRecording.end(), samples, samples + count);
    }

    // Process VAD (cheap — just RMS energy computation)
    self->processVAD(samples, count);

    return paContinue;
}

// =========================================================================
// VAD Processing
// =========================================================================

void AudioCapture::processVAD(const float* samples, size_t count)
{
    if (!m_vadEnabled) return;

    float energy = computeRMSEnergy(samples, count);
    m_vadState.currentEnergy = energy;

    // Update noise floor estimate
    updateNoiseFloor(energy);

    // Adaptive threshold: 3x noise floor, but at least the configured minimum
    float adaptiveThreshold = std::max(m_vadState.threshold,
                                        m_vadState.noiseFloor * 3.0f);

    // Emit audio level for VU meter (called from callback — will be queued)
    emit audioLevelChanged(energy);

    bool speechDetected = (energy > adaptiveThreshold);

    if (speechDetected) {
        m_vadHoldCounter = VAD_HOLD_FRAMES;  // Reset hold timer

        if (!m_vadState.isSpeechActive) {
            // Speech onset
            m_vadState.isSpeechActive = true;
            m_vadState.speechStartMs = QDateTime::currentMSecsSinceEpoch();
            m_vadState.peakEnergy = energy;
            emit speechStarted();
        } else {
            // Continuing speech
            m_vadState.peakEnergy = std::max(m_vadState.peakEnergy, energy);
            m_vadState.speechDurationMs =
                QDateTime::currentMSecsSinceEpoch() - m_vadState.speechStartMs;
        }
    } else if (m_vadState.isSpeechActive) {
        // Speech might be ending — use hold timer to bridge brief pauses
        m_vadHoldCounter--;
        m_vadState.speechDurationMs =
            QDateTime::currentMSecsSinceEpoch() - m_vadState.speechStartMs;

        if (m_vadHoldCounter <= 0) {
            // Speech ended
            int duration = static_cast<int>(m_vadState.speechDurationMs);
            m_vadState.isSpeechActive = false;
            m_vadState.peakEnergy = 0.0f;
            emit speechEnded(duration);
        }
    }
}

float AudioCapture::computeRMSEnergy(
    const float* samples, size_t count) noexcept
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
    // Exponential moving average with very slow decay
    // Only update when energy is below current estimate * 2
    // (avoids corrupting noise floor during speech)
    if (energy < m_vadState.noiseFloor * 2.0f) {
        constexpr float alpha = 0.005f;  // Very slow adaptation
        m_vadState.noiseFloor =
            m_vadState.noiseFloor * (1.0f - alpha) + energy * alpha;
    }
}

} // namespace vision::voice
