#pragma once
/**
 * @file agent_memory.h
 * @brief Persistent JSON memory for the agent
 * 
 * Records task history, learns app aliases, tracks file locations,
 * stores corrections, and provides similarity-based task lookup.
 * Auto-saves to disk on destruction.
 */

#include <string>
#include <vector>
#include <mutex>
#include <filesystem>
#include <chrono>
#include <nlohmann/json.hpp>

namespace vision {

class AgentMemory {
public:
    explicit AgentMemory(const std::string& memory_path = "");
    ~AgentMemory();  // auto-saves

    // Non-copyable
    AgentMemory(const AgentMemory&) = delete;
    AgentMemory& operator=(const AgentMemory&) = delete;

    /// Record a completed task with command, result, success flag, and action list
    void recordTask(const std::string& command, const std::string& result,
                    bool success, const std::vector<std::string>& actions);

    /// Learn an app alias (user_name → actual_name)
    void learnAlias(const std::string& alias, const std::string& real_name);

    /// Resolve an alias to actual app name (returns original if not found)
    std::string resolveAlias(const std::string& name) const;

    /// Record a correction (failed → correct + context)
    void recordCorrection(const std::string& wrong_action,
                          const std::string& correct_action,
                          const std::string& context = "");

    /// Remember a file location for a given type/name
    void rememberFileLocation(const std::string& name, const std::string& path);

    /// Recall where files of a type are usually found
    std::string recallFileLocation(const std::string& name) const;

    /// Find similar past tasks (word-overlap scoring)
    std::vector<nlohmann::json> findSimilarTasks(const std::string& command,
                                                  int max_results = 3) const;

    /// Get memory statistics 
    nlohmann::json getStats() const;

private:
    nlohmann::json memory_;
    std::string file_path_;
    mutable std::mutex mutex_;
    bool dirty_ = false;
    std::chrono::steady_clock::time_point last_save_{};

    void load();
    void save();
    void autoSave();
};

} // namespace vision
