/**
 * @file media_plugin.cpp
 * @brief Example Media/Spotify control plugin for VISION AI
 *
 * Exposes: media_play_pause, media_next, media_prev, media_volume
 * Uses Win32 virtual key simulation for universal media control.
 * Build as: add_library(media_plugin SHARED ...)
 */

#define VISION_PLUGIN_EXPORT
#include "i_action_plugin.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <algorithm>

namespace {

/// Send a virtual media key press
static void sendMediaKey(WORD vk) {
    INPUT inputs[2] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = vk;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = vk;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, inputs, sizeof(INPUT));
}

class MediaPlugin final : public vision::IActionPlugin {
public:
    bool initialize() override { return true; }
    void shutdown() override {}

    std::string pluginName() const override { return "MediaControlPlugin"; }

    std::vector<vision::ToolManifest> getManifest() const override {
        return {
            {
                "media_play_pause",
                "Toggle play/pause on the active media player (Spotify, etc.)",
                "media",
                {{"type", "object"}, {"properties", nlohmann::json::object()}}
            },
            {
                "media_next",
                "Skip to the next track in the active media player",
                "media",
                {{"type", "object"}, {"properties", nlohmann::json::object()}}
            },
            {
                "media_prev",
                "Go back to the previous track in the active media player",
                "media",
                {{"type", "object"}, {"properties", nlohmann::json::object()}}
            },
            {
                "media_volume",
                "Set the system volume to a specific level (0-100)",
                "media",
                {
                    {"type", "object"},
                    {"properties", {
                        {"level", {{"type", "integer"}, {"description", "Volume level 0-100"}}}
                    }},
                    {"required", nlohmann::json::array({"level"})}
                }
            }
        };
    }

    std::pair<bool, std::string> execute(
        const std::string& tool_name,
        const nlohmann::json& params) override {

        if (tool_name == "media_play_pause") {
            sendMediaKey(VK_MEDIA_PLAY_PAUSE);
            return {true, "Toggled play/pause"};
        }

        if (tool_name == "media_next") {
            sendMediaKey(VK_MEDIA_NEXT_TRACK);
            return {true, "Skipped to next track"};
        }

        if (tool_name == "media_prev") {
            sendMediaKey(VK_MEDIA_PREV_TRACK);
            return {true, "Went to previous track"};
        }

        if (tool_name == "media_volume") {
            int level = params.value("level", 50);
            level = std::clamp(level, 0, 100);

            // Use Core Audio API for precise volume control
            CoInitialize(nullptr);
            IMMDeviceEnumerator* enumerator = nullptr;
            HRESULT hr = CoCreateInstance(
                __uuidof(MMDeviceEnumerator), nullptr,
                CLSCTX_INPROC_SERVER, __uuidof(IMMDeviceEnumerator),
                (void**)&enumerator);

            if (SUCCEEDED(hr) && enumerator) {
                IMMDevice* device = nullptr;
                if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device))) {
                    IAudioEndpointVolume* volume = nullptr;
                    if (SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume),
                                                    CLSCTX_INPROC_SERVER, nullptr,
                                                    (void**)&volume))) {
                        volume->SetMasterVolumeLevelScalar(level / 100.0f, nullptr);
                        volume->Release();
                    }
                    device->Release();
                }
                enumerator->Release();
            }
            CoUninitialize();
            return {true, "Volume set to " + std::to_string(level) + "%"};
        }

        return {false, "Unknown tool: " + tool_name};
    }
};

} // anonymous namespace

extern "C" VISION_PLUGIN_API vision::IActionPlugin* create_plugin() {
    return new MediaPlugin();
}

extern "C" VISION_PLUGIN_API void destroy_plugin(vision::IActionPlugin* p) {
    delete p;
}
