#pragma once
/**
 * @file command_router.h
 * @brief Intelligent command routing based on complexity classification
 * 
 * Routes commands to the appropriate handler:
 * - Simple → SmartTemplateMatcher (instant)
 * - FastComplex → FastComplexHandler (no LLM)
 * - AgentRequired → ReActAgent (full AI)
 */

#include <string>
#include <utility>

namespace vision {

// Forward declarations
class SmartTemplateMatcher;
class FastComplexHandler;
#ifdef VISION_HAS_LLM
class ReActAgent;
#endif

class CommandRouter {
public:
    CommandRouter(SmartTemplateMatcher& matcher, FastComplexHandler& fast_handler);

    /// Main dispatch: classify and route command to appropriate handler
    std::pair<bool, std::string> route(const std::string& command);

#ifdef VISION_HAS_LLM
    void setReActAgent(ReActAgent* agent);
#endif

    /// Complexity levels
    enum class Complexity { Simple, FastComplex, AgentRequired };

    /// Classify command complexity
    Complexity classifyComplexity(const std::string& command);

private:
    SmartTemplateMatcher& matcher_;
    FastComplexHandler& fast_handler_;
#ifdef VISION_HAS_LLM
    ReActAgent* react_agent_ = nullptr;
#endif

    std::string complexityToString(Complexity c);
};

} // namespace vision
