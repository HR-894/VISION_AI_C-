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
    condensed_memory_.clear();  // PRD Fix 5: reset memory
    
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
        
        // Check for task completion OR chat reply (1-step fast-exit)
        if (action == "task_complete" || action == "done" || action == "finish" ||
            action == "chat") {
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
        
        // PRD Fix 5: Summarize old steps instead of hard-deleting
        if (action_history_.size() > 5) {
            summarizeHistory();
        }
        
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
    // Smart dual-mode prompt: AI Agent + Chat (inspired by HR-AI-MIND)
    // The AI decides whether to execute an OS action or reply naturally.
    std::string smart_prompt =
        "You are VISION AI, a smart and friendly Windows desktop assistant. "
        "You understand English, Hindi, and Hinglish — always match the user's language. "
        "You have TWO modes:\n"
        "MODE 1 — OS ACTION: For tasks like opening apps, searching, typing, taking screenshots, etc.\n"
        "  Output: {\"action\": \"<action_name>\", \"params\": {<relevant_params>}}\n"
        "MODE 2 — CHAT: For greetings, questions, conversations, explanations, or when you cannot perform a task.\n"
        "  Output: {\"action\": \"chat\", \"params\": {\"message\": \"your natural reply\"}}\n\n"
        "Available OS actions: open_app, open_url, search_web, type_text, press_key, "
        "click_element, scroll, set_volume, set_brightness, minimize, maximize, "
        "close_window, focus_window, screenshot, list_files, move_file, copy_file, "
        "delete_file, clipboard_get, clipboard_set, get_ui_tree, task_complete, "
        "run_powershell.\n\n"
        "CRITICAL FOR COMPLEX WORKFLOWS:\n"
        "- If asked to do complex data processing (like 'search top 20 billionaires and write to excel' or 'organize downloads'), "
        "DO NOT use simple macros. Instead, use 'run_powershell' to write a script that does the entire job seamlessly.\n"
        "  Example: {\"action\": \"run_powershell\", \"params\": {\"script\": \"$data = Invoke-RestMethod ...; $data | Export-Csv output.csv\"}}\n"
        "  PowerShell has full access to the web (Invoke-RestMethod) and files.\n\n"
        "RULES:\n"
        "1. ALWAYS output ONLY valid JSON — no extra text before or after.\n"
        "2. If the user is chatting (hi, hello, how are you, what is X, etc.) use MODE 2.\n"
        "3. If the user wants an OS action, use MODE 1.\n"
        "4. Be smart, helpful, and conversational like a real assistant.\n\n";

    // PRD Fix 5: Inject condensed memory from earlier steps
    if (!condensed_memory_.empty()) {
        smart_prompt += "[MEMORY STATE — summary of earlier steps]\n" +
                        condensed_memory_ + "\n\n";
    }

    smart_prompt += "[USER] " + cmd;

    return llm_.reactStep(smart_prompt, ctx, action_history_);
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
        // FIX L4: Use keyword length instead of hardcoded 7
        std::string keyword;
        auto pos = lower.find("search ");
        if (pos != std::string::npos) { keyword = "search "; }
        else {
            pos = lower.find("google ");
            if (pos != std::string::npos) { keyword = "google "; }
        }
        if (pos != std::string::npos) {
            std::string query = cmd.substr(pos + keyword.size());
            return {{"thought", "Fallback: web search"},
                    {"action", "search_web"},
                    {"params", {{"query", query}}}};
        }
    }
    
    // Default: route to chat instead of error — let the AI respond naturally
    return {{"thought", "No template match — treating as conversation"},
            {"action", "chat"},
            {"params", {{"message", "I'm not sure how to do that as a system action, but I'll try to help! Could you tell me more about what you need?"}}}};
}

bool ReActAgent::isActionRepeated(const std::string& key) {
    action_counts_[key]++;
    return action_counts_[key] > 3;
}

void ReActAgent::summarizeHistory() {
    // PRD Fix 5: Compress oldest 3 entries into a condensed paragraph
    std::string summary_prompt =
        "Summarize these agent actions into ONE short paragraph (max 100 words).\n"
        "Focus on: what was the goal, what actions were taken, what results occurred.\n\n";
    
    int to_summarize = std::min(3, (int)action_history_.size());
    for (int i = 0; i < to_summarize; i++) {
        summary_prompt += "Step " + std::to_string(i + 1) + ": ";
        summary_prompt += "Action=" + action_history_[0].value("action", "?");
        summary_prompt += ", Result=" + action_history_[0].value("result", "?");
        summary_prompt += "\n";
        action_history_.erase(action_history_.begin());
    }
    
    // Use LLM to generate summary (lightweight single-shot call)
    auto result = llm_.generate(summary_prompt, {});
    if (!result.empty()) {
        if (!condensed_memory_.empty()) {
            condensed_memory_ += " | " + result;
        } else {
            condensed_memory_ = result;
        }
        // Keep condensed memory from growing unbounded
        if (condensed_memory_.size() > 500) {
            condensed_memory_ = condensed_memory_.substr(condensed_memory_.size() - 500);
        }
    }
    LOG_INFO("ReAct history summarized: {} steps compressed", to_summarize);
}

} // namespace vision
