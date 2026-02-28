#pragma once
/**
 * @file context_manager.h
 * @brief Application context and command history tracking
 * 
 * Tracks the currently active application, browser state, file explorer state,
 * and maintains a rolling command history for context-aware command processing.
 */

#include <string>
#include <vector>
#include <mutex>
#include <nlohmann/json.hpp>

namespace vision {

class ContextManager {
public:
    ContextManager();

    // ── App context ──────────────────────────────────────────────
    struct AppContext {
        std::string name;
        std::string title;
        std::string exe_name;
        double timestamp = 0.0;
    };
    void updateActiveApp(const std::string& title, const std::string& exe);
    AppContext getActiveApp() const;

    // ── Browser state ────────────────────────────────────────────
    struct BrowserState {
        std::string url;
        std::string tab_title;
        bool is_active = false;
    };
    void updateBrowserState(const std::string& url, const std::string& tab_title);
    BrowserState getBrowserState() const;

    // ── File explorer state ──────────────────────────────────────
    struct FileExplorerState {
        std::string current_path;
        std::vector<std::string> visible_files;
        bool is_active = false;
    };
    void updateFileExplorerState(const std::string& current_path,
                                  const std::vector<std::string>& files);
    FileExplorerState getFileExplorerState() const;

    // ── Command history ──────────────────────────────────────────
    void recordCommand(const std::string& command, const std::string& result);
    std::vector<nlohmann::json> getRecentCommands(int count = 5) const;

    // ── Full context snapshot ────────────────────────────────────
    nlohmann::json getFullContext() const;

private:
    mutable std::mutex mutex_;
    AppContext active_app_;
    BrowserState browser_state_;
    FileExplorerState explorer_state_;
    std::vector<nlohmann::json> command_history_;
    int max_history_ = 50;

    std::vector<nlohmann::json> getRecentCommands_unlocked(int count) const;
};

} // namespace vision
