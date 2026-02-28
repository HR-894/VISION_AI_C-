/**
 * @file smart_template_matcher.cpp
 * @brief Regex-based command matching engine — handles ~90% of commands instantly
 */

#include "smart_template_matcher.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <sstream>

#ifdef VISION_HAS_SPDLOG
#include <spdlog/spdlog.h>
#define LOG_INFO(...) spdlog::info(__VA_ARGS__)
#else
#define LOG_INFO(...) (void)0
#endif

using json = nlohmann::json;

namespace vision {

SmartTemplateMatcher::SmartTemplateMatcher() {
    initTemplates();
}

void SmartTemplateMatcher::initTemplates() {
    // ── Application control ──────────────────────────────────────
    addTemplate("open_app",
        R"((?:open|launch|start|run)\s+(.+?)(?:\s+app(?:lication)?)?$)",
        {"app_name"}, CommandCategory::App, 10);
    
    addTemplate("close_app",
        R"((?:close|quit|exit|kill|end)\s+(.+?)$)",
        {"app_name"}, CommandCategory::App, 9);
    
    addTemplate("switch_to",
        R"((?:switch\s+to|go\s+to|focus(?:\s+on)?)\s+(.+?)$)",
        {"app_name"}, CommandCategory::App, 9);
    
    // ── Window management ────────────────────────────────────────
    addTemplate("minimize_window",
        R"((?:minimize|min)\s+(.+?)$)",
        {"window"}, CommandCategory::Window, 9);
    
    addTemplate("maximize_window",
        R"((?:maximize|max|fullscreen)\s+(.+?)$)",
        {"window"}, CommandCategory::Window, 9);
    
    addTemplate("minimize_all",
        R"((?:minimize\s+all|show\s+desktop|hide\s+(?:all\s+)?windows)$)",
        {}, CommandCategory::Window, 10);
    
    addTemplate("snap_left",
        R"((?:snap|move|put)\s+(?:this\s+)?(?:window\s+)?(?:to\s+)?(?:the\s+)?left$)",
        {}, CommandCategory::Window, 9);
    
    addTemplate("snap_right",
        R"((?:snap|move|put)\s+(?:this\s+)?(?:window\s+)?(?:to\s+)?(?:the\s+)?right$)",
        {}, CommandCategory::Window, 9);
    
    addTemplate("tile_windows",
        R"((?:tile|arrange|layout)\s+(?:all\s+)?windows$)",
        {}, CommandCategory::Window, 9);
    
    // ── Volume ───────────────────────────────────────────────────
    addTemplate("set_volume",
        R"((?:set\s+)?volume\s+(?:to\s+)?(\d+)(?:\s*%)?$)",
        {"level"}, CommandCategory::System, 10);
    
    addTemplate("volume_up",
        R"((?:volume\s+up|increase\s+volume|louder|turn\s+up(?:\s+volume)?)$)",
        {}, CommandCategory::System, 10);
    
    addTemplate("volume_down",
        R"((?:volume\s+down|decrease\s+volume|quieter|turn\s+down(?:\s+volume)?)$)",
        {}, CommandCategory::System, 10);
    
    addTemplate("mute",
        R"((?:mute|unmute|toggle\s+mute)$)",
        {}, CommandCategory::System, 10);
    
    // ── Brightness ───────────────────────────────────────────────
    addTemplate("set_brightness",
        R"((?:set\s+)?brightness\s+(?:to\s+)?(\d+)(?:\s*%)?$)",
        {"level"}, CommandCategory::System, 10);
    
    addTemplate("brightness_up",
        R"((?:brightness\s+up|increase\s+brightness|brighter)$)",
        {}, CommandCategory::System, 10);
    
    addTemplate("brightness_down",
        R"((?:brightness\s+down|decrease\s+brightness|dimmer|dim)$)",
        {}, CommandCategory::System, 10);
    
    // ── System info ──────────────────────────────────────────────
    addTemplate("battery",
        R"((?:battery|battery\s+(?:level|status|percent(?:age)?))$)",
        {}, CommandCategory::System, 10);
    
    addTemplate("storage",
        R"((?:storage|disk\s+space|free\s+space|how\s+much\s+(?:space|storage))$)",
        {}, CommandCategory::System, 10);
    
    addTemplate("uptime",
        R"((?:uptime|how\s+long\s+(?:has\s+)?(?:the\s+)?(?:computer|pc|system)\s+been\s+(?:on|running))$)",
        {}, CommandCategory::System, 10);
    
    addTemplate("system_info",
        R"((?:system\s+info(?:rmation)?|sys\s+info|about\s+(?:this\s+)?(?:computer|pc|system))$)",
        {}, CommandCategory::System, 10);
    
    // ── Network ──────────────────────────────────────────────────
    addTemplate("wifi_on",
        R"((?:turn\s+on\s+wifi|enable\s+wifi|wifi\s+on|connect\s+wifi)$)",
        {}, CommandCategory::System, 10);
    
    addTemplate("wifi_off",
        R"((?:turn\s+off\s+wifi|disable\s+wifi|wifi\s+off|disconnect\s+wifi)$)",
        {}, CommandCategory::System, 10);
    
    addTemplate("bluetooth_on",
        R"((?:turn\s+on\s+bluetooth|enable\s+bluetooth|bluetooth\s+on)$)",
        {}, CommandCategory::System, 10);
    
    addTemplate("bluetooth_off",
        R"((?:turn\s+off\s+bluetooth|disable\s+bluetooth|bluetooth\s+off)$)",
        {}, CommandCategory::System, 10);
    
    addTemplate("ip_address",
        R"((?:what(?:'?s)?\s+(?:my\s+)?ip(?:\s+address)?|show\s+ip|ip\s+address)$)",
        {}, CommandCategory::System, 10);
    
    // ── Power ────────────────────────────────────────────────────
    addTemplate("lock",
        R"((?:lock\s+(?:the\s+)?(?:computer|pc|screen|system)|lock)$)",
        {}, CommandCategory::System, 10);
    
    addTemplate("sleep",
        R"((?:sleep|go\s+to\s+sleep|put\s+(?:the\s+)?(?:computer|pc)\s+to\s+sleep)$)",
        {}, CommandCategory::System, 10);
    
    // ── Files ────────────────────────────────────────────────────
    addTemplate("list_downloads",
        R"((?:list|show|what(?:'?s)?\s+in)\s+(?:my\s+)?downloads$)",
        {}, CommandCategory::File, 10);
    
    addTemplate("list_desktop",
        R"((?:list|show|what(?:'?s)?\s+in)\s+(?:my\s+)?desktop$)",
        {}, CommandCategory::File, 10);
    
    addTemplate("list_documents",
        R"((?:list|show|what(?:'?s)?\s+in)\s+(?:my\s+)?documents$)",
        {}, CommandCategory::File, 10);
    
    addTemplate("search_files",
        R"((?:search|find|look\s+for)\s+(?:file(?:s)?\s+)?(?:named?\s+)?(.+?)(?:\s+in\s+(.+?))?$)",
        {"query", "directory"}, CommandCategory::File, 8);
    
    // ── Browser ──────────────────────────────────────────────────
    addTemplate("search_web",
        R"((?:search|google|look\s+up|find\s+(?:online|on\s+the\s+web))\s+(?:for\s+)?(.+?)$)",
        {"query"}, CommandCategory::Web, 8);
    
    addTemplate("open_url",
        R"((?:open|go\s+to|navigate\s+to|visit)\s+((?:https?://)?[\w.-]+\.(?:com|org|net|io|dev|ai|co|gov|edu)(?:/\S*)?)$)",
        {"url"}, CommandCategory::Web, 9);
    
    addTemplate("search_in_browser",
        R"((?:search|look\s+up)\s+(.+?)\s+(?:in|on|using)\s+(edge|chrome|firefox|brave)$)",
        {"query", "browser"}, CommandCategory::Web, 10);
    
    // ── Settings ─────────────────────────────────────────────────
    addTemplate("open_settings",
        R"((?:open|show|go\s+to)\s+(.+?)\s+settings$)",
        {"page"}, CommandCategory::System, 9);
    
    addTemplate("task_manager",
        R"((?:open\s+)?task\s+manager$)",
        {}, CommandCategory::System, 10);
    
    // ── Clipboard ────────────────────────────────────────────────
    addTemplate("clipboard_get",
        R"((?:what(?:'?s)?\s+(?:in\s+(?:my\s+)?)?clipboard|show\s+clipboard|paste|clipboard)$)",
        {}, CommandCategory::System, 10);
    
    addTemplate("clipboard_set",
        R"((?:copy|set\s+clipboard\s+to)\s+(.+?)$)",
        {"text"}, CommandCategory::System, 9);
    
    // ── Timer / stopwatch ────────────────────────────────────────
    addTemplate("set_timer",
        R"((?:set\s+(?:a\s+)?timer\s+(?:for\s+)?|timer\s+)(\d+)\s*(minutes?|mins?|seconds?|secs?|hours?)$)",
        {"amount", "unit"}, CommandCategory::System, 10);
    
    addTemplate("start_stopwatch",
        R"((?:start\s+(?:a\s+)?stopwatch|stopwatch\s+start)$)",
        {}, CommandCategory::System, 10);
    
    addTemplate("stop_stopwatch",
        R"((?:stop\s+(?:the\s+)?stopwatch|stopwatch\s+stop)$)",
        {}, CommandCategory::System, 10);
    
    // ── Screenshot ───────────────────────────────────────────────
    addTemplate("screenshot",
        R"((?:take\s+(?:a\s+)?screenshot|screenshot|screen\s+capture|print\s+screen)$)",
        {}, CommandCategory::System, 10);
    
    // ── Keyboard shortcuts ───────────────────────────────────────
    addTemplate("press_key",
        R"((?:press|hit|type)\s+((?:ctrl|alt|shift|win)\+\w+)$)",
        {"key"}, CommandCategory::System, 9);
    
    addTemplate("type_text",
        R"((?:type|write|enter)\s+[\"']?(.+?)[\"']?\s*(?:in(?:to)?\s+(.+?))?$)",
        {"text", "target"}, CommandCategory::App, 8);
    
    // ── Focus mode ───────────────────────────────────────────────
    addTemplate("focus_mode",
        R"((?:start\s+)?focus\s+mode(?:\s+(?:for\s+)?(\d+)\s*(?:min(?:ute)?s?)?)?$)",
        {"minutes"}, CommandCategory::System, 9);
    
    // ── Health scan ──────────────────────────────────────────────
    addTemplate("health_scan",
        R"((?:system\s+)?health\s+(?:scan|check)|run\s+diagnostics$)",
        {}, CommandCategory::System, 10);
    
    // ── Running apps ─────────────────────────────────────────────
    addTemplate("running_apps",
        R"((?:list|show|what(?:'?s)?)\s+(?:the\s+)?(?:running|open)\s+(?:app(?:lication)?s|programs|windows)$)",
        {}, CommandCategory::System, 10);
    
    // ── Help ─────────────────────────────────────────────────────
    addTemplate("help",
        R"((?:help|what\s+can\s+you\s+do|commands?|abilities)$)",
        {}, CommandCategory::System, 10);
}

void SmartTemplateMatcher::addTemplate(const std::string& name,
                                        const std::string& pattern,
                                        const std::vector<std::string>& vars,
                                        CommandCategory category,
                                        int priority) {
    CommandTemplate tmpl;
    tmpl.name = name;
    tmpl.pattern = std::regex(pattern, std::regex::icase);
    tmpl.variables = vars;
    tmpl.category = category;
    tmpl.priority = priority;
    templates_.push_back(std::move(tmpl));
}

std::optional<MatchResult> SmartTemplateMatcher::match(const std::string& text) {
    std::string input = text;
    // Trim
    input.erase(0, input.find_first_not_of(" \t"));
    input.erase(input.find_last_not_of(" \t") + 1);
    
    if (input.empty()) return std::nullopt;
    
    MatchResult best;
    bool found = false;
    
    for (const auto& tmpl : templates_) {
        std::smatch m;
        if (std::regex_match(input, m, tmpl.pattern)) {
            if (!found || tmpl.priority > best.confidence) {
                best.template_name = tmpl.name;
                best.confidence = tmpl.priority;
                best.category = tmpl.category;
                best.variables.clear();
                
                for (size_t i = 0; i < tmpl.variables.size() && i + 1 < m.size(); i++) {
                    std::string val = m[i + 1].str();
                    // Trim captured value
                    val.erase(0, val.find_first_not_of(" \t"));
                    val.erase(val.find_last_not_of(" \t") + 1);
                    if (!val.empty()) {
                        best.variables[tmpl.variables[i]] = val;
                    }
                }
                
                found = true;
            }
        }
    }
    
    if (found) return best;
    return std::nullopt;
}

std::vector<MatchResult> SmartTemplateMatcher::matchChained(const std::string& text) {
    std::vector<MatchResult> results;
    
    // Split on " and ", " then ", " also "
    std::regex separator(R"(\s+(?:and\s+then|and|then|also|after\s+that)\s+)",
                          std::regex::icase);
    
    std::sregex_token_iterator it(text.begin(), text.end(), separator, -1);
    std::sregex_token_iterator end;
    
    for (; it != end; ++it) {
        std::string part = it->str();
        part.erase(0, part.find_first_not_of(" \t"));
        part.erase(part.find_last_not_of(" \t") + 1);
        
        if (!part.empty()) {
            auto result = match(part);
            if (result) {
                results.push_back(*result);
            }
        }
    }
    
    return results;
}

bool SmartTemplateMatcher::canHandle(const std::string& text) {
    return match(text).has_value();
}

} // namespace vision
