/**
 * @file safety_guard.cpp
 * @brief Path protection and action safety validation
 */

#include "safety_guard.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <regex>
#include <windows.h> // Required for GetWindowsDirectoryA

namespace fs = std::filesystem;

namespace vision {

// Protected system paths that should never be modified
// Dynamically detect the system drive for protected paths
static std::vector<std::string> buildProtectedPaths() {
    char windir[MAX_PATH];
    UINT len = GetWindowsDirectoryA(windir, MAX_PATH);
    std::string drive = (len > 0) ? std::string(windir, 2) + "\\" : "C:\\";
    return {
        drive + "Windows",
        drive + "Windows\\System32",
        drive + "Windows\\SysWOW64",
        drive + "Program Files",
        drive + "Program Files (x86)",
        drive + "ProgramData",
        drive + "Users\\Default",
        drive + "Users\\Public",
        drive + "Recovery",
        drive + "Boot",
        drive + "$Recycle.Bin",
        drive + "System Volume Information", // Added back from original list
        drive + "EFI", // Added back from original list
    };
}

const std::vector<std::string> SafetyGuard::PROTECTED_PATHS = buildProtectedPaths();

const std::vector<std::string> SafetyGuard::CAUTION_ACTIONS = {
    "move", "rename", "copy_to_system", "modify_config",
};

const std::vector<std::string> SafetyGuard::DANGER_ACTIONS = {
    "delete", "format", "remove", "wipe", "destroy",
    "kill_process", "end_task", "shutdown", "restart",
};

const std::vector<std::string> SafetyGuard::PROTECTED_PROCESSES = {
    "explorer.exe", "svchost.exe", "csrss.exe", "wininit.exe",
    "winlogon.exe", "lsass.exe", "services.exe", "smss.exe",
    "dwm.exe", "System", "Registry", "taskmgr.exe",
};

SafetyGuard::SafetyGuard() = default;

std::string SafetyGuard::normalizePath(const std::string& path) const {
    try {
        fs::path p(path);
        if (p.is_relative()) {
            p = fs::absolute(p);
        }
        // Use lexically_normal to clean up .. and .
        std::string normalized = p.lexically_normal().string();
        // Uppercase drive letter
        if (normalized.size() >= 2 && normalized[1] == ':') {
            normalized[0] = static_cast<char>(std::toupper(normalized[0]));
        }
        return normalized;
    } catch (...) {
        return path;
    }
}

bool SafetyGuard::matchesPattern(const std::string& path,
                                  const std::string& pattern) const {
    std::string norm_path = normalizePath(path);
    std::string norm_pattern = normalizePath(pattern);
    
    // Case-insensitive prefix match
    std::string lower_path = norm_path;
    std::string lower_pattern = norm_pattern;
    std::transform(lower_path.begin(), lower_path.end(), lower_path.begin(), ::tolower);
    std::transform(lower_pattern.begin(), lower_pattern.end(), lower_pattern.begin(), ::tolower);
    
    return lower_path.starts_with(lower_pattern);
}

bool SafetyGuard::isPathProtected(const std::string& path) const {
    for (const auto& protected_path : PROTECTED_PATHS) {
        if (matchesPattern(path, protected_path)) {
            return true;
        }
    }
    return false;
}

SafetyGuard::ActionLevel SafetyGuard::getActionLevel(const std::string& command) const {
    std::string lower = command;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    
    for (const auto& word : DANGER_ACTIONS) {
        if (lower.find(word) != std::string::npos) {
            return ActionLevel::Danger;
        }
    }
    
    for (const auto& word : CAUTION_ACTIONS) {
        if (lower.find(word) != std::string::npos) {
            return ActionLevel::Caution;
        }
    }
    
    return ActionLevel::Safe;
}

std::string SafetyGuard::getActionLevelString(const std::string& command) const {
    switch (getActionLevel(command)) {
        case ActionLevel::Safe: return "SAFE";
        case ActionLevel::Caution: return "CAUTION";
        case ActionLevel::Danger: return "DANGER";
    }
    return "UNKNOWN";
}

std::pair<bool, std::string> SafetyGuard::validateFileOperation(
    const std::string& operation, const std::string& path) const {
    
    // Check protected paths
    if (isPathProtected(path)) {
        return {false, "Path is protected: " + path};
    }
    
    // Check action level
    auto level = getActionLevel(operation);
    if (level == ActionLevel::Danger) {
        // Allow delete to recycle only
        std::string lower_op = operation;
        std::transform(lower_op.begin(), lower_op.end(), lower_op.begin(), ::tolower);
        if (lower_op.find("recycle") != std::string::npos || 
            lower_op.find("trash") != std::string::npos) {
            return {true, "Operation will use recycle bin (recoverable)"};
        }
        return {false, "Dangerous operation blocked: " + operation};
    }
    
    return {true, "Operation allowed"};
}

std::pair<bool, int> SafetyGuard::requiresConfirmation(const std::string& command) const {
    auto level = getActionLevel(command);
    
    switch (level) {
        case ActionLevel::Danger:
            return {true, 9};
        case ActionLevel::Caution:
            return {true, 5};
        case ActionLevel::Safe:
            return {false, 1};
    }
    
    return {false, 0};
}

void SafetyGuard::logAction(const std::string& action, const std::string& target,
                             const std::string& status) {
    auto now = std::chrono::system_clock::now();
    auto epoch = now.time_since_epoch();
    double timestamp = std::chrono::duration<double>(epoch).count();
    
    nlohmann::json entry = {
        {"action", action},
        {"target", target},
        {"status", status},
        {"timestamp", timestamp},
        {"level", getActionLevelString(action)}
    };
    
    action_log_.push_back(std::move(entry));
    
    // Keep log bounded
    if (action_log_.size() > 1000) {
        action_log_.erase(action_log_.begin(),
                          action_log_.begin() + static_cast<long>(action_log_.size() - 500));
    }
}

} // namespace vision
