#pragma once
/**
 * @file config_manager.h
 * @brief JSON-based configuration management for VISION AI
 * 
 * Handles loading, saving, and accessing application configuration
 * stored in a JSON file. Thread-safe for concurrent reads.
 */

#include <string>
#include <mutex>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace vision {

class ConfigManager {
public:
    /// Construct with optional config file path. Defaults to "data/vision_config.json"
    explicit ConfigManager(const std::string& config_path = "");

    /// Load configuration from disk. Creates defaults if file doesn't exist.
    bool load();

    /// Save current configuration to disk.
    bool save() const;

    /// Get a config value with default fallback
    template <typename T>
    T get(const std::string& key, const T& default_value) const {
        std::lock_guard lock(mutex_);
        if (config_.contains(key)) {
            try { return config_[key].get<T>(); }
            catch (...) { return default_value; }
        }
        return default_value;
    }

    /// Set a config value
    template <typename T>
    void set(const std::string& key, const T& value) {
        std::lock_guard lock(mutex_);
        config_[key] = value;
    }

    /// Get nested config value using dot notation (e.g., "llm.model_path")
    template <typename T>
    T getNested(const std::string& dotted_key, const T& default_value) const {
        std::lock_guard lock(mutex_);
        auto keys = splitDotKey(dotted_key);
        const nlohmann::json* node = &config_;
        for (const auto& k : keys) {
            if (!node->contains(k)) return default_value;
            node = &(*node)[k];
        }
        try { return node->get<T>(); }
        catch (...) { return default_value; }
    }

    /// Get the raw JSON object (const reference)
    const nlohmann::json& raw() const { return config_; }

    /// Get base directory for all data files
    std::string getDataDir() const { return data_dir_; }

    /// Get the app's base directory (where the exe lives)
    static std::string getBaseDir();

private:
    nlohmann::json config_;
    std::string file_path_;
    std::string data_dir_;
    mutable std::mutex mutex_;

    void initDefaults();
    bool saveInternal() const;  // Lock-free save, called from load() to avoid deadlock
    static std::vector<std::string> splitDotKey(const std::string& key);
};

} // namespace vision
