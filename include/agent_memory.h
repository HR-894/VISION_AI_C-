#pragma once
/**
 * @file agent_memory.h
 * @brief Persistent JSON memory for the agent
 * 
 * Records task history (with optional vector embeddings), learns app aliases,
 * tracks file locations, stores corrections, and provides cosine-similarity
 * based task lookup for vector memory. Auto-saves to disk on destruction.
 */

#include <string>
#include <vector>
#include <mutex>
#include <filesystem>
#include <chrono>
#include <functional>
#include <nlohmann/json.hpp>

namespace vision {

// Forward declaration — AgentMemory optionally uses LLMController for embeddings
class LLMController;

class AgentMemory {
public:
    explicit AgentMemory(const std::string& memory_path = "");
    ~AgentMemory();  // auto-saves

    // Non-copyable
    AgentMemory(const AgentMemory&) = delete;
    AgentMemory& operator=(const AgentMemory&) = delete;

    /// Set LLM controller reference for embedding generation
    void setLLM(LLMController* llm) { llm_ = llm; }

    /// Record a completed task with command, result, success flag, and action list
    /// Optionally includes vector embedding for similarity search
    void recordTask(const std::string& command, const std::string& result,
                    bool success, const std::vector<std::string>& actions,
                    const std::vector<float>& embedding = {});

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

    /// Find similar past tasks using cosine similarity on embeddings
    /// Falls back to word-overlap if no embeddings available
    std::vector<nlohmann::json> findSimilarTasks(const std::string& command,
                                                  int max_results = 3,
                                                  const std::vector<float>& query_embedding = {}) const;

    /// Get memory statistics 
    nlohmann::json getStats() const;

private:
    nlohmann::json memory_;
    std::string file_path_;
    mutable std::mutex mutex_;
    bool dirty_ = false;
    std::chrono::steady_clock::time_point last_save_{};
    LLMController* llm_ = nullptr;  // Optional: for embedding generation

    void load();
    void save();
    void autoSave();
};

} // namespace vision
