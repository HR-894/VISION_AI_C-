/**
 * @file context_manager.cpp
 * @brief Application context tracking and command history
 */

#include "context_manager.h"
#include <algorithm>
#include <chrono>

#ifdef VISION_HAS_SPDLOG
#include <spdlog/spdlog.h>
#define LOG_INFO(...) spdlog::info(__VA_ARGS__)
#else
#define LOG_INFO(...) (void)0
#endif

namespace vision {

ContextManager::ContextManager() = default;

void ContextManager::updateActiveApp(const std::string& title, const std::string& exe) {
    std::lock_guard lock(mutex_);
    active_app_.title = title;
    active_app_.exe_name = exe;
    active_app_.timestamp = std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    // Extract app name from exe
    auto dot = exe.rfind('.');
    active_app_.name = (dot != std::string::npos) ? exe.substr(0, dot) : exe;
    std::transform(active_app_.name.begin(), active_app_.name.end(),
                   active_app_.name.begin(), ::tolower);
}

ContextManager::AppContext ContextManager::getActiveApp() const {
    std::lock_guard lock(mutex_);
    return active_app_;
}

void ContextManager::updateBrowserState(const std::string& url, const std::string& tab_title) {
    std::lock_guard lock(mutex_);
    browser_state_.url = url;
    browser_state_.tab_title = tab_title;
    browser_state_.is_active = true;
}

ContextManager::BrowserState ContextManager::getBrowserState() const {
    std::lock_guard lock(mutex_);
    return browser_state_;
}

void ContextManager::updateFileExplorerState(const std::string& current_path,
                                               const std::vector<std::string>& files) {
    std::lock_guard lock(mutex_);
    explorer_state_.current_path = current_path;
    explorer_state_.visible_files = files;
    explorer_state_.is_active = true;
}

ContextManager::FileExplorerState ContextManager::getFileExplorerState() const {
    std::lock_guard lock(mutex_);
    return explorer_state_;
}

void ContextManager::recordCommand(const std::string& command, const std::string& result) {
    std::lock_guard lock(mutex_);
    nlohmann::json entry = {
        {"command", command},
        {"result", result},
        {"timestamp", std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch()).count()},
        {"app", active_app_.name}
    };
    
    command_history_.push_back(entry);
    
    // Keep bounded
    while (command_history_.size() > static_cast<size_t>(max_history_)) {
        command_history_.erase(command_history_.begin());
    }
}

std::vector<nlohmann::json> ContextManager::getRecentCommands(int count) const {
    std::lock_guard lock(mutex_);
    int n = std::min(count, static_cast<int>(command_history_.size()));
    return std::vector<nlohmann::json>(command_history_.end() - n, command_history_.end());
}

nlohmann::json ContextManager::getFullContext() const {
    std::lock_guard lock(mutex_);
    nlohmann::json ctx;
    ctx["active_app"] = {
        {"name", active_app_.name},
        {"title", active_app_.title},
        {"exe", active_app_.exe_name}
    };
    ctx["browser"] = {
        {"active", browser_state_.is_active},
        {"url", browser_state_.url},
        {"tab", browser_state_.tab_title}
    };
    ctx["explorer"] = {
        {"active", explorer_state_.is_active},
        {"path", explorer_state_.current_path}
    };
    ctx["recent_commands"] = getRecentCommands_unlocked(3);
    return ctx;
}

std::vector<nlohmann::json> ContextManager::getRecentCommands_unlocked(int count) const {
    int n = std::min(count, static_cast<int>(command_history_.size()));
    return std::vector<nlohmann::json>(command_history_.end() - n, command_history_.end());
}

} // namespace vision
