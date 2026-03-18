/**
 * @file main.cpp
 * @brief VISION AI application entry point
 */

#include "vision_ai.h"
#include "gpu_setup_wizard.h"
#include "model_downloader_wizard.h"
#include <QApplication>
#include <QStyleFactory>
#include <QFile>
#include <QIcon>

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

    // ── Application Icon ─────────────────────────────────────────
    // Load from assets/icon.png next to the executable, or from project root.
    // This icon shows in taskbar, title bar, system tray, and Alt+Tab.
    {
        QString iconPath = QCoreApplication::applicationDirPath() + "/assets/icon.png";
        if (!QFile::exists(iconPath)) {
            // Fallback: look relative to working directory (dev mode)
            iconPath = "assets/icon.png";
        }
        if (QFile::exists(iconPath)) {
            app.setWindowIcon(QIcon(iconPath));
        }
    }

    // Force dark style
    app.setStyle(QStyleFactory::create("Fusion"));
    
    // Hacker Dark palette (Matrix Black + Neon Green)
    QPalette dark_palette;
    dark_palette.setColor(QPalette::Window, QColor(10, 10, 10));       // #0A0A0A
    dark_palette.setColor(QPalette::WindowText, QColor(224, 224, 224)); // #E0E0E0
    dark_palette.setColor(QPalette::Base, QColor(17, 17, 17));         // #111111
    dark_palette.setColor(QPalette::AlternateBase, QColor(26, 26, 26)); // #1A1A1A
    dark_palette.setColor(QPalette::ToolTipBase, QColor(17, 17, 17));
    dark_palette.setColor(QPalette::ToolTipText, QColor(224, 224, 224));
    dark_palette.setColor(QPalette::Text, QColor(224, 224, 224));
    dark_palette.setColor(QPalette::Button, QColor(17, 17, 17));
    dark_palette.setColor(QPalette::ButtonText, QColor(224, 224, 224));
    dark_palette.setColor(QPalette::BrightText, QColor(57, 255, 20));   // #39FF14
    dark_palette.setColor(QPalette::Link, QColor(57, 255, 20));
    dark_palette.setColor(QPalette::Highlight, QColor(57, 255, 20));    // #39FF14
    dark_palette.setColor(QPalette::HighlightedText, QColor(0, 0, 0));
    dark_palette.setColor(QPalette::Disabled, QPalette::Text, QColor(80, 80, 80));
    dark_palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(80, 80, 80));
    app.setPalette(dark_palette);
    
    // Global stylesheet
    app.setStyleSheet(R"(
        QToolTip { color: #E0E0E0; background-color: #111111; border: 1px solid #39FF14; padding: 4px; }
        QScrollBar:vertical { background: #0A0A0A; width: 10px; margin: 0; }
        QScrollBar::handle:vertical { background: #39FF14; min-height: 20px; border-radius: 5px; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
        QScrollBar:horizontal { background: #0A0A0A; height: 10px; margin: 0; }
        QScrollBar::handle:horizontal { background: #39FF14; min-width: 20px; border-radius: 5px; }
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
