#pragma once
/**
 * @file web_search.h
 * @brief Web search via DuckDuckGo Instant Answer API
 * 
 * Provides quick answers and search results using WinHTTP.
 */

#include <string>
#include <nlohmann/json.hpp>

namespace vision {

class WebSearch {
public:
    WebSearch();

    /// Search DuckDuckGo and return structured results JSON
    nlohmann::json search(const std::string& query);

    /// Get a quick answer string (instant answer, abstract, or first related)
    std::string quickAnswer(const std::string& query);

private:
    int timeout_ms_ = 5000;

    std::string httpGet(const std::string& host, const std::string& path);
    std::string urlEncode(const std::string& text);
};

} // namespace vision
