/**
 * @file fast_complex_handler.cpp
 * @brief Pattern-based multi-step command handler (no LLM required)
 */

#include "fast_complex_handler.h"
#include "vision_ai.h"
#include <algorithm>
#include <regex>
#include <sstream>
#include <thread>
#include <shlobj.h>

#ifdef VISION_HAS_SPDLOG
#include <spdlog/spdlog.h>
#define LOG_INFO(...) spdlog::info(__VA_ARGS__)
#else
#define LOG_INFO(...) (void)0
#endif

using json = nlohmann::json;

namespace vision {

FastComplexHandler::FastComplexHandler(VisionAI& app) : app_(app) {
    initPatterns();
}

void FastComplexHandler::initPatterns() {
    // "open X and search/type Y"
    addPattern("open_and_search",
        R"((?:open|launch)\s+(.+?)\s+and\s+(?:search|look\s+up|find)\s+(.+?)$)",
        [this](const std::smatch& m) -> std::pair<bool, std::string> {
            std::string app = m[1].str();
            std::string query = m[2].str();
            
            auto result = app_.instantExecute("open " + app);
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            
            // If it's a browser, type in address bar
            if (app_.sysCmds().isBrowser(app)) {
                app_.windowMgr().waitAndFocus(app, 3.0f);
                app_.windowMgr().pressKey("ctrl+l");
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                app_.windowMgr().typeText(query);
                app_.windowMgr().pressKey("enter");
                return {true, "Opened " + app + " and searched for: " + query};
            }
            
            // Generic: type the query
            app_.windowMgr().waitAndFocus(app, 3.0f);
            app_.windowMgr().typeText(query);
            return {true, "Opened " + app + " and typed: " + query};
        });
    // "open X and type Y"
    addPattern("open_and_type",
        R"((?:open|launch)\s+(.+?)\s+and\s+(?:type|write|enter)\s+[\"']?(.+?)[\"']?$)",
        [this](const std::smatch& m) -> std::pair<bool, std::string> {
            std::string app = m[1].str();
            std::string text = m[2].str();
            
            app_.instantExecute("open " + app);
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            app_.windowMgr().waitAndFocus(app, 3.0f);
            app_.windowMgr().typeText(text);
            return {true, "Opened " + app + " and typed: " + text};
        });

    // "open X and paste Y"
    addPattern("open_and_paste",
        R"((?:open|launch)\s+(.+?)\s+and\s+(?:paste|set\s+clipboard\s+to)\s+(.+?)$)",
        [this](const std::smatch& m) -> std::pair<bool, std::string> {
            std::string app = m[1].str();
            std::string text = m[2].str();
            
            app_.instantExecute("open " + app);
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            app_.windowMgr().waitAndFocus(app, 3.0f);
            app_.windowMgr().setClipboard(text);
            app_.windowMgr().pasteClipboard();
            return {true, "Opened " + app + " and pasted: " + text};
        });
    
    // "list downloads in notepad"
    addPattern("list_in_app",
        R"((?:list|show)\s+(.+?)\s+in\s+(.+?)$)",
        [this](const std::smatch& m) -> std::pair<bool, std::string> {
            std::string what = m[1].str();
            std::string app = m[2].str();
            
            // Get file list first
            std::string list_result = app_.instantExecute("list " + what);
            
            // Open the app and type the list
            app_.instantExecute("open " + app);
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            app_.windowMgr().waitAndFocus(app, 3.0f);
            app_.windowMgr().typeText(list_result);
            return {true, "Listed " + what + " in " + app};
        });
    
    // "organize downloads"
    addPattern("organize_folder",
        R"((?:organize|sort|categorize)\s+(?:my\s+)?(.+?)$)",
        [this](const std::smatch& m) -> std::pair<bool, std::string> {
            std::string folder = m[1].str();
            std::string lower = folder;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            
            std::string path;
            if (lower == "downloads") {
                char prof[MAX_PATH]; SHGetFolderPathA(nullptr, CSIDL_PROFILE, nullptr, 0, prof);
                path = std::string(prof) + "\\Downloads";
            } else if (lower == "desktop") {
                char prof[MAX_PATH]; SHGetFolderPathA(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, prof);
                path = std::string(prof);
            } else {
                path = folder;
            }
            
            auto organized = app_.fileMgr().organizeByType(path);
            std::ostringstream ss;
            ss << "Files in " << folder << " by type:\n";
            for (auto& [cat, files] : organized.items()) {
                ss << "\n" << cat << " (" << files.size() << "):\n";
                for (auto& f : files) ss << "  - " << f.get<std::string>() << "\n";
            }
            return {true, ss.str()};
        });
    
    // "move X to Y"
    addPattern("move_file",
        R"((?:move|transfer)\s+(.+?)\s+(?:to|into)\s+(.+?)$)",
        [this](const std::smatch& m) -> std::pair<bool, std::string> {
            return app_.fileMgr().moveFile(m[1].str(), m[2].str());
        });
    
    // "copy X to Y"
    addPattern("copy_file",
        R"((?:copy|duplicate)\s+(.+?)\s+(?:to|into)\s+(.+?)$)",
        [this](const std::smatch& m) -> std::pair<bool, std::string> {
            return app_.fileMgr().copyFile(m[1].str(), m[2].str());
        });
}

void FastComplexHandler::addPattern(
    const std::string& name,
    const std::string& regex_pattern,
    std::function<std::pair<bool, std::string>(const std::smatch&)> handler) {
    
    Pattern p;
    p.name = name;
    p.regex = std::regex(regex_pattern, std::regex::icase);
    p.handler = std::move(handler);
    patterns_.push_back(std::move(p));
}

std::optional<std::pair<bool, std::string>> FastComplexHandler::tryHandle(
    const std::string& command) {
    
    for (const auto& p : patterns_) {
        std::smatch m;
        if (std::regex_match(command, m, p.regex)) {
            LOG_INFO("FastComplexHandler matched: {}", p.name);
            return p.handler(m);
        }
    }
    
    return std::nullopt;
}

bool FastComplexHandler::canHandle(const std::string& command) const {
    for (const auto& p : patterns_) {
        std::smatch m;
        std::string cmd = command;
        if (std::regex_match(cmd, m, p.regex)) return true;
    }
    return false;
}

} // namespace vision
