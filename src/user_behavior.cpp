/**
 * @file user_behavior.cpp
 * @brief User behavior tracking and suggestion engine
 */

#include "user_behavior.h"
#include <fstream>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace vision {

UserBehaviorTracker::UserBehaviorTracker(const std::string& path)
    : file_path_(path.empty() ? "data/user_behavior.json" : path) {
    load();
}

UserBehaviorTracker::~UserBehaviorTracker() { save(); }

void UserBehaviorTracker::load() {
    std::lock_guard lock(mutex_);
    if (!fs::exists(file_path_)) {
        data_ = {{"commands", json::object()}, {"apps", json::object()},
                 {"time_patterns", json::object()}, {"sessions", 0}};
        return;
    }
    try { std::ifstream f(file_path_); f >> data_; }
    catch (...) { data_ = json::object(); }
}

void UserBehaviorTracker::save() {
    std::lock_guard lock(mutex_);
    try {
        fs::create_directories(fs::path(file_path_).parent_path());
        std::ofstream f(file_path_);
        f << data_.dump(2) << std::flush;
    } catch (...) {}
}

void UserBehaviorTracker::recordCommand(const std::string& command, const std::string& app) {
    std::lock_guard lock(mutex_);
    
    std::string lower = command;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    
    // Command frequency
    int count = data_["commands"].value(lower, 0);
    data_["commands"][lower] = count + 1;
    
    // App usage
    if (!app.empty()) {
        int app_count = data_["apps"].value(app, 0);
        data_["apps"][app] = app_count + 1;
    }
    
    // Time pattern
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm; localtime_s(&tm, &tt);
    int hour = tm.tm_hour;
    std::string period;
    if (hour >= 6 && hour < 12) period = "morning";
    else if (hour >= 12 && hour < 17) period = "afternoon";
    else if (hour >= 17 && hour < 22) period = "evening";
    else period = "night";
    
    if (!data_["time_patterns"].contains(period))
        data_["time_patterns"][period] = json::object();
    
    int tp_count = data_["time_patterns"][period].value(lower, 0);
    data_["time_patterns"][period][lower] = tp_count + 1;
}

void UserBehaviorTracker::recordAppUsage(const std::string& app_name, int duration_sec) {
    std::lock_guard lock(mutex_);
    std::string lower = app_name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    int total = data_["apps"].value(lower, 0);
    data_["apps"][lower] = total + duration_sec;
}

std::vector<std::string> UserBehaviorTracker::getSuggestions(const std::string& context,
                                                              int max_count) const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> suggestions;
    
    // Get current time period
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm; localtime_s(&tm, &tt);
    int hour = tm.tm_hour;
    std::string period;
    if (hour >= 6 && hour < 12) period = "morning";
    else if (hour >= 12 && hour < 17) period = "afternoon";
    else if (hour >= 17 && hour < 22) period = "evening";
    else period = "night";
    
    // Get frequently used commands for this time period
    std::vector<std::pair<int, std::string>> ranked;
    
    if (data_.contains("time_patterns") && data_["time_patterns"].contains(period)) {
        for (auto& [cmd, count] : data_["time_patterns"][period].items()) {
            ranked.emplace_back(count.get<int>(), cmd);
        }
    }
    
    std::sort(ranked.begin(), ranked.end(), std::greater<>());
    
    for (int i = 0; i < std::min(max_count, (int)ranked.size()); i++) {
        suggestions.push_back(ranked[i].second);
    }
    
    return suggestions;
}

std::vector<std::pair<std::string, int>> UserBehaviorTracker::getTopCommands(int count) const {
    std::lock_guard lock(mutex_);
    std::vector<std::pair<std::string, int>> ranked;
    
    if (data_.contains("commands")) {
        for (auto& [cmd, cnt] : data_["commands"].items()) {
            ranked.emplace_back(cmd, cnt.get<int>());
        }
    }
    
    std::sort(ranked.begin(), ranked.end(),
              [](auto& a, auto& b) { return a.second > b.second; });
    
    if ((int)ranked.size() > count) ranked.resize(count);
    return ranked;
}

std::vector<std::pair<std::string, int>> UserBehaviorTracker::getTopApps(int count) const {
    std::lock_guard lock(mutex_);
    std::vector<std::pair<std::string, int>> ranked;
    
    if (data_.contains("apps")) {
        for (auto& [app, cnt] : data_["apps"].items()) {
            ranked.emplace_back(app, cnt.get<int>());
        }
    }
    
    std::sort(ranked.begin(), ranked.end(),
              [](auto& a, auto& b) { return a.second > b.second; });
    
    if ((int)ranked.size() > count) ranked.resize(count);
    return ranked;
}

json UserBehaviorTracker::getInsights() const {
    std::lock_guard lock(mutex_);
    json insights;
    insights["total_commands"] = 0;
    
    if (data_.contains("commands")) {
        int total = 0;
        for (auto& [_, v] : data_["commands"].items()) total += v.get<int>();
        insights["total_commands"] = total;
    }
    
    // Top 5 commands
    auto top_cmds = getTopCommands_unlocked(5);
    insights["top_commands"] = json::array();
    for (auto& [cmd, cnt] : top_cmds) {
        insights["top_commands"].push_back({{"command", cmd}, {"count", cnt}});
    }
    
    return insights;
}

std::vector<std::pair<std::string, int>> UserBehaviorTracker::getTopCommands_unlocked(int count) const {
    std::vector<std::pair<std::string, int>> ranked;
    if (data_.contains("commands")) {
        for (auto& [cmd, cnt] : data_["commands"].items()) {
            ranked.emplace_back(cmd, cnt.get<int>());
        }
    }
    std::sort(ranked.begin(), ranked.end(),
              [](auto& a, auto& b) { return a.second > b.second; });
    if ((int)ranked.size() > count) ranked.resize(count);
    return ranked;
}

} // namespace vision
