#pragma once

#include <memory>
#include <string>
#include <thread>
#include <atomic>
#include <grpcpp/grpcpp.h>
#include "vision_daemon.grpc.pb.h"
#include "vision_daemon.pb.h"

namespace vision {
namespace daemon {

class SensoryMotorEngineImpl final : public vision_daemon::SensoryMotorEngine::Service {
public:
    SensoryMotorEngineImpl();
    ~SensoryMotorEngineImpl() override;

    grpc::Status CheckHealth(grpc::ServerContext* context, const vision_daemon::Empty* request, vision_daemon::DaemonStatus* reply) override;
    
    grpc::Status ExecuteMouseKeyboard(grpc::ServerContext* context, const vision_daemon::ActionRequest* request, vision_daemon::ActionResult* reply) override;
    
    grpc::Status ManageOSWindow(grpc::ServerContext* context, const vision_daemon::WindowRequest* request, vision_daemon::ActionResult* reply) override;

    grpc::Status StreamVisuals(grpc::ServerContext* context, const vision_daemon::VisualRequest* request, grpc::ServerWriter<vision_daemon::ScreenFrame>* writer) override;
    
    grpc::Status StreamMicrophone(grpc::ServerContext* context, const vision_daemon::Empty* request, grpc::ServerWriter<vision_daemon::AudioTranscript>* writer) override;

private:
    std::atomic<bool> m_is_running;
};

class DaemonServer {
public:
    DaemonServer(const std::string& address);
    ~DaemonServer();

    void Run();
    void Stop();

private:
    std::string m_server_address;
    std::unique_ptr<grpc::Server> m_server;
    SensoryMotorEngineImpl m_service;
};

} // namespace daemon
} // namespace vision
