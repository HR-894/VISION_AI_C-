#pragma once
/**
 * @file react_agent.h
 * @brief ReAct (Reasoning + Acting) agent loop
 * 
 * Implements Observe → Think → Act cycle with self-correction,
 * action deduplication, and fallback reasoning.
 */

#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <atomic>
#include <algorithm>
#include <nlohmann/json.hpp>

namespace vision {

// Forward declarations
class LLMController;
class ActionExecutor;
class WindowManager;
class VisionAI;

class ReActAgent {
public:
    ReActAgent(LLMController& llm, ActionExecutor& executor,
               WindowManager& winmgr, VisionAI& app);

    /// Main entry: execute user command via ReAct loop
    /// Returns {success, summary_message}
    std::pair<bool, std::string> executeTask(const std::string& command);

    /// Stop the currently running task
    void stop();

    /// Get action history for the current/last task
    std::vector<nlohmann::json> getActionHistory() const;

    /// Set max iterations
    void setMaxSteps(int steps) { max_steps_ = steps; }

private:
    LLMController& llm_;
    ActionExecutor& executor_;
    WindowManager& winmgr_;
    VisionAI& app_;

    std::vector<nlohmann::json> action_history_;
    std::unordered_map<std::string, int> action_counts_;
    int max_steps_ = 10;
    std::atomic<bool> running_{false};

    // ── Core loop steps ──────────────────────────────────────────
    nlohmann::json observe();
    std::optional<nlohmann::json> think(const std::string& cmd,
                                         const nlohmann::json& ctx);
    std::pair<bool, std::string> act(const nlohmann::json& plan);

    // ── Fallback ─────────────────────────────────────────────────
    nlohmann::json fallbackThink(const std::string& cmd);

    // ── Helpers ──────────────────────────────────────────────────
    bool isActionRepeated(const std::string& key);
};

} // namespace vision
