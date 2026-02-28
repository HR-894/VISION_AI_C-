#pragma once
/**
 * @file ui_automation.h
 * @brief Microsoft UI Automation (UIA) integration for semantic screen understanding
 *
 * Provides access to the accessibility tree of the active window, allowing the
 * LLM agent to understand UI elements (buttons, text fields, menus) semantically
 * without relying on OCR/screenshot processing.
 */

#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <nlohmann/json.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace vision {

/// Represents a single UI element from the accessibility tree
struct UIElement {
    std::string name;
    std::string control_type;
    std::string automation_id;
    std::string class_name;
    RECT bounds{};
    bool is_enabled = false;
    bool is_offscreen = false;
    std::vector<UIElement> children;
};

class UIAutomation {
public:
    UIAutomation();
    ~UIAutomation();

    // Non-copyable
    UIAutomation(const UIAutomation&) = delete;
    UIAutomation& operator=(const UIAutomation&) = delete;

    /// Get accessibility tree of the foreground window as JSON
    /// @param max_depth Maximum depth to walk (default 3 to avoid massive trees)
    nlohmann::json getAccessibilityTree(int max_depth = 3);

    /// Find an element by name or automation ID in the foreground window
    std::optional<UIElement> findElement(const std::string& name);

    /// Check if UI Automation COM is initialized
    bool isAvailable() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vision
