#include "daemon/grpc_server.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <spdlog/spdlog.h>

#include "voice/VoiceManager.h"
#include "automation/ui_automation.h"
#include "automation/window_manager.h"
#include "vision/screen_observer.h"

namespace vision {
namespace daemon {

SensoryMotorEngineImpl::SensoryMotorEngineImpl(
    std::shared_ptr<vision::voice::VoiceManager> voice_mgr,
    std::shared_ptr<vision::UIAutomation> ui_auto,
    std::shared_ptr<vision::WindowManager> win_mgr,
    std::shared_ptr<vision::ScreenObserver> screen_obs
) : m_is_running(true),
    m_voice_mgr(std::move(voice_mgr)),
    m_ui_auto(std::move(ui_auto)),
    m_win_mgr(std::move(win_mgr)),
    m_screen_obs(std::move(screen_obs)) 
{
}

SensoryMotorEngineImpl::~SensoryMotorEngineImpl() {
    m_is_running = false;
}

grpc::Status SensoryMotorEngineImpl::CheckHealth(grpc::ServerContext* context, const vision_daemon::Empty* request, vision_daemon::DaemonStatus* reply) {
    (void)context;
    (void)request;
    reply->set_is_alive(true);
    if (m_win_mgr) {
        reply->set_active_window_title(m_win_mgr->getActiveWindowTitle());
    } else {
        reply->set_active_window_title("Unknown");
    }
    reply->set_memory_usage_mb(0); // Can connect to core/doctor later
    return grpc::Status::OK;
}

grpc::Status SensoryMotorEngineImpl::ExecuteMouseKeyboard(grpc::ServerContext* context, const vision_daemon::ActionRequest* request, vision_daemon::ActionResult* reply) {
    (void)context;
    
    if (!m_win_mgr) {
        reply->set_success(false);
        reply->set_error_message("Window Manager not initialized");
        return grpc::Status::INTERNAL;
    }

    try {
        switch (request->action_type()) {
            case vision_daemon::ActionRequest::CLICK_LEFT:
            case vision_daemon::ActionRequest::CLICK_RIGHT:
                // TODO: UIAutomation COM Invoke Pattern mapping here
                spdlog::info("Click action received (x: {}, y: {}) - COM placeholder", request->x(), request->y());
                break;
                
            case vision_daemon::ActionRequest::SCROLL:
                m_win_mgr->scrollPage("down", 3);
                break;
                
            case vision_daemon::ActionRequest::TYPE_TEXT:
                m_win_mgr->typeText(request->text_payload());
                break;
                
            case vision_daemon::ActionRequest::KEY_COMBO:
                m_win_mgr->pressKey(request->text_payload());
                break;
                
            default:
                reply->set_success(false);
                reply->set_error_message("Unknown action type");
                return grpc::Status::INVALID_ARGUMENT;
        }
        
        reply->set_success(true);
        return grpc::Status::OK;
    } catch (const std::exception& e) {
        spdlog::error("Error executing motor command: {}", e.what());
        reply->set_success(false);
        reply->set_error_message(e.what());
        return grpc::Status::INTERNAL;
    }
}

grpc::Status SensoryMotorEngineImpl::ManageOSWindow(grpc::ServerContext* context, const vision_daemon::WindowRequest* request, vision_daemon::ActionResult* reply) {
    (void)context;
    
    if (!m_win_mgr) {
        reply->set_success(false);
        reply->set_error_message("Window Manager not initialized");
        return grpc::Status::INTERNAL;
    }

    bool success = false;
    std::string pattern = request->window_title_substring();
    
    switch (request->action()) {
        case vision_daemon::WindowRequest::FOCUS:
            success = m_win_mgr->focusWindow(pattern);
            break;
        case vision_daemon::WindowRequest::MINIMIZE:
            success = m_win_mgr->minimizeWindow(pattern);
            break;
        case vision_daemon::WindowRequest::MAXIMIZE:
            success = m_win_mgr->maximizeWindow(pattern);
            break;
        case vision_daemon::WindowRequest::CLOSE:
            success = m_win_mgr->closeWindow(pattern);
            break;
        default:
            reply->set_success(false);
            reply->set_error_message("Unknown window action");
            return grpc::Status::INVALID_ARGUMENT;
    }
    
    reply->set_success(success);
    if (!success) {
        reply->set_error_message("Failed to perform window action (window not found?)");
    }
    return grpc::Status::OK;
}

grpc::Status SensoryMotorEngineImpl::StreamVisuals(grpc::ServerContext* context, const vision_daemon::VisualRequest* request, grpc::ServerWriter<vision_daemon::ScreenFrame>* writer) {
    spdlog::info("Client connected to Visuals Stream. FPS cap: {}, Diff only: {}", request->fps_cap(), request->only_on_change());
    
    if (!m_screen_obs) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "ScreenObserver not initialized");
    }

    int fps = request->fps_cap() > 0 ? request->fps_cap() : 2;
    auto delay = std::chrono::milliseconds(1000 / fps);
    int quality = request->compression_quality() > 0 ? request->compression_quality() : 80;
    
    uint64_t last_phash = 0;

    while (!context->IsCancelled() && m_is_running) {
        auto snapshot = m_screen_obs->getLatest();
        
        bool should_send = true;
        if (request->only_on_change()) {
            if (snapshot.valid && snapshot.phash == last_phash) {
                should_send = false;
            }
        }
        
        if (should_send && snapshot.valid) {
            auto jpeg_bytes = m_screen_obs->getCurrentFrameJpeg(quality);
            
            vision_daemon::ScreenFrame frame;
            frame.set_timestamp(std::chrono::duration_cast<std::chrono::milliseconds>(snapshot.timestamp.time_since_epoch()).count());
            if (m_win_mgr) {
                frame.set_active_window(m_win_mgr->getActiveWindowTitle());
            }
            frame.set_jpeg_image_data(jpeg_bytes.data(), jpeg_bytes.size());
            
            if (!writer->Write(frame)) {
                break;
            }
            last_phash = snapshot.phash;
        }
        
        std::this_thread::sleep_for(delay);
    }
    
    spdlog::info("Client disconnected from Visuals Stream.");
    return grpc::Status::OK;
}

grpc::Status SensoryMotorEngineImpl::StreamMicrophone(grpc::ServerContext* context, const vision_daemon::Empty* request, grpc::ServerWriter<vision_daemon::AudioTranscript>* writer) {
    (void)request;
    spdlog::info("Client connected to Microphone Stream.");
    
    if (!m_voice_mgr) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "VoiceManager not initialized");
    }

    std::queue<vision_daemon::AudioTranscript> transcript_queue;
    std::mutex q_mutex;
    std::condition_variable q_cv;
    
    // Hook callbacks
    auto push_transcript = [&](const std::string& text, bool is_final) {
        vision_daemon::AudioTranscript t;
        t.set_timestamp(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        t.set_text(text);
        t.set_is_final(is_final);
        
        {
            std::lock_guard<std::mutex> lock(q_mutex);
            transcript_queue.push(std::move(t));
        }
        q_cv.notify_one();
    };

    m_voice_mgr->onPartialText = [&](const std::string& text) {
        if (!text.empty()) push_transcript(text, false);
    };
    
    m_voice_mgr->onFinalText = [&](const std::string& text) {
        if (!text.empty()) push_transcript(text, true);
    };

    m_voice_mgr->startListening();

    // Stream loop
    while (!context->IsCancelled() && m_is_running) {
        vision_daemon::AudioTranscript t;
        bool has_item = false;
        
        {
            std::unique_lock<std::mutex> lock(q_mutex);
            // Wait until queue is not empty, OR context cancelled, OR server shutting down
            q_cv.wait_for(lock, std::chrono::milliseconds(200), [&]() {
                return !transcript_queue.empty() || context->IsCancelled() || !m_is_running;
            });
            
            if (!transcript_queue.empty()) {
                t = std::move(transcript_queue.front());
                transcript_queue.pop();
                has_item = true;
            }
        }
        
        if (has_item) {
            if (!writer->Write(t)) {
                break;
            }
        }
    }

    m_voice_mgr->stopListening();
    
    // Unhook callbacks
    m_voice_mgr->onPartialText = nullptr;
    m_voice_mgr->onFinalText = nullptr;

    spdlog::info("Client disconnected from Microphone Stream.");
    return grpc::Status::OK;
}

DaemonServer::DaemonServer(
    const std::string& address,
    std::shared_ptr<vision::voice::VoiceManager> voice_mgr,
    std::shared_ptr<vision::UIAutomation> ui_auto,
    std::shared_ptr<vision::WindowManager> win_mgr,
    std::shared_ptr<vision::ScreenObserver> screen_obs
) : m_server_address(address),
    m_service(std::move(voice_mgr), std::move(ui_auto), std::move(win_mgr), std::move(screen_obs)) 
{
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
