/**
 * @file command_router.cpp
 * @brief Intelligent command dispatcher — routes to template matcher, fast handler, or ReAct agent
 */

#include "command_router.h"
#include "smart_template_matcher.h"
#include "fast_complex_handler.h"
#include <nlohmann/json.hpp>

#ifdef VISION_HAS_LLM
#include "react_agent.h"
#endif

#include <algorithm>
#include <regex>

#ifdef VISION_HAS_SPDLOG
#include <spdlog/spdlog.h>
#define LOG_INFO(...) spdlog::info(__VA_ARGS__)
#else
#define LOG_INFO(...) (void)0
#endif

using json = nlohmann::json;

namespace vision {

CommandRouter::CommandRouter(SmartTemplateMatcher& matcher,
                              FastComplexHandler& fast_handler)
    : matcher_(matcher)
    , fast_handler_(fast_handler) {}

#ifdef VISION_HAS_LLM
void CommandRouter::setReActAgent(ReActAgent* agent) {
    react_agent_ = agent;
}
#endif

CommandRouter::Complexity CommandRouter::classifyComplexity(const std::string& command) {
    std::string lower = command;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    
    // 1. Simple template match
    if (matcher_.canHandle(lower)) {
        return Complexity::Simple;
    }
    
    // 2. Fast complex patterns
    if (fast_handler_.canHandle(lower)) {
        return Complexity::FastComplex;
    }
    
    // 3. Multi-step indicators → needs agent
    static const std::vector<std::string> complex_indicators = {
        "find and", "look for", "if", "unless", "check whether",
        "figure out", "help me", "how to", "explain",
        "compare", "analyze", "summarize", "what should",
    };
    
    for (const auto& indicator : complex_indicators) {
        if (lower.find(indicator) != std::string::npos) {
            return Complexity::AgentRequired;
        }
    }
    
    // 4. Very long commands likely need agent
    int word_count = 0;
    bool in_word = false;
    for (char c : lower) {
        if (c == ' ') { in_word = false; }
        else if (!in_word) { in_word = true; word_count++; }
    }
    
    if (word_count > 8) {
        return Complexity::AgentRequired;
    }
    
    // Default: try template first, fall back to agent
    return Complexity::Simple;
}

std::pair<bool, std::string> CommandRouter::route(const std::string& command) {
    auto complexity = classifyComplexity(command);
    
    LOG_INFO("Command complexity: {} -> {}",
             command, complexityToString(complexity));
    
    switch (complexity) {
        case Complexity::Simple: {
            // Try template matcher
            auto result = matcher_.match(command);
            if (result) {
                // Return the match result for caller to execute
                json match_data;
                match_data["type"] = "template";
                match_data["template"] = result->template_name;
                match_data["variables"] = result->variables;
                return std::make_pair(true, match_data.dump());
            }
            // Fall through to fast complex
            [[fallthrough]];
        }
        
        case Complexity::FastComplex: {
            auto result = fast_handler_.tryHandle(command);
            if (result) return *result;
            // Fall through to agent
            [[fallthrough]];
        }
        
        case Complexity::AgentRequired: {
#ifdef VISION_HAS_LLM
            if (react_agent_) {
                return react_agent_->executeTask(command);
            }
#endif
            return {false, "Command too complex and no AI backend available. "
                           "Try a simpler command or enable LLM support."};
        }
    }
    
    return {false, "Unable to process command"};
}

std::string CommandRouter::complexityToString(Complexity c) {
    switch (c) {
        case Complexity::Simple: return "SIMPLE";
        case Complexity::FastComplex: return "FAST_COMPLEX";
        case Complexity::AgentRequired: return "AGENT_REQUIRED";
    }
    return "UNKNOWN";
}

} // namespace vision
