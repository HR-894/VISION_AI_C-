/**
 * @file web_search.cpp
 * @brief DuckDuckGo Instant Answer API integration via WinHTTP
 */

#include "web_search.h"
#include <windows.h>
#include <winhttp.h>
#include <sstream>
#include <algorithm>

#pragma comment(lib, "winhttp.lib")

#ifdef VISION_HAS_SPDLOG
#include <spdlog/spdlog.h>
#define LOG_INFO(...) spdlog::info(__VA_ARGS__)
#define LOG_WARN(...) spdlog::warn(__VA_ARGS__)
#else
#define LOG_INFO(...) (void)0
#define LOG_WARN(...) (void)0
#endif

using json = nlohmann::json;

namespace vision {

WebSearch::WebSearch() = default;

std::string WebSearch::urlEncode(const std::string& text) {
    std::ostringstream encoded;
    for (char c : text) {
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

std::string WebSearch::httpGet(const std::string& host, const std::string& path) {
    std::string result;
    
    HINTERNET hSession = WinHttpOpen(L"VisionAI/1.0",
                                      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return result;
    
    // L1 fix: proper UTF-8 to UTF-16 conversion
    auto toWide = [](const std::string& s) -> std::wstring {
        int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        if (len <= 0) return {};
        std::wstring ws(len - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, ws.data(), len);
        return ws;
    };
    std::wstring whost = toWide(host);
    
    HINTERNET hConnect = WinHttpConnect(hSession, whost.c_str(),
                                         INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return result; }
    
    std::wstring wpath = toWide(path);
    
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", wpath.c_str(),
                                             nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return result;
    }
    
    // Set timeout
    DWORD timeout = timeout_ms_;
    WinHttpSetTimeouts(hRequest, timeout, timeout, timeout, timeout);
    
    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, nullptr)) {
        
        DWORD bytesAvailable;
        while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0) {
            std::vector<char> buffer(bytesAvailable);
            DWORD bytesRead;
            WinHttpReadData(hRequest, buffer.data(), bytesAvailable, &bytesRead);
            result.append(buffer.data(), bytesRead);
        }
    }
    
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    
    return result;
}

json WebSearch::search(const std::string& query) {
    std::string encoded = urlEncode(query);
    std::string path = "/?q=" + encoded + "&format=json&no_redirect=1";
    
    std::string response = httpGet("api.duckduckgo.com", path);
    
    json result;
    if (response.empty()) {
        result["error"] = "No response from DuckDuckGo API";
        return result;
    }
    
    try {
        auto data = json::parse(response);
        
        result["query"] = query;
        result["abstract"] = data.value("Abstract", "");
        result["abstract_source"] = data.value("AbstractSource", "");
        result["abstract_url"] = data.value("AbstractURL", "");
        result["answer"] = data.value("Answer", "");
        result["definition"] = data.value("Definition", "");
        
        // Related topics
        result["related"] = json::array();
        if (data.contains("RelatedTopics")) {
            for (auto& topic : data["RelatedTopics"]) {
                if (topic.contains("Text")) {
                    result["related"].push_back({
                        {"text", topic["Text"]},
                        {"url", topic.value("FirstURL", "")}
                    });
                }
            }
        }
        
    } catch (const std::exception& e) {
        result["error"] = std::string("Parse error: ") + e.what();
    }
    
    return result;
}

std::string WebSearch::quickAnswer(const std::string& query) {
    auto result = search(query);
    
    // Try answer first
    std::string answer = result.value("answer", "");
    if (!answer.empty()) return answer;
    
    // Then abstract
    std::string abstract = result.value("abstract", "");
    if (!abstract.empty()) return abstract;
    
    // Then definition
    std::string def = result.value("definition", "");
    if (!def.empty()) return def;
    
    // Then first related topic
    if (result.contains("related") && !result["related"].empty()) {
        return result["related"][0].value("text", "No results found");
    }
    
    return "No results found for: " + query;
}

} // namespace vision
