#pragma once
/**
 * @file safety_guard.h
 * @brief WHITELIST-based path protection and action safety validation
 * 
 * Uses a strict WHITELIST approach: the AI can only modify files inside
 * designated safe folders (AI_Workspace, Downloads, Desktop).
 * All operations outside the whitelist require explicit user confirmation.
 * System-critical paths are still hard-blocked as a secondary safety net.
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

    /// Check if a path is in the whitelist (safe to modify without confirmation)
    bool isPathWhitelisted(const std::string& path) const;

    /// Check if a path is in the hard-blocked system protected list
    bool isPathProtected(const std::string& path) const;

    /// Get the risk level of a command
    ActionLevel getActionLevel(const std::string& command) const;
    std::string getActionLevelString(const std::string& command) const;

    /// Validate a file operation using WHITELIST logic.
    /// Returns {allowed, reason}. If not allowed, reason explains why
    /// and indicates whether user confirmation would override.
    std::pair<bool, std::string> validateFileOperation(
        const std::string& operation, const std::string& path) const;

    /// Check if a command requires user confirmation.
    /// Returns {needs_confirm, risk_score 0-10}.
    std::pair<bool, int> requiresConfirmation(const std::string& command) const;

    /// Log an action for audit trail
    void logAction(const std::string& action, const std::string& target,
                   const std::string& status);

    /// Get action log
    const std::vector<nlohmann::json>& getActionLog() const { return action_log_; }

    /// Get the whitelist paths (for UI display)
    const std::vector<std::string>& getWhitelistedPaths() const { return WHITELISTED_PATHS; }

private:
    static const std::vector<std::string> PROTECTED_PATHS;
    static const std::vector<std::string> CAUTION_ACTIONS;
    static const std::vector<std::string> DANGER_ACTIONS;
    static const std::vector<std::string> PROTECTED_PROCESSES;
    std::vector<std::string> WHITELISTED_PATHS;

    std::vector<nlohmann::json> action_log_;

    bool matchesPattern(const std::string& path, const std::string& pattern) const;
    std::string normalizePath(const std::string& path) const;

    static std::vector<std::string> buildProtectedPaths();
    std::vector<std::string> buildWhitelistedPaths() const;
};

} // namespace vision
