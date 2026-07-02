#include <iostream>
#include <string>
#include <memory>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "daemon/grpc_server.h"
#include "voice/VoiceManager.h"
#include "automation/ui_automation.h"
#include "automation/window_manager.h"
#include "vision/screen_observer.h"

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    // Initialize spdlog console logger
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    spdlog::set_default_logger(std::make_shared<spdlog::logger>("vision_daemon", console_sink));
    spdlog::set_level(spdlog::level::debug);

    spdlog::info("Initializing VISION_DAEMON Subsystems...");
    
    // 1. Initialize Subsystems
    auto voice_mgr = std::make_shared<vision::voice::VoiceManager>();
    if (!voice_mgr->initialize("models/ggml-base.bin")) {
        spdlog::warn("Failed to initialize VoiceManager (missing model?)");
    }
    
    auto ui_auto = std::make_shared<vision::UIAutomation>();
    
    auto win_mgr = std::make_shared<vision::WindowManager>();
    
    auto screen_obs = std::make_shared<vision::ScreenObserver>();
    // Start background screen observer at 2 FPS (500ms interval)
    // We pass nullptr for OCR callback to save CPU, as we only need JPEGs for the stream
    screen_obs->start(nullptr, 500); 

    // Hardcoded local port lock for strict execution constraints
    const std::string server_address = "127.0.0.1:53912";
    
    // 2. Inject Subsystems into Daemon Server
    vision::daemon::DaemonServer server(server_address, voice_mgr, ui_auto, win_mgr, screen_obs);
    
    // 3. Run
    server.Run();

    // 4. Cleanup
    screen_obs->stop();
    voice_mgr->shutdown();

    return 0;
}
