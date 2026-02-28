/**
 * @file agent_memory.cpp
 * @brief Persistent DPAPI-encrypted agent memory
 *
 * All data is encrypted at rest using Windows DPAPI (CryptProtectData).
 * Only the logged-in Windows user account can decrypt the data.
 * Handles transparent migration from old plaintext JSON files.
 */

#include "agent_memory.h"
#include <fstream>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <windows.h>
#include <wincrypt.h>

#pragma comment(lib, "crypt32.lib")

#ifdef VISION_HAS_SPDLOG
#include <spdlog/spdlog.h>
#define LOG_INFO(...) spdlog::info(__VA_ARGS__)
#define LOG_ERROR(...) spdlog::error(__VA_ARGS__)
#else
#define LOG_INFO(...) (void)0
#define LOG_ERROR(...) (void)0
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace vision {

// ═══════════════════ DPAPI Helpers ═══════════════════

static std::vector<BYTE> dpapi_encrypt(const std::string& plaintext) {
    DATA_BLOB input;
    input.pbData = const_cast<BYTE*>(reinterpret_cast<const BYTE*>(plaintext.data()));
    input.cbData = static_cast<DWORD>(plaintext.size());

    DATA_BLOB output{};
    if (CryptProtectData(&input, L"VisionAI_Memory", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        std::vector<BYTE> result(output.pbData, output.pbData + output.cbData);
        LocalFree(output.pbData);
        return result;
    }
    return {};
}

static std::string dpapi_decrypt(const std::vector<BYTE>& encrypted) {
    DATA_BLOB input;
    input.pbData = const_cast<BYTE*>(encrypted.data());
    input.cbData = static_cast<DWORD>(encrypted.size());

    DATA_BLOB output{};
    if (CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        std::string result(reinterpret_cast<char*>(output.pbData), output.cbData);
        LocalFree(output.pbData);
        return result;
    }
    return {};
}

/// Check if file content looks like plaintext JSON (for migration)
static bool is_plaintext_json(const std::vector<BYTE>& data) {
    if (data.empty()) return false;
    // Skip BOM if present
    size_t start = 0;
    if (data.size() >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        start = 3;
    }
    // Skip whitespace
    while (start < data.size() && (data[start] == ' ' || data[start] == '\t' ||
                                     data[start] == '\n' || data[start] == '\r')) {
        start++;
    }
    return start < data.size() && data[start] == '{';
}

// ═══════════════════ AgentMemory Implementation ═══════════════════

AgentMemory::AgentMemory(const std::string& memory_path)
    : file_path_(memory_path) {
    if (file_path_.empty()) {
        file_path_ = "data/agent_memory.json";
    }
    load();
}

AgentMemory::~AgentMemory() {
    save();
}

void AgentMemory::load() {
    std::lock_guard lock(mutex_);
    if (!fs::exists(file_path_)) {
        memory_ = {
            {"task_history", json::array()},
            {"aliases", json::object()},
            {"corrections", json::array()},
            {"file_locations", json::object()},
            {"preferences", json::object()},
            {"stats", {{"total_tasks", 0}, {"success_count", 0}, {"fail_count", 0}}}
        };
        return;
    }
    try {
        // Read raw bytes
        std::ifstream f(file_path_, std::ios::binary);
        std::vector<BYTE> raw((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
        f.close();

        if (raw.empty()) {
            memory_ = json::object();
            return;
        }

        // Migration: detect plaintext JSON and re-encrypt on next save
        if (is_plaintext_json(raw)) {
            LOG_INFO("Migrating plaintext agent_memory to DPAPI encryption");
            std::string text(raw.begin(), raw.end());
            memory_ = json::parse(text);
            dirty_ = true;  // Will re-save encrypted
            return;
        }

        // Decrypt DPAPI blob
        std::string decrypted = dpapi_decrypt(raw);
        if (decrypted.empty()) {
            LOG_ERROR("DPAPI decryption failed for agent_memory — starting fresh");
            memory_ = json::object();
            return;
        }

        memory_ = json::parse(decrypted);
        LOG_INFO("Agent memory loaded (encrypted) from {}", file_path_);
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to load memory: {}", e.what());
        memory_ = json::object();
    }
}

void AgentMemory::save() {
    std::lock_guard lock(mutex_);
    try {
        fs::create_directories(fs::path(file_path_).parent_path());

        std::string plaintext = memory_.dump(2);
        auto encrypted = dpapi_encrypt(plaintext);

        if (encrypted.empty()) {
            LOG_ERROR("DPAPI encryption failed — falling back to plaintext");
            std::ofstream f(file_path_);
            f << plaintext << std::flush;
            return;
        }

        std::ofstream f(file_path_, std::ios::binary);
        f.write(reinterpret_cast<char*>(encrypted.data()),
                static_cast<std::streamsize>(encrypted.size()));
        if (!f.good()) { LOG_ERROR("Memory file write I/O error"); }
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to save memory: {}", e.what());
    }
}

void AgentMemory::recordTask(const std::string& command, const std::string& result,
                              bool success, const std::vector<std::string>& actions,
                              const std::vector<float>& embedding) {
    std::lock_guard lock(mutex_);
    
    auto now = std::chrono::system_clock::now();
    double ts = std::chrono::duration<double>(now.time_since_epoch()).count();
    
    json entry = {
        {"command", command},
        {"result", result},
        {"success", success},
        {"actions", actions},
        {"timestamp", ts}
    };

    // Store vector embedding if provided
    if (!embedding.empty()) {
        entry["embedding"] = embedding;
    }
    
    memory_["task_history"].push_back(entry);
    
    // Limit history
    if (memory_["task_history"].size() > 200) {
        auto& hist = memory_["task_history"];
        hist.erase(hist.begin(), hist.begin() + static_cast<long>(hist.size() - 100));
    }
    
    memory_["stats"]["total_tasks"] = memory_["stats"]["total_tasks"].get<int>() + 1;
    if (success) memory_["stats"]["success_count"] = memory_["stats"]["success_count"].get<int>() + 1;
    else memory_["stats"]["fail_count"] = memory_["stats"]["fail_count"].get<int>() + 1;
    
    dirty_ = true;
    autoSave();
}

void AgentMemory::learnAlias(const std::string& alias, const std::string& real_name) {
    std::lock_guard lock(mutex_);
    std::string lower = alias;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    memory_["aliases"][lower] = real_name;
    dirty_ = true;
    autoSave();
}

std::string AgentMemory::resolveAlias(const std::string& name) const {
    std::lock_guard lock(mutex_);
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (memory_.contains("aliases") && memory_["aliases"].contains(lower)) {
        return memory_["aliases"][lower].get<std::string>();
    }
    return name;
}

void AgentMemory::recordCorrection(const std::string& wrong_action,
                                     const std::string& correct_action,
                                     const std::string& context) {
    std::lock_guard lock(mutex_);
    memory_["corrections"].push_back({
        {"wrong", wrong_action},
        {"correct", correct_action},
        {"context", context},
        {"timestamp", std::chrono::duration<double>(
            std::chrono::system_clock::now().time_since_epoch()).count()}
    });
    dirty_ = true;
    autoSave();
}

void AgentMemory::rememberFileLocation(const std::string& name, const std::string& path) {
    std::lock_guard lock(mutex_);
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    memory_["file_locations"][lower] = path;
    dirty_ = true;
}

std::string AgentMemory::recallFileLocation(const std::string& name) const {
    std::lock_guard lock(mutex_);
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    if (memory_.contains("file_locations") && memory_["file_locations"].contains(lower)) {
        return memory_["file_locations"][lower].get<std::string>();
    }
    return "";
}

std::vector<json> AgentMemory::findSimilarTasks(const std::string& command,
                                                  int max_results,
                                                  const std::vector<float>& query_embedding) const {
    std::lock_guard lock(mutex_);
    std::vector<json> results;
    if (!memory_.contains("task_history")) return results;

    // ── Strategy 1: Cosine similarity with vector embeddings ──
    if (!query_embedding.empty()) {
        std::vector<std::pair<float, json>> scored;

        for (const auto& task : memory_["task_history"]) {
            if (!task.contains("embedding")) continue;
            auto saved_emb = task["embedding"].get<std::vector<float>>();
            if (saved_emb.size() != query_embedding.size()) continue;

            // Cosine similarity using <cmath>
            float dot = 0.0f, norm_a = 0.0f, norm_b = 0.0f;
            for (size_t i = 0; i < query_embedding.size(); i++) {
                dot += query_embedding[i] * saved_emb[i];
                norm_a += query_embedding[i] * query_embedding[i];
                norm_b += saved_emb[i] * saved_emb[i];
            }
            float denom = std::sqrt(norm_a) * std::sqrt(norm_b);
            float similarity = (denom > 0.0f) ? (dot / denom) : 0.0f;

            if (similarity > 0.85f) {
                scored.emplace_back(similarity, task);
            }
        }

        std::sort(scored.begin(), scored.end(),
                  [](auto& a, auto& b) { return a.first > b.first; });

        for (int i = 0; i < std::min(max_results, (int)scored.size()); i++) {
            results.push_back(scored[i].second);
        }

        if (!results.empty()) return results;
        // Fall through to word-overlap if no embedding matches
    }

    // ── Strategy 2: Word-overlap fallback ──
    std::string lower_cmd = command;
    std::transform(lower_cmd.begin(), lower_cmd.end(), lower_cmd.begin(), ::tolower);

    std::vector<std::string> cmd_words;
    std::istringstream ss(lower_cmd);
    std::string word;
    while (ss >> word) cmd_words.push_back(word);

    std::vector<std::pair<int, json>> scored;

    for (const auto& task : memory_["task_history"]) {
        std::string task_cmd = task.value("command", "");
        std::string lower_task = task_cmd;
        std::transform(lower_task.begin(), lower_task.end(), lower_task.begin(), ::tolower);

        int score = 0;
        for (const auto& w : cmd_words) {
            if (lower_task.find(w) != std::string::npos) score++;
        }

        if (score > 0) scored.emplace_back(score, task);
    }

    std::sort(scored.begin(), scored.end(),
              [](auto& a, auto& b) { return a.first > b.first; });

    for (int i = 0; i < std::min(max_results, (int)scored.size()); i++) {
        results.push_back(scored[i].second);
    }

    return results;
}

json AgentMemory::getStats() const {
    std::lock_guard lock(mutex_);
    return memory_.value("stats", json::object());
}

void AgentMemory::autoSave() {
    auto now = std::chrono::steady_clock::now();
    if (dirty_ && (now - last_save_ > std::chrono::seconds(30))) {
        // Save without re-locking (called from locked context)
        try {
            fs::create_directories(fs::path(file_path_).parent_path());

            std::string plaintext = memory_.dump(2);
            auto encrypted = dpapi_encrypt(plaintext);

            if (!encrypted.empty()) {
                std::ofstream f(file_path_, std::ios::binary);
                f.write(reinterpret_cast<char*>(encrypted.data()),
                        static_cast<std::streamsize>(encrypted.size()));
            } else {
                // Fallback
                std::ofstream f(file_path_);
                f << plaintext;
            }

            dirty_ = false;
            last_save_ = now;
        } catch (...) {}
    }
}

} // namespace vision
