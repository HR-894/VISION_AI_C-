#include "doctor.h"
#include <filesystem>
#include <sstream>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace fs = std::filesystem;

namespace vision {

DiagnosticResult Doctor::checkModelFile(const std::string& path) {
    DiagnosticResult r;
    r.check_name = "Local LLM Model";
    if (path.empty()) {
        r.passed = false;
        r.message = "No model path configured";
        r.suggestion = "Set the model path in Settings > Model Path, or use the Model Downloader wizard.";
        return r;
    }
    if (!fs::exists(path)) {
        r.passed = false;
        r.message = "Model file not found: " + path;
        r.suggestion = "Download a GGUF model and set the path in Settings.";
        return r;
    }
    try {
        auto size_mb = fs::file_size(path) / (1024 * 1024);
        r.passed = true;
        r.message = "Found: " + path + " (" + std::to_string(size_mb) + " MB)";
    } catch (...) {
        r.passed = true;
        r.message = "Found: " + path + " (size unknown)";
    }
    return r;
}

DiagnosticResult Doctor::checkWhisperModel(const std::string& path) {
    DiagnosticResult r;
    r.check_name = "Whisper Voice Model";
    if (path.empty()) {
        r.passed = false;
        r.message = "No Whisper model path configured";
        r.suggestion = "Voice commands are disabled. Set whisper.model_path in config.";
        return r;
    }
    if (!fs::exists(path)) {
        r.passed = false;
        r.message = "Whisper model not found: " + path;
        r.suggestion = "Download ggml-base.en.bin from huggingface.co/ggerganov/whisper.cpp";
        return r;
    }
    r.passed = true;
    try {
        auto size_mb = fs::file_size(path) / (1024 * 1024);
        r.message = "Found: " + path + " (" + std::to_string(size_mb) + " MB)";
    } catch (...) {
        r.message = "Found: " + path + " (size unknown)";
    }
    return r;
}

DiagnosticResult Doctor::checkGroqApiKey(const std::string& key) {
    DiagnosticResult r;
    r.check_name = "Cloud API Key (Groq)";
    if (key.empty()) {
        r.passed = false;
        r.message = "No Groq API key configured";
        r.suggestion = "Set GROQ_API_KEY env var or enter it in Settings for cloud AI.";
        return r;
    }
    if (key.size() < 20) {
        r.passed = false;
        r.message = "API key looks too short (" + std::to_string(key.size()) + " chars)";
        r.suggestion = "Verify your Groq API key at console.groq.com";
        return r;
    }
    r.passed = true;
    r.message = "Configured (" + std::to_string(key.size()) + " chars)";
    return r;
}

DiagnosticResult Doctor::checkScreenshotDir(const std::string& dir) {
    DiagnosticResult r;
    r.check_name = "Screenshot Directory";
    if (dir.empty()) {
        r.passed = true;
        r.message = "Using default (AI_Workspace/Screenshots)";
        return r;
    }
    if (!fs::exists(dir)) {
        // Try to create it
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) {
            r.passed = false;
            r.message = "Cannot create directory: " + dir;
            r.suggestion = "Check permissions or add to Windows Defender Controlled Folder Access exceptions.";
            return r;
        }
    }
    r.passed = true;
    r.message = "Writable: " + dir;
    return r;
}

DiagnosticResult Doctor::checkGPU(int gpu_layers) {
    DiagnosticResult r;
    r.check_name = "GPU Acceleration";
    if (gpu_layers <= 0) {
        r.passed = false;
        r.message = "GPU offloading disabled (0 layers)";
        r.suggestion = "Set gpu_layers > 0 in Settings for faster inference (needs NVIDIA GPU with CUDA or Vulkan).";
        return r;
    }
    r.passed = true;
    r.message = std::to_string(gpu_layers) + " layers offloaded to GPU";
    return r;
}

DiagnosticResult Doctor::checkMemoryUsage() {
    DiagnosticResult r;
    r.check_name = "System Memory";
    
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        size_t working_mb = pmc.WorkingSetSize / (1024 * 1024);
        size_t private_mb = pmc.PrivateUsage / (1024 * 1024);
        r.message = "Working set: " + std::to_string(working_mb) + " MB, Private: " + std::to_string(private_mb) + " MB";
        r.passed = (working_mb < 16000);  // Warn if > 16GB
        if (!r.passed) {
            r.suggestion = "Memory usage is very high. Consider using a smaller model or reducing context size.";
        }
    } else {
        r.passed = true;
        r.message = "Unable to query (non-critical)";
    }
#else
    r.passed = true;
    r.message = "Not available on this platform";
#endif
    return r;
}

DiagnosticResult Doctor::checkLocalBackend(bool loaded) {
    DiagnosticResult r;
    r.check_name = "Local Backend (llama.cpp)";
    r.passed = loaded;
    r.message = loaded ? "Model loaded and ready" : "Not loaded";
    if (!loaded) r.suggestion = "Ensure model path is correct and press 'Load Model' or restart the app.";
    return r;
}

DiagnosticResult Doctor::checkCloudBackend(bool initialized) {
    DiagnosticResult r;
    r.check_name = "Cloud Backend (Groq)";
    r.passed = initialized;
    r.message = initialized ? "Connected and ready" : "Not initialized";
    if (!initialized) r.suggestion = "Set a valid GROQ_API_KEY and switch to Cloud backend.";
    return r;
}

DiagnosticResult Doctor::checkVectorMemory(size_t entries) {
    DiagnosticResult r;
    r.check_name = "Vector Memory";
    r.passed = true;
    r.message = std::to_string(entries) + " memories stored";
    if (entries > 8000) {
        r.suggestion = "Memory is getting large. Consider purging old entries for performance.";
    }
    return r;
}

DiagnosticResult Doctor::checkConversationHealth(size_t size) {
    DiagnosticResult r;
    r.check_name = "Conversation Context";
    r.passed = (size < 50);
    r.message = std::to_string(size) + " messages in context";
    if (!r.passed) {
        r.suggestion = "Context is very large. Use /clear to reset or let auto-pruning handle it.";
    }
    return r;
}

std::string Doctor::formatReport(const std::vector<DiagnosticResult>& results) {
    std::ostringstream report;
    report << "=== VISION AI Health Report ===\n\n";
    
    int passed = 0, failed = 0;
    for (const auto& r : results) {
        if (r.passed) passed++;
        else failed++;
        
        report << (r.passed ? "[PASS] " : "[FAIL] ");
        report << r.check_name << ": " << r.message << "\n";
        if (!r.passed && !r.suggestion.empty()) {
            report << "       -> " << r.suggestion << "\n";
        }
    }
    
    report << "\n--- Summary: " << passed << " passed, " << failed << " failed ---\n";
    
    if (failed == 0) {
        report << "All systems operational!\n";
    } else {
        report << "Fix the failed checks above for optimal performance.\n";
    }
    
    return report.str();
}

std::string Doctor::runFullDiagnostic(
    const std::string& model_path,
    const std::string& whisper_model_path,
    const std::string& groq_api_key,
    const std::string& screenshot_dir,
    bool local_backend_loaded,
    bool cloud_backend_initialized,
    int gpu_layers,
    size_t memory_entries,
    size_t conversation_size
) {
    std::vector<DiagnosticResult> results;
    
    results.push_back(checkModelFile(model_path));
    results.push_back(checkLocalBackend(local_backend_loaded));
    results.push_back(checkGPU(gpu_layers));
    results.push_back(checkGroqApiKey(groq_api_key));
    results.push_back(checkCloudBackend(cloud_backend_initialized));
    results.push_back(checkWhisperModel(whisper_model_path));
    results.push_back(checkScreenshotDir(screenshot_dir));
    results.push_back(checkMemoryUsage());
    results.push_back(checkVectorMemory(memory_entries));
    results.push_back(checkConversationHealth(conversation_size));
    
    return formatReport(results);
}

} // namespace vision
