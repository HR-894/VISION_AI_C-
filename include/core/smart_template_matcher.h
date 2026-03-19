#pragma once
/**
 * @file smart_template_matcher.h
 * @brief Regex-based command pattern matching with variable extraction
 * 
 * Handles ~90% of common commands instantly without LLM.
 * Supports variable extraction, chained commands, and context-aware routing.
 */

#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <regex>

namespace vision {

/// Command category for organization
enum class CommandCategory {
    App, Window, System, File, Web
};

/// Result of a template match
struct MatchResult {
    std::string template_name;
    std::unordered_map<std::string, std::string> variables;
    int confidence = 0;
    CommandCategory category = CommandCategory::System;
};

/// A command template with regex pattern and variable names
struct CommandTemplate {
    std::string name;
    std::regex pattern;
    std::vector<std::string> variables;
    CommandCategory category = CommandCategory::System;
    int priority = 0;
};

class SmartTemplateMatcher {
public:
    SmartTemplateMatcher();

    /// Try to match a command. Returns MatchResult or nullopt.
    std::optional<MatchResult> match(const std::string& command);

    /// Match chained commands (split on "and", "then", etc.)
    std::vector<MatchResult> matchChained(const std::string& text);

    /// Check if a command can be handled by templates
    bool canHandle(const std::string& text);

    /// Get the number of registered templates
    size_t getTemplateCount() const { return templates_.size(); }

private:
    std::vector<CommandTemplate> templates_;

    void initTemplates();
    void addTemplate(const std::string& name,
                     const std::string& pattern,
                     const std::vector<std::string>& vars,
                     CommandCategory category,
                     int priority);
};

} // namespace vision
