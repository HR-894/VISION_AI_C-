#include <iostream>
#include <string>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "daemon/grpc_server.h"

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    // Initialize spdlog console logger
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    spdlog::set_default_logger(std::make_shared<spdlog::logger>("vision_daemon", console_sink));
    spdlog::set_level(spdlog::level::debug);

    spdlog::info("Initializing VISION_DAEMON...");
    
    // Hardcoded local port lock for strict execution constraints
    const std::string server_address = "127.0.0.1:53912";
    
    vision::daemon::DaemonServer server(server_address);
    server.Run();

    return 0;
}
