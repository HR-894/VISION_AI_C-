#pragma once
/**
 * @file fast_complex_handler.h
 * @brief Pattern-based multi-step command handler (no LLM needed)
 * 
 * Handles complex multi-step commands that can be resolved purely
 * by pattern matching, such as "open X and search Y", "list downloads
 * in notepad", or "organize downloads".
 */

#include <string>
#include <vector>
#include <optional>
#include <utility>
#include <functional>
#include <regex>

namespace vision {

class VisionAI; // Forward declaration

class FastComplexHandler {
public:
    explicit FastComplexHandler(VisionAI& app);

    /// Try to handle a complex command. Returns {success, result} or nullopt.
    std::optional<std::pair<bool, std::string>> tryHandle(const std::string& command);

    /// Check if a command matches any fast complex pattern
    bool canHandle(const std::string& command) const;

private:
    VisionAI& app_;

    // Pattern registry
    struct Pattern {
        std::string name;
        std::regex regex;
        std::function<std::pair<bool, std::string>(const std::smatch&)> handler;
    };
    std::vector<Pattern> patterns_;

    void initPatterns();
    void addPattern(const std::string& name,
                    const std::string& regex_pattern,
                    std::function<std::pair<bool, std::string>(const std::smatch&)> handler);
};

} // namespace vision
