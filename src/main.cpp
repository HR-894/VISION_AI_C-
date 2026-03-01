/**
 * @file main.cpp
 * @brief VISION AI application entry point
 */

#include "vision_ai.h"
#include "gpu_setup_wizard.h"
#include "model_downloader_wizard.h"
#include <QApplication>
#include <QStyleFactory>

#ifdef VISION_HAS_SPDLOG
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#endif

#include <windows.h>
#include <filesystem>

static void setupLogging() {
#ifdef VISION_HAS_SPDLOG
    try {
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            "data/vision_ai.log", 1024 * 1024 * 5, 3);
        
        auto logger = std::make_shared<spdlog::logger>(
            "VISION_AI",
            spdlog::sinks_init_list{console_sink, file_sink});
        
        logger->set_level(spdlog::level::info);
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%n] %v");
        spdlog::set_default_logger(logger);
        spdlog::info("VISION AI v{} starting...", VISION_VERSION);
    } catch (const spdlog::spdlog_ex& ex) {
        // Fallback: just continue without file logging
    }
#endif
}

int WINAPI WinMain(HINSTANCE /*hInstance*/, HINSTANCE /*hPrevInstance*/,
                   LPSTR /*lpCmdLine*/, int /*nCmdShow*/) {
    // Initialize COM for Windows APIs (audio, WMI, etc.)
    // COM: Let Qt manage COM on the GUI thread (apartment-threaded).
    // Worker threads that need WMI/COM should call CoInitializeEx themselves.
    
    // Qt application
    int argc = 0;
    QApplication app(argc, nullptr);
    app.setApplicationName("VISION AI");
    app.setApplicationVersion(VISION_VERSION);
    app.setOrganizationName("VISION");
    
    // Force dark style
    app.setStyle(QStyleFactory::create("Fusion"));
    
    // Dark palette
    QPalette dark_palette;
    dark_palette.setColor(QPalette::Window, QColor(30, 30, 30));
    dark_palette.setColor(QPalette::WindowText, Qt::white);
    dark_palette.setColor(QPalette::Base, QColor(20, 20, 20));
    dark_palette.setColor(QPalette::AlternateBase, QColor(40, 40, 40));
    dark_palette.setColor(QPalette::ToolTipBase, QColor(50, 50, 50));
    dark_palette.setColor(QPalette::ToolTipText, Qt::white);
    dark_palette.setColor(QPalette::Text, Qt::white);
    dark_palette.setColor(QPalette::Button, QColor(45, 45, 45));
    dark_palette.setColor(QPalette::ButtonText, Qt::white);
    dark_palette.setColor(QPalette::BrightText, Qt::red);
    dark_palette.setColor(QPalette::Link, QColor(80, 160, 255));
    dark_palette.setColor(QPalette::Highlight, QColor(80, 120, 200));
    dark_palette.setColor(QPalette::HighlightedText, Qt::white);
    dark_palette.setColor(QPalette::Disabled, QPalette::Text, QColor(128, 128, 128));
    dark_palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(128, 128, 128));
    app.setPalette(dark_palette);
    
    // Global stylesheet
    app.setStyleSheet(R"(
        QToolTip { color: #ffffff; background-color: #2d2d2d; border: 1px solid #555; padding: 4px; }
        QScrollBar:vertical { background: #1e1e1e; width: 10px; margin: 0; }
        QScrollBar::handle:vertical { background: #555; min-height: 20px; border-radius: 5px; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
        QScrollBar:horizontal { background: #1e1e1e; height: 10px; margin: 0; }
        QScrollBar::handle:horizontal { background: #555; min-width: 20px; border-radius: 5px; }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }
    )");
    
    // Ensure data directory exists (for logs, config, etc.)
    std::filesystem::create_directories("data");
    
    setupLogging();

    // ── GPU Setup Wizard (first-run only) ──────────────────────
    vision::GpuConfig gpu_cfg = vision::runGpuSetupIfNeeded(nullptr);

    // ── Model Download Wizard (if no model configured) ────────
    vision::DeviceProfiler startup_profiler;
    vision::ConfigManager  startup_config;
    startup_config.load();
    vision::runModelDownloadIfNeeded(startup_profiler, startup_config, nullptr);

    vision::VisionAI window;
    window.show();

    int result = app.exec();
    
    // COM cleanup removed — Qt manages COM lifecycle on GUI thread
    return result;
}
