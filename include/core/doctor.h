#pragma once
/**
 * @file doctor.h
 * @brief Self-Diagnosis System — Inspired by OpenClaw's `openclaw doctor`
 *
 * Runs a series of diagnostic checks and reports the health status of
 * all subsystems: model files, GPU, API keys, Whisper, directories,
 * memory usage, and configuration validity.
 */

#include <string>
#include <vector>
#include <functional>

namespace vision {

struct DiagnosticResult {
    std::string check_name;
    bool passed;
    std::string message;
    std::string suggestion;  // Only set if !passed
};

class Doctor {
public:
    /// Run all diagnostics and return a formatted report
    static std::string runFullDiagnostic(
        const std::string& model_path,
        const std::string& whisper_model_path,
        const std::string& groq_api_key,
        const std::string& screenshot_dir,
        bool local_backend_loaded,
        bool cloud_backend_initialized,
        int gpu_layers,
        size_t memory_entries,
        size_t conversation_size
    );

    /// Run individual checks
    static DiagnosticResult checkModelFile(const std::string& path);
    static DiagnosticResult checkWhisperModel(const std::string& path);
    static DiagnosticResult checkGroqApiKey(const std::string& key);
    static DiagnosticResult checkScreenshotDir(const std::string& dir);
    static DiagnosticResult checkGPU(int gpu_layers);
    static DiagnosticResult checkMemoryUsage();
    static DiagnosticResult checkLocalBackend(bool loaded);
    static DiagnosticResult checkCloudBackend(bool initialized);
    static DiagnosticResult checkVectorMemory(size_t entries);
    static DiagnosticResult checkConversationHealth(size_t size);

private:
    static std::string formatReport(const std::vector<DiagnosticResult>& results);
};

} // namespace vision
