/**
 * @file react_agent.cpp
 * @brief ReAct (Reasoning + Acting) agent loop
 */

#include "react_agent.h"
#include "llm_controller.h"
#include "action_executor.h"
#include "window_manager.h"
#include "vision_ai.h"
#include <sstream>
#include <thread>
#include <chrono>

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

using json = nlohmann::json;

namespace vision {

ReActAgent::ReActAgent(LLMController& llm, ActionExecutor& executor,
                       WindowManager& winmgr, VisionAI& app)
    : llm_(llm), executor_(executor), winmgr_(winmgr), app_(app) {}

std::pair<bool, std::string> ReActAgent::executeTask(const std::string& command) {
    LOG_INFO("ReAct agent starting task: {}", command);
    
    running_ = true;
    action_history_.clear();
    action_counts_.clear();
    
    std::string last_result;
    bool success = false;
    
    for (int step = 0; step < max_steps_ && running_.load(); step++) {
        LOG_INFO("ReAct step {}/{}", step + 1, max_steps_);
        
        // 1. Observe
        auto context = observe();
        context["step"] = step + 1;
        context["max_steps"] = max_steps_;
        
        // 2. Think
        auto plan = think(command, context);
        if (!plan) {
            // Try fallback
            plan = fallbackThink(command);
            if (!plan) {
                last_result = "Unable to determine next action";
                break;
            }
        }
        
        auto& action_json = *plan;
        std::string thought = action_json.value("thought", "");
        std::string action = action_json.value("action", "");
        auto params = action_json.value("params", json::object());
        
        LOG_INFO("Thought: {} | Action: {}", thought, action);
        
        // Check for task completion
        if (action == "task_complete" || action == "done" || action == "finish") {
            success = true;
            last_result = params.value("message",
                          thought.empty() ? "Task completed" : thought);
            break;
        }
        
        // Check for repeated actions
        std::string action_key = action + ":" + params.dump();
        if (isActionRepeated(action_key)) {
            LOG_WARN("Action repeated too many times: {}", action);
            last_result = "Action loop detected, stopping";
            break;
        }
        
        // 3. Act
        auto [act_success, act_result] = act(action_json);
        
        // Record in history
        json history_entry = {
            {"step", step + 1},
            {"thought", thought},
            {"action", action},
            {"params", params},
            {"result", act_result},
            {"success", act_success}
        };
        action_history_.push_back(history_entry);
        
        last_result = act_result;
        
        if (!act_success) {
            LOG_WARN("Action failed: {}", act_result);
            // Continue — the agent can self-correct
        }
        
        // Small delay between steps
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    
    running_ = false;
    
    if (last_result.empty()) last_result = success ? "Task completed" : "Task failed";
    LOG_INFO("ReAct agent finished: {} - {}", success ? "SUCCESS" : "FAILED", last_result);
    
    return {success, last_result};
}

void ReActAgent::stop() {
    running_ = false;
}

std::vector<json> ReActAgent::getActionHistory() const {
    return action_history_;
}

json ReActAgent::observe() {
    json ctx;
    
    // Current window info
    ctx["active_window"] = winmgr_.getActiveWindowTitle();
    
    // Application context
    ctx["context"] = app_.contextMgr().getFullContext();
    
    // Previous actions summary
    if (!action_history_.empty()) {
        ctx["previous_actions"] = json::array();
        // Only include last 3 actions to avoid context overflow
        int start = std::max(0, (int)action_history_.size() - 3);
        for (int i = start; i < (int)action_history_.size(); i++) {
            ctx["previous_actions"].push_back({
                {"action", action_history_[i]["action"]},
                {"result", action_history_[i]["result"]},
                {"success", action_history_[i]["success"]}
            });
        }
    }
    
    return ctx;
}

std::optional<json> ReActAgent::think(const std::string& cmd, const json& ctx) {
    return llm_.reactStep(cmd, ctx, action_history_);
}

std::pair<bool, std::string> ReActAgent::act(const json& plan) {
    std::string action = plan.value("action", "");
    auto params = plan.value("params", json::object());
    
    if (action.empty()) {
        return {false, "No action specified"};
    }
    
    return executor_.executeAction(action, params);
}

json ReActAgent::fallbackThink(const std::string& cmd) {
    // Simple keyword-based fallback when LLM fails
    std::string lower = cmd;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    
    if (lower.find("open") != std::string::npos) {
        // Extract app name
        auto pos = lower.find("open ");
        if (pos != std::string::npos) {
            std::string app = cmd.substr(pos + 5);
            return {{"thought", "Fallback: opening app"},
                    {"action", "open_app"},
                    {"params", {{"name", app}}}};
        }
    }
    
    if (lower.find("search") != std::string::npos || lower.find("google") != std::string::npos) {
        auto pos = lower.find("search ");
        if (pos == std::string::npos) pos = lower.find("google ");
        if (pos != std::string::npos) {
            std::string query = cmd.substr(pos + 7);
            return {{"thought", "Fallback: web search"},
                    {"action", "search_web"},
                    {"params", {{"query", query}}}};
        }
    }
    
    // Default: mark done with failure message
    return {{"thought", "Unable to determine action"},
            {"action", "task_complete"},
            {"params", {{"message", "I couldn't understand that command. Try rephrasing."}}}};
}

bool ReActAgent::isActionRepeated(const std::string& key) {
    action_counts_[key]++;
    return action_counts_[key] > 3;
}

} // namespace vision
