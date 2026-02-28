#pragma once
/**
 * @file action_executor.h
 * @brief Multi-step action executor with visual grounding (OCR + template matching)
 * 
 * Executes action plans from the ReAct agent. Supports OCR text finding
 * (Tesseract), screen reading, and a comprehensive action dispatcher map.
 */

#include <string>
#include <vector>
#include <optional>
#include <utility>
#include <unordered_map>
#include <functional>
#include <nlohmann/json.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifdef VISION_HAS_OCR
#include <opencv2/core.hpp>
#endif

namespace vision {

class VisionAI; // Forward declaration

/// OCR result structure
struct OCRResult {
    std::string full_text;
    struct Word {
        std::string text;
        RECT bbox{};
        int confidence = 0;
    };
    std::vector<Word> words;
    bool valid = false;
};

/// Point on screen
struct ScreenPoint {
    int x = 0;
    int y = 0;
};

class ActionExecutor {
public:
    explicit ActionExecutor(VisionAI& app);
    ~ActionExecutor();

    /// Check if OCR is available
    static bool isOCRAvailable();

    // ── OCR text finding (Tesseract) ─────────────────────────────
    std::optional<ScreenPoint> findElementOCR(const std::string& text,
                                               int min_confidence = 50);

    // ── High-level actions ───────────────────────────────────────
    std::pair<bool, std::string> findAndClick(const std::string& element,
                                               int retry = 3);
    std::string readScreenText(std::optional<RECT> region = std::nullopt);

    // ── Plan execution ───────────────────────────────────────────
    std::pair<bool, std::vector<std::string>> executePlan(
        const std::vector<nlohmann::json>& actions);

    /// Execute a single named action with params
    std::pair<bool, std::string> executeAction(const std::string& action,
                                                const nlohmann::json& params);

    // ── Action map (string → handler) ────────────────────────────
    using ActionHandler = std::function<std::pair<bool, std::string>(
        const nlohmann::json& params)>;
    void registerAction(const std::string& name, ActionHandler handler);

private:
    VisionAI& app_;
    std::unordered_map<std::string, ActionHandler> action_map_;

    // ── Screen capture ───────────────────────────────────────────
#ifdef VISION_HAS_OCR
    cv::Mat captureScreen(std::optional<RECT> region = std::nullopt);
    cv::Mat preprocessForOCR(const cv::Mat& screenshot);
    OCRResult runOCR(const cv::Mat& screenshot, bool preprocess = true);
#endif

    // ── Action registration ──────────────────────────────────────
    void initActionMap();

    // ── Individual action implementations ────────────────────────
    std::pair<bool, std::string> actionOpenApp(const nlohmann::json& params);
    std::pair<bool, std::string> actionOpenUrl(const nlohmann::json& params);
    std::pair<bool, std::string> actionSearchWeb(const nlohmann::json& params);
    std::pair<bool, std::string> actionSearchInBrowser(const nlohmann::json& params);
    std::pair<bool, std::string> actionTypeText(const nlohmann::json& params);
    std::pair<bool, std::string> actionPressKey(const nlohmann::json& params);
    std::pair<bool, std::string> actionClickElement(const nlohmann::json& params);
    std::pair<bool, std::string> actionScroll(const nlohmann::json& params);
    std::pair<bool, std::string> actionReadScreen(const nlohmann::json& params);
    std::pair<bool, std::string> actionWait(const nlohmann::json& params);
    std::pair<bool, std::string> actionSetVolume(const nlohmann::json& params);
    std::pair<bool, std::string> actionSetBrightness(const nlohmann::json& params);
    std::pair<bool, std::string> actionMinimize(const nlohmann::json& params);
    std::pair<bool, std::string> actionMaximize(const nlohmann::json& params);
    std::pair<bool, std::string> actionCloseWindow(const nlohmann::json& params);
    std::pair<bool, std::string> actionFocusWindow(const nlohmann::json& params);
    std::pair<bool, std::string> actionScreenshot(const nlohmann::json& params);
    std::pair<bool, std::string> actionListFiles(const nlohmann::json& params);
    std::pair<bool, std::string> actionMoveFile(const nlohmann::json& params);
    std::pair<bool, std::string> actionCopyFile(const nlohmann::json& params);
    std::pair<bool, std::string> actionDeleteFile(const nlohmann::json& params);
    std::pair<bool, std::string> actionClipboardGet(const nlohmann::json& params);
    std::pair<bool, std::string> actionClipboardSet(const nlohmann::json& params);
    std::pair<bool, std::string> actionTaskComplete(const nlohmann::json& params);
};

} // namespace vision
