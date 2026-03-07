/**
 * @file config_manager.cpp
 * @brief JSON configuration management implementation
 */

#include "config_manager.h"
#include <fstream>
#include <filesystem>
#include <windows.h>

#ifdef VISION_HAS_SPDLOG
#include <spdlog/spdlog.h>
#define LOG_INFO(...) spdlog::info(__VA_ARGS__)
#define LOG_WARN(...) spdlog::warn(__VA_ARGS__)
#define LOG_ERROR(...) spdlog::error(__VA_ARGS__)
#else
#define LOG_INFO(...) (void)0
#define LOG_WARN(...) (void)0
#define LOG_ERROR(...) (void)0
#endif

namespace fs = std::filesystem;

namespace vision {

std::string ConfigManager::getBaseDir() {
    // If running from a frozen exe, use exe directory
    char path[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        LOG_WARN("GetModuleFileNameA failed or path truncated, using current dir");
        return ".";
    }
    return fs::path(path).parent_path().string();
}

ConfigManager::ConfigManager(const std::string& config_path) {
    std::string base = getBaseDir();
    data_dir_ = (fs::path(base) / "data").string();
    
    if (config_path.empty()) {
        file_path_ = (fs::path(data_dir_) / "vision_config.json").string();
    } else {
        file_path_ = config_path;
    }
    
    // Ensure data directory exists
    fs::create_directories(data_dir_);
    
    initDefaults();
}

void ConfigManager::initDefaults() {
    config_ = {
        {"hotkey", "ctrl+win"},
        {"whisper_model", "base"},
        {"run_on_startup", false},
        {"theme", "dark"},
        {"language", "en"},
        {"engine_mode", "local"},
        {"cloud_api_key_encrypted", ""},
        {"cloud", {
            {"model", "llama-3.3-70b-versatile"}
        }},
        {"llm", {
            {"model_path", ""},
            {"gpu_layers", 0},
            {"context_size", 2048},
            {"temperature", 0.1},
            {"top_p", 0.9},
            {"timeout", 30}
        }},
        {"audio", {
            {"sample_rate", 16000},
            {"channels", 1}
        }},
        {"ocr", {
            {"cache_max_age", 2.0},
            {"min_confidence", 50}
        }},
        {"agent", {
            {"max_steps", 10},
            {"step_timeout", 15}
        }}
    };
}

bool ConfigManager::load() {
    std::lock_guard lock(mutex_);
    
    if (!fs::exists(file_path_)) {
        LOG_INFO("Config file not found, using defaults: {}", file_path_);
        return saveInternal(); // Lock-free inner method to avoid deadlock
    }
    
    try {
        std::ifstream file(file_path_);
        if (!file.is_open()) {
            LOG_ERROR("Failed to open config file: {}", file_path_);
            return false;
        }
        
        nlohmann::json loaded;
        file >> loaded;
        
        // Merge loaded values over defaults (preserving any new default keys)
        for (auto& [key, val] : loaded.items()) {
            config_[key] = val;
        }
        
        LOG_INFO("Config loaded from {}", file_path_);
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to parse config: {}", e.what());
        return false;
    }
}

bool ConfigManager::save() const {
    std::lock_guard lock(mutex_);
    return saveInternal();
}

bool ConfigManager::saveInternal() const {
    try {
        fs::create_directories(fs::path(file_path_).parent_path());
        
        std::ofstream file(file_path_);
        if (!file.is_open()) {
            LOG_ERROR("Failed to write config: {}", file_path_);
            return false;
        }
        
        file << config_.dump(4) << std::flush;
        if (!file.good()) {
            LOG_ERROR("Config write failed (I/O error): {}", file_path_);
            return false;
        }
        LOG_INFO("Config saved to {}", file_path_);
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to save config: {}", e.what());
        return false;
    }
}

std::vector<std::string> ConfigManager::splitDotKey(const std::string& key) {
    std::vector<std::string> parts;
    std::string current;
    for (char c : key) {
        if (c == '.') {
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
        } else {
            current += c;
        }
    }
    if (!current.empty()) parts.push_back(current);
    return parts;
}

} // namespace vision
