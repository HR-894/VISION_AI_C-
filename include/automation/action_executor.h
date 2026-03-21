#pragma once
/**
 * @file action_executor.h
 * @brief Multi-step action executor with visual grounding (OCR + template matching)
 * 
 * Executes action plans from the ReAct agent. Supports OCR text finding
 * (Tesseract), screen reading, a comprehensive action dispatcher map,
 * and dynamic plugin loading from DLLs.
 */

#include <string>
#include <vector>
#include <optional>
#include <utility>
#include <unordered_map>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <memory>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifdef VISION_HAS_OCR
#include <opencv2/core.hpp>
#endif

namespace vision {

class VisionAI; // Forward declaration
class PluginLoader;  // Forward declaration

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

    // ── Plugin system ────────────────────────────────────────────
    /// Load plugins from a directory (called once at startup)
    int loadPlugins(const std::string& directory);

    /// Get all tool manifests from loaded plugins, optionally filtered
    std::vector<nlohmann::json> getPluginManifests(const std::string& category = "") const;

    /// Get all built-in action names (for swarm routing)
    std::vector<std::string> getBuiltinActionNames() const;

private:
    VisionAI& app_;
    std::unordered_map<std::string, ActionHandler> action_map_;
    std::unique_ptr<PluginLoader> plugin_loader_;

    // ── Screen capture ───────────────────────────────────────────
#ifdef VISION_HAS_OCR
    cv::Mat captureScreen(std::optional<RECT> region = std::nullopt);
    cv::Mat preprocessForOCR(const cv::Mat& screenshot);
    OCRResult runOCR(const cv::Mat& screenshot, bool preprocess = true);

    // FIX B1: Persistent Tesseract instance (init once, reuse per call)
    // Tesseract Init() loads ~20-50MB language models from disk — doing it
    // per-call causes Resource Thrashing. Singleton eliminates this.
    void* ocr_engine_ = nullptr;  // tesseract::TessBaseAPI* (opaque to avoid header)
    bool ocr_initialized_ = false;
    std::mutex ocr_mutex_;        // Tesseract is NOT thread-safe
    void initOCR();
    void shutdownOCR();
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
    std::pair<bool, std::string> actionRunPowerShell(const nlohmann::json& params);
    std::pair<bool, std::string> actionTaskComplete(const nlohmann::json& params);
    std::pair<bool, std::string> actionGetUITree(const nlohmann::json& params);

    // ── Document RAG (Phase 10) ──────────────────────────────────
    std::pair<bool, std::string> actionQueryDocuments(const nlohmann::json& params);
    std::pair<bool, std::string> actionIngestDocument(const nlohmann::json& params);
    
    // ── OS Semantic Timeline (Phase 11) ──────────────────────────
    std::pair<bool, std::string> actionSearchTimeline(const nlohmann::json& params);
};

} // namespace vision
