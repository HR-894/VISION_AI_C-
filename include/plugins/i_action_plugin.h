#pragma once
/**
 * @file i_action_plugin.h
 * @brief Dynamic Plugin Interface for VISION AI
 *
 * Plugins implement this interface and are loaded at runtime via
 * QLibrary / LoadLibrary from the `plugins/` directory.
 *
 * Each plugin DLL must export a C-linkage factory function:
 *   extern "C" __declspec(dllexport) IActionPlugin* create_plugin();
 *   extern "C" __declspec(dllexport) void destroy_plugin(IActionPlugin* p);
 */

#include <string>
#include <vector>
#include <utility>
#include <nlohmann/json.hpp>

#ifdef VISION_PLUGIN_EXPORT
#define VISION_PLUGIN_API __declspec(dllexport)
#else
#define VISION_PLUGIN_API __declspec(dllimport)
#endif

namespace vision {

/**
 * @brief Manifest describing a single tool exposed by a plugin.
 *
 * The ReAct agent injects these manifests into the LLM system prompt
 * so the model knows what tools are available and their JSON schemas.
 */
struct ToolManifest {
    std::string name;              ///< Unique action name (e.g. "spotify_play")
    std::string description;       ///< Human-readable description for the LLM
    std::string category;          ///< Routing category: "web", "media", "os", "file"
    nlohmann::json param_schema;   ///< JSON Schema for the `params` object
};

/**
 * @brief Abstract interface that all action plugins must implement.
 *
 * Lifecycle:
 *   1. `create_plugin()` factory instantiates the plugin.
 *   2. `initialize()` is called once after loading.
 *   3. `getManifest()` returns tool schemas for LLM prompt injection.
 *   4. `execute()` is called per action dispatch.
 *   5. `shutdown()` then `destroy_plugin()` on unload.
 */
class IActionPlugin {
public:
    virtual ~IActionPlugin() = default;

    /// Called once after DLL load — acquire resources, COM init, etc.
    virtual bool initialize() = 0;

    /// Called before DLL unload — release resources
    virtual void shutdown() = 0;

    /// Return the list of tools this plugin exposes
    virtual std::vector<ToolManifest> getManifest() const = 0;

    /// Execute a named tool with the given params
    /// @param tool_name  One of the names from getManifest()
    /// @param params     The JSON params object from the LLM output
    /// @return {success, result_message}
    virtual std::pair<bool, std::string> execute(
        const std::string& tool_name,
        const nlohmann::json& params) = 0;

    /// Human-readable plugin name (for logging)
    virtual std::string pluginName() const = 0;
};

} // namespace vision

// ── C-linkage factory typedefs (resolved via QLibrary) ───────────
extern "C" {
    typedef vision::IActionPlugin* (*CreatePluginFunc)();
    typedef void (*DestroyPluginFunc)(vision::IActionPlugin*);
}
