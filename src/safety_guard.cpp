/**
 * @file safety_guard.cpp
 * @brief WHITELIST-based path protection and action safety validation
 *
 * Logic:
 *   1. System paths (Windows, Program Files, etc.) → HARD BLOCK (never allowed)
 *   2. Whitelisted paths (AI_Workspace, Downloads, Desktop) → ALLOWED
 *   3. Everything else → DENIED with "user confirmation required" message
 */

#include "safety_guard.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <regex>
#include <windows.h>
#include <shlobj.h>

namespace fs = std::filesystem;

namespace vision {

// ═══════════════════ Protected System Paths (Hard Block) ═══════════════════

std::vector<std::string> SafetyGuard::buildProtectedPaths() {
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
        drive + "System Volume Information",
        drive + "EFI",
    };
}

const std::vector<std::string> SafetyGuard::PROTECTED_PATHS = buildProtectedPaths();

// ═══════════════════ Whitelisted Safe Paths ═══════════════════

std::vector<std::string> SafetyGuard::buildWhitelistedPaths() const {
    std::vector<std::string> paths;
    char user_profile[MAX_PATH];

    // AI_Workspace — primary sandbox
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PROFILE, nullptr, 0, user_profile))) {
        paths.push_back(std::string(user_profile) + "\\AI_Workspace");
    }

    // Downloads
    char downloads[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PROFILE, nullptr, 0, downloads))) {
        paths.push_back(std::string(downloads) + "\\Downloads");
    }

    // Desktop
    char desktop[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desktop))) {
        paths.push_back(std::string(desktop));
    }

    // Temp (for scratch files)
    char temp[MAX_PATH];
    if (GetTempPathA(MAX_PATH, temp)) {
        paths.push_back(std::string(temp));
    }

    return paths;
}

// ═══════════════════ Action Classification ═══════════════════

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

// ═══════════════════ Constructor ═══════════════════

SafetyGuard::SafetyGuard() {
    WHITELISTED_PATHS = buildWhitelistedPaths();
}

// ═══════════════════ Path Utilities ═══════════════════

std::string SafetyGuard::normalizePath(const std::string& path) const {
    try {
        fs::path p(path);
        if (p.is_relative()) {
            p = fs::absolute(p);
        }
        std::string normalized = p.lexically_normal().string();
        // Uppercase drive letter
        if (normalized.size() >= 2 && normalized[1] == ':') {
            normalized[0] = static_cast<char>(std::toupper(normalized[0]));
        }
        // Remove trailing backslash (unless root)
        while (normalized.size() > 3 && normalized.back() == '\\') {
            normalized.pop_back();
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

// ═══════════════════ Core Whitelist/Protection Checks ═══════════════════

bool SafetyGuard::isPathProtected(const std::string& path) const {
    for (const auto& protected_path : PROTECTED_PATHS) {
        if (matchesPattern(path, protected_path)) {
            return true;
        }
    }
    return false;
}

bool SafetyGuard::isPathWhitelisted(const std::string& path) const {
    for (const auto& safe_path : WHITELISTED_PATHS) {
        if (matchesPattern(path, safe_path)) {
            return true;
        }
    }
    return false;
}

// ═══════════════════ Action Level Classification ═══════════════════

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

// ═══════════════════ File Operation Validation (WHITELIST-FIRST) ═══════════════════

std::pair<bool, std::string> SafetyGuard::validateFileOperation(
    const std::string& operation, const std::string& path) const {
    
    // STEP 1: Hard-block system-critical paths (always denied, no override)
    if (isPathProtected(path)) {
        return {false, "🛑 BLOCKED: System-critical path is protected: " + path};
    }
    
    // STEP 2: Check whitelist — if inside safe zone, apply action-level rules
    if (isPathWhitelisted(path)) {
        auto level = getActionLevel(operation);
        if (level == ActionLevel::Danger) {
            // Even in the whitelist, destructive ops need extra care
            std::string lower_op = operation;
            std::transform(lower_op.begin(), lower_op.end(), lower_op.begin(), ::tolower);
            if (lower_op.find("recycle") != std::string::npos ||
                lower_op.find("trash") != std::string::npos) {
                return {true, "✅ Operation will use recycle bin (recoverable)"};
            }
            return {true, "⚠️ Dangerous operation allowed inside AI workspace: " + operation};
        }
        return {true, "✅ Operation allowed (path is in AI workspace)"};
    }
    
    // STEP 3: Path is outside whitelist — DENY and require user confirmation
    return {false, "⛔ Path outside AI workspace. User confirmation required for: " + path +
                   "\nAllowed folders: AI_Workspace, Downloads, Desktop, Temp"};
}

// ═══════════════════ Confirmation Check ═══════════════════

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

// ═══════════════════ Audit Logging ═══════════════════

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
