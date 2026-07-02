#include "daemon/grpc_server.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <spdlog/spdlog.h>
// Include internal hooks (to be connected)
#include "automation/ui_automation.h"
#include "automation/window_manager.h"
#include "automation/system_commands.h"
#include "vision/screen_observer.h"

namespace vision {
namespace daemon {

SensoryMotorEngineImpl::SensoryMotorEngineImpl() : m_is_running(true) {
}

SensoryMotorEngineImpl::~SensoryMotorEngineImpl() {
    m_is_running = false;
}

grpc::Status SensoryMotorEngineImpl::CheckHealth(grpc::ServerContext* context, const vision_daemon::Empty* request, vision_daemon::DaemonStatus* reply) {
    (void)context;
    (void)request;
    reply->set_is_alive(true);
    reply->set_active_window_title("VisionDaemon"); // To be hooked to WindowManager
    reply->set_memory_usage_mb(0); // Placeholder
    return grpc::Status::OK;
}

grpc::Status SensoryMotorEngineImpl::ExecuteMouseKeyboard(grpc::ServerContext* context, const vision_daemon::ActionRequest* request, vision_daemon::ActionResult* reply) {
    (void)context;
    spdlog::info("Executing Motor Command: type={}", static_cast<int>(request->action_type()));
    // TODO: Connect to Win32 SendInput / UI Automation
    reply->set_success(true);
    reply->set_error_message("");
    return grpc::Status::OK;
}

grpc::Status SensoryMotorEngineImpl::ManageOSWindow(grpc::ServerContext* context, const vision_daemon::WindowRequest* request, vision_daemon::ActionResult* reply) {
    (void)context;
    spdlog::info("Managing OS Window: type={}", static_cast<int>(request->action()));
    // TODO: Connect to WindowManager
    reply->set_success(true);
    reply->set_error_message("");
    return grpc::Status::OK;
}

grpc::Status SensoryMotorEngineImpl::StreamVisuals(grpc::ServerContext* context, const vision_daemon::VisualRequest* request, grpc::ServerWriter<vision_daemon::ScreenFrame>* writer) {
    spdlog::info("Client connected to Visuals Stream. FPS cap: {}, Diff only: {}", request->fps_cap(), request->only_on_change());
    
    int fps = request->fps_cap() > 0 ? request->fps_cap() : 2;
    auto delay = std::chrono::milliseconds(1000 / fps);

    // Dedicated C++ thread logic execution context for streaming
    while (!context->IsCancelled() && m_is_running) {
        vision_daemon::ScreenFrame frame;
        frame.set_timestamp(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        frame.set_active_window("PlaceholderActiveWindow");
        // TODO: Read DXGI duplicate output and compress to JPEG here
        
        if (!writer->Write(frame)) {
            // Broken stream
            break;
        }
        std::this_thread::sleep_for(delay);
    }
    
    spdlog::info("Client disconnected from Visuals Stream.");
    return grpc::Status::OK;
}

grpc::Status SensoryMotorEngineImpl::StreamMicrophone(grpc::ServerContext* context, const vision_daemon::Empty* request, grpc::ServerWriter<vision_daemon::AudioTranscript>* writer) {
    (void)request;
    spdlog::info("Client connected to Microphone Stream.");

    // Dedicated C++ thread logic execution context for transcription stream
    while (!context->IsCancelled() && m_is_running) {
        // TODO: Connect to VoiceManager & SPSC Ring Buffer to stream whisper output
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    spdlog::info("Client disconnected from Microphone Stream.");
    return grpc::Status::OK;
}

DaemonServer::DaemonServer(const std::string& address) 
    : m_server_address(address) {
}

DaemonServer::~DaemonServer() {
    Stop();
}

void DaemonServer::Run() {
    grpc::ServerBuilder builder;
    builder.AddListeningPort(m_server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&m_service);
    
    m_server = builder.BuildAndStart();
    spdlog::info("VISION_DAEMON Headless Engine Listening on {}", m_server_address);
    
    m_server->Wait();
}

void DaemonServer::Stop() {
    if (m_server) {
        m_server->Shutdown();
    }
}

} // namespace daemon
} // namespace vision
