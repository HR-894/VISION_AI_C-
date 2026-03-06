/**
 * @file action_executor.cpp
 * @brief Multi-step action execution with OCR + template matching
 */

#include "action_executor.h"
#include "vision_ai.h"
#include "ui_automation.h"
#include <thread>
#include <chrono>
#include <sstream>
#include <filesystem>

#ifdef VISION_HAS_OCR
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/highgui.hpp>
#include <tesseract/baseapi.h>
#include <leptonica/allheaders.h>
#endif

#ifdef VISION_HAS_SPDLOG
#include <spdlog/spdlog.h>
#define LOG_INFO(...) spdlog::info(__VA_ARGS__)
#define LOG_WARN(...) spdlog::warn(__VA_ARGS__)
#define LOG_ERROR(...) spdlog::error(__VA_ARGS__)
#else
#define LOG_INFO(...) (void)0
#define LOG_WARN(...) (void)0
#define LOG_ERROR(...) (void)0
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace vision {

// File-local UI Automation instance (lazy — COM init happens once)
static UIAutomation& getUIAuto() {
    static UIAutomation instance;
    return instance;
}

ActionExecutor::ActionExecutor(VisionAI& app) : app_(app) {
    initActionMap();
}

ActionExecutor::~ActionExecutor() = default;

bool ActionExecutor::isOCRAvailable() {
#ifdef VISION_HAS_OCR
    return true;
#else
    return false;
#endif
}

void ActionExecutor::initActionMap() {
    registerAction("open_app", [this](const json& p) { return actionOpenApp(p); });
    registerAction("open_url", [this](const json& p) { return actionOpenUrl(p); });
    registerAction("search_web", [this](const json& p) { return actionSearchWeb(p); });
    registerAction("search_in_browser", [this](const json& p) { return actionSearchInBrowser(p); });
    registerAction("type_text", [this](const json& p) { return actionTypeText(p); });
    registerAction("press_key", [this](const json& p) { return actionPressKey(p); });
    registerAction("click_element", [this](const json& p) { return actionClickElement(p); });
    registerAction("scroll", [this](const json& p) { return actionScroll(p); });
    registerAction("read_screen", [this](const json& p) { return actionReadScreen(p); });
    registerAction("wait", [this](const json& p) { return actionWait(p); });
    registerAction("set_volume", [this](const json& p) { return actionSetVolume(p); });
    registerAction("set_brightness", [this](const json& p) { return actionSetBrightness(p); });
    registerAction("minimize", [this](const json& p) { return actionMinimize(p); });
    registerAction("maximize", [this](const json& p) { return actionMaximize(p); });
    registerAction("close_window", [this](const json& p) { return actionCloseWindow(p); });
    registerAction("focus_window", [this](const json& p) { return actionFocusWindow(p); });
    registerAction("screenshot", [this](const json& p) { return actionScreenshot(p); });
    registerAction("list_files", [this](const json& p) { return actionListFiles(p); });
    registerAction("move_file", [this](const json& p) { return actionMoveFile(p); });
    registerAction("copy_file", [this](const json& p) { return actionCopyFile(p); });
    registerAction("delete_file", [this](const json& p) { return actionDeleteFile(p); });
    registerAction("clipboard_get", [this](const json& p) { return actionClipboardGet(p); });
    registerAction("clipboard_set", [this](const json& p) { return actionClipboardSet(p); });
    registerAction("task_complete", [this](const json& p) { return actionTaskComplete(p); });
    registerAction("get_ui_tree", [this](const json& p) { return actionGetUITree(p); });
}

void ActionExecutor::registerAction(const std::string& name, ActionHandler handler) {
    action_map_[name] = std::move(handler);
}

std::pair<bool, std::string> ActionExecutor::executeAction(const std::string& action,
                                                            const json& params) {
    auto it = action_map_.find(action);
    if (it != action_map_.end()) {
        try {
            return it->second(params);
        } catch (const std::exception& e) {
            return {false, "Action error: " + std::string(e.what())};
        }
    }
    return {false, "Unknown action: " + action};
}

std::pair<bool, std::vector<std::string>> ActionExecutor::executePlan(
    const std::vector<json>& actions) {
    std::vector<std::string> results;
    bool all_ok = true;
    
    for (const auto& action : actions) {
        std::string name = action.value("action", "");
        auto params = action.value("params", json::object());
        
        auto [success, result] = executeAction(name, params);
        results.push_back(result);
        
        if (!success) {
            all_ok = false;
            LOG_WARN("Plan step failed: {} - {}", name, result);
        }
        
        // Delay between steps
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    
    return {all_ok, results};
}

// ═══════════════════ OCR / Template Matching ═══════════════════

#ifdef VISION_HAS_OCR
cv::Mat ActionExecutor::captureScreen(std::optional<RECT> region) {
    int x = 0, y = 0, w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
    if (region) {
        x = region->left; y = region->top;
        w = region->right - region->left;
        h = region->bottom - region->top;
    }
    
    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP bmp = CreateCompatibleBitmap(screenDC, w, h);
    HGDIOBJ oldBmp = SelectObject(memDC, bmp);  // C6 fix: save old bitmap
    BitBlt(memDC, 0, 0, w, h, screenDC, x, y, SRCCOPY);
    
    BITMAPINFOHEADER bi{};
    bi.biSize = sizeof(bi);
    bi.biWidth = w;
    bi.biHeight = -h;
    bi.biPlanes = 1;
    bi.biBitCount = 24;
    bi.biCompression = BI_RGB;
    
    cv::Mat mat(h, w, CV_8UC3);
    GetDIBits(memDC, bmp, 0, h, mat.data, (BITMAPINFO*)&bi, DIB_RGB_COLORS);
    
    SelectObject(memDC, oldBmp);  // C6 fix: restore before delete
    DeleteObject(bmp);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
    
    return mat;
}

cv::Mat ActionExecutor::preprocessForOCR(const cv::Mat& screenshot) {
    cv::Mat gray, processed;
    cv::cvtColor(screenshot, gray, cv::COLOR_BGR2GRAY);
    cv::threshold(gray, processed, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);
    // Scale up for better OCR
    cv::resize(processed, processed, cv::Size(), 2.0, 2.0, cv::INTER_CUBIC);
    return processed;
}

OCRResult ActionExecutor::runOCR(const cv::Mat& screenshot, bool preprocess) {
    OCRResult result;
    
    cv::Mat img = preprocess ? preprocessForOCR(screenshot) : screenshot;
    
    auto* api = new tesseract::TessBaseAPI();
    if (api->Init(nullptr, "eng") != 0) {
        LOG_ERROR("Tesseract init failed");
        delete api;
        return result;
    }
    
    api->SetImage(img.data, img.cols, img.rows,
                   img.channels(), (int)img.step);
    
    char* text = api->GetUTF8Text();
    if (text) {
        result.full_text = text;
        result.valid = true;
        delete[] text;
    }
    
    // Get word-level bounding boxes
    auto* ri = api->GetIterator();
    if (ri) {
        float scale = preprocess ? 0.5f : 1.0f;
        do {
            const char* word = ri->GetUTF8Text(tesseract::RIL_WORD);
            if (word) {
                OCRResult::Word w;
                w.text = word;
                w.confidence = ri->Confidence(tesseract::RIL_WORD);
                int x1, y1, x2, y2;
                ri->BoundingBox(tesseract::RIL_WORD, &x1, &y1, &x2, &y2);
                w.bbox = {(LONG)(x1*scale), (LONG)(y1*scale),
                          (LONG)(x2*scale), (LONG)(y2*scale)};
                result.words.push_back(w);
                delete[] word;
            }
        } while (ri->Next(tesseract::RIL_WORD));
        delete ri;  // L4 fix: free the ResultIterator
    }
    
    api->End();
    delete api;
    
    return result;
}
#endif

std::optional<ScreenPoint> ActionExecutor::findElementOCR(const std::string& text,
                                                            int min_confidence) {
#ifdef VISION_HAS_OCR
    auto screenshot = captureScreen();
    auto ocr = runOCR(screenshot);
    
    std::string lower_text = text;
    std::transform(lower_text.begin(), lower_text.end(), lower_text.begin(), ::tolower);
    
    for (const auto& word : ocr.words) {
        std::string lower_word = word.text;
        std::transform(lower_word.begin(), lower_word.end(), lower_word.begin(), ::tolower);
        
        if (lower_word.find(lower_text) != std::string::npos &&
            word.confidence >= min_confidence) {
            ScreenPoint pt;
            pt.x = (word.bbox.left + word.bbox.right) / 2;
            pt.y = (word.bbox.top + word.bbox.bottom) / 2;
            return pt;
        }
    }
#else
    (void)text; (void)min_confidence;
#endif
    return std::nullopt;
}

std::pair<bool, std::string> ActionExecutor::findAndClick(const std::string& element,
                                                           int retry) {
    // ── Strategy 1: Try UI Automation first (instant, precise) ────────
    auto& uia = getUIAuto();
    if (uia.isAvailable()) {
        auto uia_elem = uia.findElement(element);
        if (uia_elem) {
            // Click the center of the UIA element's bounding rect
            int cx = (uia_elem->bounds.left + uia_elem->bounds.right) / 2;
            int cy = (uia_elem->bounds.top + uia_elem->bounds.bottom) / 2;
            SetCursorPos(cx, cy);
            INPUT inputs[2] = {};
            inputs[0].type = INPUT_MOUSE;
            inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
            inputs[1].type = INPUT_MOUSE;
            inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
            SendInput(2, inputs, sizeof(INPUT));
            LOG_INFO("Clicked via UIA: {} at ({}, {})", element, cx, cy);
            return {true, "Clicked (UIA): " + element};
        }
        LOG_INFO("UIA did not find '{}', falling back to OCR", element);
    }

    // ── Strategy 2: OCR fallback with optimized TessBaseAPI reuse ──────
#ifdef VISION_HAS_OCR
    // Initialize Tesseract ONCE outside the retry loop
    auto* api = new tesseract::TessBaseAPI();
    if (api->Init(nullptr, "eng") != 0) {
        LOG_ERROR("Tesseract init failed in findAndClick");
        delete api;
        return {false, "OCR init failed, element not found: " + element};
    }

    std::string lower_element = element;
    std::transform(lower_element.begin(), lower_element.end(),
                   lower_element.begin(), ::tolower);

    for (int i = 0; i < retry; i++) {
        // Capture a fresh screenshot each retry (window may have changed)
        auto screenshot = captureScreen();
        cv::Mat img = preprocessForOCR(screenshot);

        // Update the image data without reinitializing the engine
        api->SetImage(img.data, img.cols, img.rows,
                       img.channels(), (int)img.step);

        auto* ri = api->GetIterator();
        if (ri) {
            float scale = 0.5f;  // preprocessForOCR scales 2x
            do {
                const char* word = ri->GetUTF8Text(tesseract::RIL_WORD);
                if (word) {
                    std::string lower_word(word);
                    std::transform(lower_word.begin(), lower_word.end(),
                                   lower_word.begin(), ::tolower);
                    int conf = ri->Confidence(tesseract::RIL_WORD);

                    if (lower_word.find(lower_element) != std::string::npos &&
                        conf >= 50) {
                        int x1, y1, x2, y2;
                        ri->BoundingBox(tesseract::RIL_WORD, &x1, &y1, &x2, &y2);
                        int cx = (int)((x1 + x2) * scale / 2);
                        int cy = (int)((y1 + y2) * scale / 2);

                        delete[] word;
                        delete ri;
                        api->End();
                        delete api;

                        SetCursorPos(cx, cy);
                        INPUT inputs[2] = {};
                        inputs[0].type = INPUT_MOUSE;
                        inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
                        inputs[1].type = INPUT_MOUSE;
                        inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
                        SendInput(2, inputs, sizeof(INPUT));
                        LOG_INFO("Clicked via OCR: {} at ({}, {})", element, cx, cy);
                        return {true, "Clicked (OCR): " + element};
                    }
                    delete[] word;
                }
            } while (ri->Next(tesseract::RIL_WORD));
            delete ri;
        }

        api->Clear();  // Reset image data for next iteration (keeps engine alive)
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    api->End();
    delete api;
#else
    (void)retry;
#endif

    return {false, "Element not found: " + element};
}

std::string ActionExecutor::readScreenText(std::optional<RECT> region) {
#ifdef VISION_HAS_OCR
    auto screenshot = captureScreen(region);
    auto ocr = runOCR(screenshot);
    return ocr.full_text;
#else
    (void)region;
    return "[OCR not available]";
#endif
}

// ═══════════════════ Action Implementations ═══════════════════

std::pair<bool, std::string> ActionExecutor::actionOpenApp(const json& params) {
    std::string name = params.value("name", "");
    if (name.empty()) return {false, "No app name"};
    bool ok = app_.openApp(name);
    return {ok, ok ? "Opened " + name : "Failed to open " + name};
}

std::pair<bool, std::string> ActionExecutor::actionOpenUrl(const json& params) {
    std::string url = params.value("url", "");
    if (url.empty()) return {false, "No URL"};
    bool ok = app_.openUrl(url);
    return {ok, ok ? "Opened " + url : "Failed to open " + url};
}

std::pair<bool, std::string> ActionExecutor::actionSearchWeb(const json& params) {
    std::string query = params.value("query", "");
    if (query.empty()) return {false, "No query"};
    // Offline-only: route all web searches through the local browser
    std::string browser = params.value("browser", "edge");
    app_.sysCmds().searchInBrowser(query, browser);
    return {true, "🔍 Opened search in browser: " + query};
}

std::pair<bool, std::string> ActionExecutor::actionSearchInBrowser(const json& params) {
    std::string query = params.value("query", "");
    std::string browser = params.value("browser", "edge");
    app_.sysCmds().searchInBrowser(query, browser);
    return {true, "Searching for: " + query};
}

std::pair<bool, std::string> ActionExecutor::actionTypeText(const json& params) {
    std::string text = params.value("text", "");
    std::string target = params.value("target", "");

    // If no target specified, use the last active app from context manager
    // This prevents accidentally typing into Vision AI's own command box
    if (target.empty()) {
        auto ctx = app_.contextMgr().getActiveApp();
        if (!ctx.name.empty()) {
            target = ctx.name;
            LOG_INFO("actionTypeText: no target specified, using context app: {}", target);
        }
    }

    app_.windowMgr().typeText(text, 0.02f, target);
    return {true, "Typed: " + text};
}

std::pair<bool, std::string> ActionExecutor::actionPressKey(const json& params) {
    std::string key = params.value("key", "");
    app_.windowMgr().pressKey(key);
    return {true, "Pressed: " + key};
}

std::pair<bool, std::string> ActionExecutor::actionClickElement(const json& params) {
    std::string element = params.value("element", params.value("text", ""));
    return findAndClick(element);
}

std::pair<bool, std::string> ActionExecutor::actionScroll(const json& params) {
    std::string dir = params.value("direction", "down");
    int amount = params.value("amount", 3);
    app_.windowMgr().scrollPage(dir, amount);
    return {true, "Scrolled " + dir};
}

std::pair<bool, std::string> ActionExecutor::actionReadScreen(const json& params) {
    (void)params;
    std::string text = readScreenText();
    return {true, text.empty() ? "[No text found]" : text};
}

std::pair<bool, std::string> ActionExecutor::actionWait(const json& params) {
    int ms = params.value("ms", params.value("seconds", 1) * 1000);
    // FIX L6: Clamp to 30s max — LLM could output wait(999999) and block forever
    ms = std::clamp(ms, 0, 30000);
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    return {true, "Waited " + std::to_string(ms) + "ms"};
}

std::pair<bool, std::string> ActionExecutor::actionSetVolume(const json& params) {
    int level = params.value("level", 50);
    app_.windowMgr().setVolume(level);
    return {true, "Volume set to " + std::to_string(level) + "%"};
}

std::pair<bool, std::string> ActionExecutor::actionSetBrightness(const json& params) {
    int level = params.value("level", 50);
    return app_.sysCmds().setBrightness(level);
}

std::pair<bool, std::string> ActionExecutor::actionMinimize(const json& params) {
    std::string win = params.value("window", "");
    bool ok = app_.windowMgr().minimizeWindow(win);
    return {ok, ok ? "Minimized " + win : "Window not found: " + win};
}

std::pair<bool, std::string> ActionExecutor::actionMaximize(const json& params) {
    std::string win = params.value("window", "");
    bool ok = app_.windowMgr().maximizeWindow(win);
    return {ok, ok ? "Maximized " + win : "Window not found: " + win};
}

std::pair<bool, std::string> ActionExecutor::actionCloseWindow(const json& params) {
    std::string win = params.value("window", "");
    bool ok = app_.windowMgr().closeWindow(win);
    return {ok, ok ? "Closed " + win : "Window not found: " + win};
}

std::pair<bool, std::string> ActionExecutor::actionFocusWindow(const json& params) {
    std::string win = params.value("window", "");
    bool ok = app_.windowMgr().focusWindow(win);
    return {ok, ok ? "Focused " + win : "Window not found: " + win};
}

std::pair<bool, std::string> ActionExecutor::actionScreenshot(const json& params) {
    std::string path = params.value("path", "");
    std::string saved = app_.windowMgr().takeScreenshot(path);
    return {true, "Screenshot saved: " + saved};
}

std::pair<bool, std::string> ActionExecutor::actionListFiles(const json& params) {
    std::string dir = params.value("directory", params.value("path", ""));
    auto files = app_.fileMgr().listFiles(dir);
    std::ostringstream ss;
    for (const auto& f : files) {
        ss << (f.is_directory ? "[DIR] " : "      ") << f.name << "\n";
    }
    return {true, ss.str()};
}

std::pair<bool, std::string> ActionExecutor::actionMoveFile(const json& params) {
    return app_.fileMgr().moveFile(params.value("source", ""), params.value("dest", ""));
}

std::pair<bool, std::string> ActionExecutor::actionCopyFile(const json& params) {
    return app_.fileMgr().copyFile(params.value("source", ""), params.value("dest", ""));
}

std::pair<bool, std::string> ActionExecutor::actionDeleteFile(const json& params) {
    return app_.fileMgr().deleteToRecycleBin(params.value("path", ""));
}

std::pair<bool, std::string> ActionExecutor::actionClipboardGet(const json& params) {
    (void)params;
    return {true, app_.windowMgr().getClipboard()};
}

std::pair<bool, std::string> ActionExecutor::actionClipboardSet(const json& params) {
    std::string text = params.value("text", "");
    bool ok = app_.windowMgr().setClipboard(text);
    return {ok, ok ? "Clipboard set" : "Failed to set clipboard"};
}

std::pair<bool, std::string> ActionExecutor::actionTaskComplete(const json& params) {
    return {true, params.value("message", "Task completed")};
}

std::pair<bool, std::string> ActionExecutor::actionGetUITree(const json& params) {
    int depth = params.value("depth", 3);
    auto& uia = getUIAuto();
    if (!uia.isAvailable()) {
        return {false, "UI Automation not available (COM init failed)"};
    }
    auto tree = uia.getAccessibilityTree(depth);
    return {true, tree.dump(2)};
}

} // namespace vision
