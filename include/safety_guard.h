#pragma once
/**
 * @file safety_guard.h
 * @brief Path protection and action safety validation
 * 
 * Prevents dangerous file operations on system-critical paths,
 * classifies action risk levels, and logs all actions for auditing.
 */

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace vision {

class SafetyGuard {
public:
    /// Action risk levels
    enum class ActionLevel { Safe, Caution, Danger };

    SafetyGuard();

    /// Check if a path is in the protected list
    bool isPathProtected(const std::string& path) const;

    /// Get the risk level of a command
    ActionLevel getActionLevel(const std::string& command) const;
    std::string getActionLevelString(const std::string& command) const;

    /// Validate a file operation. Returns {allowed, reason}.
    std::pair<bool, std::string> validateFileOperation(
        const std::string& operation, const std::string& path) const;

    /// Check if a command requires user confirmation. Returns {needs_confirm, risk_score 0-10}.
    std::pair<bool, int> requiresConfirmation(const std::string& command) const;

    /// Log an action for audit trail
    void logAction(const std::string& action, const std::string& target,
                   const std::string& status);

    /// Get action log
    const std::vector<nlohmann::json>& getActionLog() const { return action_log_; }

private:
    static const std::vector<std::string> PROTECTED_PATHS;
    static const std::vector<std::string> CAUTION_ACTIONS;
    static const std::vector<std::string> DANGER_ACTIONS;
    static const std::vector<std::string> PROTECTED_PROCESSES;

    std::vector<nlohmann::json> action_log_;

    bool matchesPattern(const std::string& path, const std::string& pattern) const;
    std::string normalizePath(const std::string& path) const;
};

} // namespace vision
