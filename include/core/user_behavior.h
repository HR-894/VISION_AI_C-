#pragma once
/**
 * @file user_behavior.h
 * @brief User behavior tracking and suggestion engine
 * 
 * Tracks command frequency, app usage patterns, time-of-day preferences,
 * and provides context-aware command suggestions. Persists to JSON.
 */

#include <string>
#include <vector>
#include <utility>
#include <mutex>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace vision {

class UserBehaviorTracker {
public:
    explicit UserBehaviorTracker(const std::string& path = "");
    ~UserBehaviorTracker();  // auto-saves

    // Non-copyable
    UserBehaviorTracker(const UserBehaviorTracker&) = delete;
    UserBehaviorTracker& operator=(const UserBehaviorTracker&) = delete;

    /// Record a command execution with optional active app context
    void recordCommand(const std::string& command, const std::string& app = "");

    /// Record app usage with duration in seconds
    void recordAppUsage(const std::string& app_name, int duration_sec = 1);

    /// Get context-aware suggestions based on time-of-day
    std::vector<std::string> getSuggestions(const std::string& context = "",
                                             int max_count = 5) const;

    /// Get top N most-used commands as (command, count)
    std::vector<std::pair<std::string, int>> getTopCommands(int count = 10) const;

    /// Get top N most-used apps as (app, usage_count)
    std::vector<std::pair<std::string, int>> getTopApps(int count = 5) const;

    /// Get insights JSON (total commands, top commands)
    nlohmann::json getInsights() const;

private:
    nlohmann::json data_;
    std::string file_path_;
    mutable std::mutex mutex_;

    void load();
    void save();

    // Unlocked version for internal use (caller must hold mutex)
    std::vector<std::pair<std::string, int>> getTopCommands_unlocked(int count) const;
};

} // namespace vision
