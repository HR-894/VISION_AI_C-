/**
 * @file ui_automation.cpp
 * @brief Microsoft UI Automation (UIA) integration
 *
 * Uses the IUIAutomation COM interface to walk the accessibility tree
 * of the foreground window and return structured element data.
 */

#include "ui_automation.h"
#include <windows.h>
#include <uiautomation.h>
#include <comdef.h>
#include <sstream>

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

using json = nlohmann::json;

namespace vision {

// ═══════════════════ PIMPL Implementation ═══════════════════

struct UIAutomation::Impl {
    struct ComScope {
        HRESULT hr;
        ComScope() { hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
        ~ComScope() {
            if (hr == S_OK || hr == S_FALSE) {
                CoUninitialize();
            }
        }
    };
    
    ComScope com_scope;
    IUIAutomation* automation = nullptr;
    bool initialized = false;

    Impl() {
        HRESULT hr = CoCreateInstance(__uuidof(CUIAutomation), nullptr,
                                      CLSCTX_INPROC_SERVER,
                                      __uuidof(IUIAutomation),
                                      reinterpret_cast<void**>(&automation));
        if (SUCCEEDED(hr) && automation) {
            initialized = true;
            LOG_INFO("UI Automation COM initialized");
        } else {
            LOG_ERROR("Failed to initialize UI Automation COM (hr=0x{:08x})", (unsigned)hr);
        }
    }

    ~Impl() {
        if (automation) automation->Release();
    }

    /// Convert a CONTROLTYPEID to a human-readable string
    static std::string controlTypeToString(CONTROLTYPEID typeId) {
        switch (typeId) {
            case UIA_ButtonControlTypeId:      return "Button";
            case UIA_CalendarControlTypeId:     return "Calendar";
            case UIA_CheckBoxControlTypeId:     return "CheckBox";
            case UIA_ComboBoxControlTypeId:     return "ComboBox";
            case UIA_EditControlTypeId:         return "Edit";
            case UIA_HyperlinkControlTypeId:    return "Hyperlink";
            case UIA_ImageControlTypeId:        return "Image";
            case UIA_ListItemControlTypeId:     return "ListItem";
            case UIA_ListControlTypeId:         return "List";
            case UIA_MenuControlTypeId:         return "Menu";
            case UIA_MenuBarControlTypeId:      return "MenuBar";
            case UIA_MenuItemControlTypeId:     return "MenuItem";
            case UIA_ProgressBarControlTypeId:  return "ProgressBar";
            case UIA_RadioButtonControlTypeId:  return "RadioButton";
            case UIA_ScrollBarControlTypeId:    return "ScrollBar";
            case UIA_SliderControlTypeId:       return "Slider";
            case UIA_SpinnerControlTypeId:      return "Spinner";
            case UIA_StatusBarControlTypeId:    return "StatusBar";
            case UIA_TabControlTypeId:          return "Tab";
            case UIA_TabItemControlTypeId:      return "TabItem";
            case UIA_TextControlTypeId:         return "Text";
            case UIA_ToolBarControlTypeId:      return "ToolBar";
            case UIA_ToolTipControlTypeId:      return "ToolTip";
            case UIA_TreeControlTypeId:         return "Tree";
            case UIA_TreeItemControlTypeId:     return "TreeItem";
            case UIA_CustomControlTypeId:       return "Custom";
            case UIA_GroupControlTypeId:        return "Group";
            case UIA_ThumbControlTypeId:        return "Thumb";
            case UIA_DataGridControlTypeId:     return "DataGrid";
            case UIA_DataItemControlTypeId:     return "DataItem";
            case UIA_DocumentControlTypeId:     return "Document";
            case UIA_SplitButtonControlTypeId:  return "SplitButton";
            case UIA_WindowControlTypeId:       return "Window";
            case UIA_PaneControlTypeId:         return "Pane";
            case UIA_HeaderControlTypeId:       return "Header";
            case UIA_HeaderItemControlTypeId:   return "HeaderItem";
            case UIA_TableControlTypeId:        return "Table";
            case UIA_TitleBarControlTypeId:     return "TitleBar";
            case UIA_SeparatorControlTypeId:    return "Separator";
            default:                            return "Unknown";
        }
    }

    /// Extract a BSTR to std::string
    static std::string bstrToString(BSTR bstr) {
        int len = WideCharToMultiByte(CP_UTF8, 0, bstr, -1, nullptr, 0, nullptr, nullptr);
        if (len <= 0) return "";
        // PRD Fix 4: Allocate full space including null-terminator to prevent heap overrun
        std::string result(len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, bstr, -1, result.data(), len, nullptr, nullptr);
        if (!result.empty() && result.back() == '\0') result.pop_back();
        return result;
    }

    /// Read properties from a UIA element
    UIElement readElement(IUIAutomationElement* elem) {
        UIElement el;

        BSTR name = nullptr;
        if (SUCCEEDED(elem->get_CurrentName(&name)) && name) {
            el.name = bstrToString(name);
            SysFreeString(name);
        }

        CONTROLTYPEID typeId = 0;
        if (SUCCEEDED(elem->get_CurrentControlType(&typeId))) {
            el.control_type = controlTypeToString(typeId);
        }

        BSTR autoId = nullptr;
        if (SUCCEEDED(elem->get_CurrentAutomationId(&autoId)) && autoId) {
            el.automation_id = bstrToString(autoId);
            SysFreeString(autoId);
        }

        BSTR className = nullptr;
        if (SUCCEEDED(elem->get_CurrentClassName(&className)) && className) {
            el.class_name = bstrToString(className);
            SysFreeString(className);
        }

        RECT rect{};
        if (SUCCEEDED(elem->get_CurrentBoundingRectangle(&rect))) {
            el.bounds = rect;
        }

        BOOL enabled = FALSE;
        if (SUCCEEDED(elem->get_CurrentIsEnabled(&enabled))) {
            el.is_enabled = (enabled == TRUE);
        }

        BOOL offscreen = FALSE;
        if (SUCCEEDED(elem->get_CurrentIsOffscreen(&offscreen))) {
            el.is_offscreen = (offscreen == TRUE);
        }

        return el;
    }

    /// Recursively walk the tree
    json walkTree(IUIAutomationElement* elem, int depth, int max_depth) {
        if (!elem || depth > max_depth) return json();

        UIElement el = readElement(elem);

        json node;
        if (!el.name.empty()) node["name"] = el.name;
        node["type"] = el.control_type;
        if (!el.automation_id.empty()) node["id"] = el.automation_id;
        if (!el.class_name.empty()) node["class"] = el.class_name;
        node["enabled"] = el.is_enabled;
        if (el.bounds.right > el.bounds.left && el.bounds.bottom > el.bounds.top) {
            node["bounds"] = {
                {"x", el.bounds.left}, {"y", el.bounds.top},
                {"w", el.bounds.right - el.bounds.left},
                {"h", el.bounds.bottom - el.bounds.top}
            };
        }

        // Skip offscreen elements
        if (el.is_offscreen) return json();

        // Walk children
        if (depth < max_depth) {
            IUIAutomationTreeWalker* walker = nullptr;
            if (SUCCEEDED(automation->get_ControlViewWalker(&walker)) && walker) {
                json children = json::array();
                IUIAutomationElement* child = nullptr;
                if (SUCCEEDED(walker->GetFirstChildElement(elem, &child)) && child) {
                    int child_count = 0;
                    while (child && child_count < 50) {  // Cap children to avoid explosion
                        json child_node = walkTree(child, depth + 1, max_depth);
                        if (!child_node.is_null()) {
                            children.push_back(std::move(child_node));
                        }

                        IUIAutomationElement* sibling = nullptr;
                        HRESULT hr = walker->GetNextSiblingElement(child, &sibling);
                        child->Release();
                        child = (SUCCEEDED(hr)) ? sibling : nullptr;
                        child_count++;
                    }
                    if (child) child->Release();
                }
                walker->Release();

                if (!children.empty()) {
                    node["children"] = std::move(children);
                }
            }
        }

        return node;
    }

    /// Search for an element by name (BFS)
    std::optional<UIElement> searchByName(IUIAutomationElement* root, const std::string& name, int depth, int max_depth) {
        if (!root || depth > max_depth) return std::nullopt;

        UIElement el = readElement(root);
        std::string lower_name = name;
        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
        std::string lower_el_name = el.name;
        std::transform(lower_el_name.begin(), lower_el_name.end(), lower_el_name.begin(), ::tolower);

        if (lower_el_name.find(lower_name) != std::string::npos) {
            return el;
        }

        // Check automation ID too
        std::string lower_id = el.automation_id;
        std::transform(lower_id.begin(), lower_id.end(), lower_id.begin(), ::tolower);
        if (!lower_id.empty() && lower_id.find(lower_name) != std::string::npos) {
            return el;
        }

        // Recurse children
        IUIAutomationTreeWalker* walker = nullptr;
        if (SUCCEEDED(automation->get_ControlViewWalker(&walker)) && walker) {
            IUIAutomationElement* child = nullptr;
            if (SUCCEEDED(walker->GetFirstChildElement(root, &child)) && child) {
                while (child) {
                    auto found = searchByName(child, name, depth + 1, max_depth);
                    if (found) {
                        child->Release();
                        walker->Release();
                        return found;
                    }

                    IUIAutomationElement* sibling = nullptr;
                    HRESULT hr = walker->GetNextSiblingElement(child, &sibling);
                    child->Release();
                    child = (SUCCEEDED(hr)) ? sibling : nullptr;
                }
            }
            walker->Release();
        }

        return std::nullopt;
    }
};

// ═══════════════════ Public Interface ═══════════════════

UIAutomation::UIAutomation() : impl_(std::make_unique<Impl>()) {}
UIAutomation::~UIAutomation() = default;

bool UIAutomation::isAvailable() const {
    return impl_ && impl_->initialized;
}

json UIAutomation::getAccessibilityTree(int max_depth) {
    if (!isAvailable()) {
        return {{"error", "UI Automation not available"}};
    }

    HWND fg = GetForegroundWindow();
    if (!fg) {
        return {{"error", "No foreground window"}};
    }

    // Get window title for context
    wchar_t title[512];
    GetWindowTextW(fg, title, 512);
    int title_len = WideCharToMultiByte(CP_UTF8, 0, title, -1, nullptr, 0, nullptr, nullptr);
    // PRD Fix 4: Allocate full space including null-terminator to prevent heap overrun
    std::string window_title(title_len > 0 ? title_len : 0, '\0');
    if (title_len > 0) {
        WideCharToMultiByte(CP_UTF8, 0, title, -1, window_title.data(), title_len, nullptr, nullptr);
        if (!window_title.empty() && window_title.back() == '\0') window_title.pop_back();
    }

    IUIAutomationElement* root = nullptr;
    HRESULT hr = impl_->automation->ElementFromHandle(fg, &root);
    if (FAILED(hr) || !root) {
        return {{"error", "Failed to get UIA element from window"}};
    }

    json result;
    result["window"] = window_title;
    result["tree"] = impl_->walkTree(root, 0, max_depth);

    root->Release();
    return result;
}

std::optional<UIElement> UIAutomation::findElement(const std::string& name) {
    if (!isAvailable()) return std::nullopt;

    HWND fg = GetForegroundWindow();
    if (!fg) return std::nullopt;

    IUIAutomationElement* root = nullptr;
    HRESULT hr = impl_->automation->ElementFromHandle(fg, &root);
    if (FAILED(hr) || !root) return std::nullopt;

    auto result = impl_->searchByName(root, name, 0, 5);
    root->Release();
    return result;
}

} // namespace vision
