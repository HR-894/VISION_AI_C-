/**
 * @file web_plugin.cpp
 * @brief Example Web Browsing plugin for VISION AI
 *
 * Exposes: search_web_plugin, open_url_plugin
 * Build as: add_library(web_plugin SHARED ...)
 */

#define VISION_PLUGIN_EXPORT
#include "i_action_plugin.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <cctype>

namespace {

class WebPlugin final : public vision::IActionPlugin {
public:
    bool initialize() override { return true; }
    void shutdown() override {}

    std::string pluginName() const override { return "WebBrowserPlugin"; }

    std::vector<vision::ToolManifest> getManifest() const override {
        return {
            {
                "search_web_plugin",
                "Search the web using Google in the default browser",
                "web",
                {
                    {"type", "object"},
                    {"properties", {
                        {"query", {{"type", "string"}, {"description", "Search query"}}}
                    }},
                    {"required", nlohmann::json::array({"query"})}
                }
            },
            {
                "open_url_plugin",
                "Open a URL in the default browser",
                "web",
                {
                    {"type", "object"},
                    {"properties", {
                        {"url", {{"type", "string"}, {"description", "URL to open"}}}
                    }},
                    {"required", nlohmann::json::array({"url"})}
                }
            }
        };
    }

    std::pair<bool, std::string> execute(
        const std::string& tool_name,
        const nlohmann::json& params) override {

        if (tool_name == "search_web_plugin") {
            std::string query = params.value("query", "");
            if (query.empty()) return {false, "No query provided"};

            // URL-encode
            std::string encoded;
            for (unsigned char c : query) {
                if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
                    encoded += c;
                else if (c == ' ')
                    encoded += '+';
                else {
                    char hex[4];
                    snprintf(hex, sizeof(hex), "%%%02X", c);
                    encoded += hex;
                }
            }
            std::string url = "https://www.google.com/search?q=" + encoded;
            ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOW);
            return {true, "Opened search: " + query};
        }

        if (tool_name == "open_url_plugin") {
            std::string url = params.value("url", "");
            if (url.empty()) return {false, "No URL provided"};
            if (url.find("://") == std::string::npos) url = "https://" + url;
            ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOW);
            return {true, "Opened: " + url};
        }

        return {false, "Unknown tool: " + tool_name};
    }
};

} // anonymous namespace

extern "C" VISION_PLUGIN_API vision::IActionPlugin* create_plugin() {
    return new WebPlugin();
}

extern "C" VISION_PLUGIN_API void destroy_plugin(vision::IActionPlugin* p) {
    delete p;
}
