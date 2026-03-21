#pragma once
/**
 * @file plugin_loader.h
 * @brief Runtime DLL plugin loader for VISION AI
 *
 * Scans a directory for .dll files, loads them via QLibrary,
 * resolves the create_plugin/destroy_plugin factory symbols,
 * and registers all exposed tools into the ActionExecutor's action map.
 */

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>
#include <nlohmann/json.hpp>

#include "i_action_plugin.h"

// Forward declare QLibrary to avoid Qt header pollution
class QLibrary;

namespace vision {

/**
 * @brief Holds a loaded plugin instance + its DLL handle.
 */
struct LoadedPlugin {
    std::string dll_path;
    QLibrary* library = nullptr;            ///< Owns the DLL handle
    IActionPlugin* instance = nullptr;      ///< Created by factory
    DestroyPluginFunc destroy_fn = nullptr;  ///< Cleanup factory
    std::vector<ToolManifest> manifests;     ///< Cached tool schemas
};

/**
 * @brief Loads and manages action plugins from a directory.
 */
class PluginLoader {
public:
    PluginLoader() = default;
    ~PluginLoader();

    // Non-copyable
    PluginLoader(const PluginLoader&) = delete;
    PluginLoader& operator=(const PluginLoader&) = delete;

    /// Scan `directory` for .dll files, load each one
    /// @return Number of plugins successfully loaded
    int loadFromDirectory(const std::string& directory);

    /// Unload all plugins (called on shutdown)
    void unloadAll();

    /// Get all loaded plugins
    const std::vector<LoadedPlugin>& getPlugins() const { return plugins_; }

    /// Get all tool manifests across all plugins, optionally filtered by category
    std::vector<ToolManifest> getAllManifests(const std::string& category = "") const;

    /// Execute a tool by name (routes to the correct plugin)
    /// @return {success, result_message}
    std::pair<bool, std::string> executePluginTool(
        const std::string& tool_name,
        const nlohmann::json& params);

    /// Check if a tool name belongs to a loaded plugin
    bool hasPluginTool(const std::string& tool_name) const;

private:
    std::vector<LoadedPlugin> plugins_;
    /// tool_name → index into plugins_
    std::unordered_map<std::string, size_t> tool_to_plugin_;
};

} // namespace vision
