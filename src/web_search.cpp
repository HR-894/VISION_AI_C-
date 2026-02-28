/**
 * @file web_search.cpp
 * @brief Offline-only web search — all queries open in the local browser
 *
 * No outbound HTTP requests. The AI never phones home.
 */

#include "web_search.h"
#include <sstream>

#ifdef VISION_HAS_SPDLOG
#include <spdlog/spdlog.h>
#define LOG_INFO(...) spdlog::info(__VA_ARGS__)
#else
#define LOG_INFO(...) (void)0
#endif

using json = nlohmann::json;

namespace vision {

std::string WebSearch::urlEncode(const std::string& text) {
    std::ostringstream encoded;
    for (unsigned char c : text) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            encoded << c;
        else if (c == ' ')
            encoded << '+';
        else {
            encoded << '%' << std::hex << std::uppercase
                    << static_cast<int>((c >> 4) & 0x0F)
                    << static_cast<int>(c & 0x0F);
        }
    }
    return encoded.str();
}

json WebSearch::search(const std::string& query) {
    json result;
    result["query"] = query;
    result["offline"] = true;

    if (browser_search_) {
        browser_search_(query, default_browser_);
        result["status"] = "opened_in_browser";
        result["message"] = "Search opened in your local browser: " + query;
        LOG_INFO("Offline search → browser: {}", query);
    } else {
        result["status"] = "no_browser_callback";
        result["message"] = "Search not available (no browser callback configured)";
    }

    return result;
}

std::string WebSearch::quickAnswer(const std::string& query) {
    if (browser_search_) {
        browser_search_(query, default_browser_);
        return "🔍 Opened search in your browser: " + query;
    }
    return "Search not available offline. Try: search in browser " + query;
}

} // namespace vision
