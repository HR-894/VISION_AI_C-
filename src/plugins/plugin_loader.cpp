/**
 * @file plugin_loader.cpp
 * @brief Runtime DLL plugin loader implementation
 *
 * Uses QLibrary to safely load/unload plugin .dll files and
 * resolve C-linkage factory functions.
 */

#include "plugin_loader.h"
#include <QLibrary>
#include <QDir>
#include <QFileInfo>
#include <filesystem>

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

namespace vision {

PluginLoader::~PluginLoader() {
    unloadAll();
}

int PluginLoader::loadFromDirectory(const std::string& directory) {
    namespace fs = std::filesystem;

    if (!fs::exists(directory) || !fs::is_directory(directory)) {
        LOG_WARN("Plugin directory does not exist: {}", directory);
        return 0;
    }

    int loaded = 0;
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (!entry.is_regular_file()) continue;

        auto ext = entry.path().extension().string();
        // Case-insensitive .dll check
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext != ".dll") continue;

        std::string dll_path = entry.path().string();
        LOG_INFO("Loading plugin: {}", dll_path);

        auto* lib = new QLibrary(QString::fromStdString(dll_path));
        if (!lib->load()) {
            LOG_ERROR("Failed to load plugin DLL: {} — {}", dll_path,
                      lib->errorString().toStdString());
            delete lib;
            continue;
        }

        // Resolve factory functions
        auto create_fn = reinterpret_cast<CreatePluginFunc>(
            lib->resolve("create_plugin"));
        auto destroy_fn = reinterpret_cast<DestroyPluginFunc>(
            lib->resolve("destroy_plugin"));

        if (!create_fn || !destroy_fn) {
            LOG_ERROR("Plugin {} missing create_plugin/destroy_plugin exports", dll_path);
            lib->unload();
            delete lib;
            continue;
        }

        // Instantiate plugin
        IActionPlugin* instance = create_fn();
        if (!instance) {
            LOG_ERROR("create_plugin() returned nullptr for {}", dll_path);
            lib->unload();
            delete lib;
            continue;
        }

        // Initialize
        if (!instance->initialize()) {
            LOG_ERROR("Plugin {} failed to initialize", instance->pluginName());
            destroy_fn(instance);
            lib->unload();
            delete lib;
            continue;
        }

        // Cache manifests and build routing table
        LoadedPlugin lp;
        lp.dll_path = dll_path;
        lp.library = lib;
        lp.instance = instance;
        lp.destroy_fn = destroy_fn;
        lp.manifests = instance->getManifest();

        size_t plugin_idx = plugins_.size();
        for (const auto& m : lp.manifests) {
            if (tool_to_plugin_.count(m.name)) {
                LOG_WARN("Tool '{}' already registered — overriding with plugin {}",
                         m.name, instance->pluginName());
            }
            tool_to_plugin_[m.name] = plugin_idx;
        }

        LOG_INFO("Loaded plugin '{}' with {} tools from {}",
                 instance->pluginName(), lp.manifests.size(), dll_path);
        plugins_.push_back(std::move(lp));
        loaded++;
    }

    LOG_INFO("Plugin loader: {} plugins loaded from {}", loaded, directory);
    return loaded;
}

void PluginLoader::unloadAll() {
    for (auto& lp : plugins_) {
        if (lp.instance) {
            lp.instance->shutdown();
            if (lp.destroy_fn) {
                lp.destroy_fn(lp.instance);
            }
            lp.instance = nullptr;
        }
        if (lp.library) {
            lp.library->unload();
            delete lp.library;
            lp.library = nullptr;
        }
    }
    plugins_.clear();
    tool_to_plugin_.clear();
}

std::vector<ToolManifest> PluginLoader::getAllManifests(const std::string& category) const {
    std::vector<ToolManifest> result;
    for (const auto& lp : plugins_) {
        for (const auto& m : lp.manifests) {
            if (category.empty() || m.category == category) {
                result.push_back(m);
            }
        }
    }
    return result;
}

std::pair<bool, std::string> PluginLoader::executePluginTool(
    const std::string& tool_name, const nlohmann::json& params) {

    auto it = tool_to_plugin_.find(tool_name);
    if (it == tool_to_plugin_.end()) {
        return {false, "Unknown plugin tool: " + tool_name};
    }

    auto& lp = plugins_[it->second];
    if (!lp.instance) {
        return {false, "Plugin instance is null for tool: " + tool_name};
    }

    return lp.instance->execute(tool_name, params);
}

bool PluginLoader::hasPluginTool(const std::string& tool_name) const {
    return tool_to_plugin_.count(tool_name) > 0;
}

} // namespace vision
