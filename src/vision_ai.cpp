/**
 * @file vision_ai.cpp
 * @brief Main application class — Qt6 GUI, command pipeline, system tray, hotkeys
 */

#include "vision_ai.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QPushButton>
#include <QMenu>
#include <QAction>
#include <QCloseEvent>
#include <QMessageBox>
#include <QScrollBar>
#include <QApplication>
#include <QDesktopServices>
#include <QUrl>
#include <QFont>
#include <QIcon>
#include <QShortcut>
#include <QKeySequence>

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <thread>
#include <chrono>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <iomanip>

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

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace vision {

// Forward declaration of file-local helper
static std::string getHelpTextStatic();

// ═══════════════════ Static Data Maps ═══════════════════

const std::unordered_map<std::string, std::string> VisionAI::APP_SHORTCUTS = {
    {"notepad", "notepad.exe"}, {"calculator", "calc.exe"}, {"calc", "calc.exe"},
    {"paint", "mspaint.exe"}, {"explorer", "explorer.exe"}, {"file explorer", "explorer.exe"},
    {"settings", "ms-settings:"}, {"cmd", "cmd.exe"}, {"command prompt", "cmd.exe"},
    {"powershell", "powershell.exe"}, {"terminal", "wt.exe"}, {"windows terminal", "wt.exe"},
    {"task manager", "taskmgr.exe"}, {"device manager", "devmgmt.msc"},
    {"control panel", "control"}, {"disk cleanup", "cleanmgr.exe"},
    {"snipping tool", "SnippingTool.exe"}, {"snip", "SnippingTool.exe"},
    {"magnifier", "magnify.exe"}, {"wordpad", "wordpad.exe"},
    {"edge", "msedge.exe"}, {"microsoft edge", "msedge.exe"},
    {"chrome", "chrome.exe"}, {"google chrome", "chrome.exe"},
    {"firefox", "firefox.exe"}, {"brave", "brave.exe"},
    {"discord", "discord.exe"}, {"spotify", "spotify.exe"},
    {"steam", "steam.exe"}, {"code", "code.exe"}, {"vs code", "code.exe"},
    {"visual studio code", "code.exe"}, {"word", "winword.exe"},
    {"excel", "excel.exe"}, {"powerpoint", "powerpnt.exe"},
    {"outlook", "outlook.exe"}, {"teams", "ms-teams.exe"},
    {"onenote", "onenote.exe"}, {"vlc", "vlc.exe"},
};

// Microsoft Store / UWP apps — use protocol URIs
const std::unordered_map<std::string, std::string> VisionAI::STORE_APPS = {
    {"calculator", "calculator:"},
    {"clock", "ms-clock:"},
    {"alarms", "ms-clock:"},
    {"photos", "ms-photos:"},
    {"store", "ms-windows-store:"},
    {"microsoft store", "ms-windows-store:"},
    {"maps", "bingmaps:"},
    {"weather", "bingweather:"},
    {"mail", "outlookmail:"},
    {"calendar", "outlookcal:"},
    {"camera", "microsoft.windows.camera:"},
    {"xbox", "xbox:"},
    {"feedback hub", "feedback-hub:"},
    {"tips", "ms-get-started:"},
    {"your phone", "ms-phone:"},
    {"phone link", "ms-phone:"},
    {"sticky notes", "ms-stickynotes:"},
    {"whiteboard", "ms-whiteboard-cmd:"},
    {"screen sketch", "ms-ScreenSketch:"},
    {"screen snip", "ms-ScreenClip:"},
    {"movies", "mswindowsvideo:"},
    {"groove", "mswindowsmusic:"},
    {"music", "mswindowsmusic:"},
    {"whatsapp", "whatsapp:"},
    {"telegram", "tg:"},
    {"linkedin", "linkedin:"},
};

const std::unordered_map<std::string, std::string> VisionAI::URL_SHORTCUTS = {
    {"youtube", "https://youtube.com"}, {"yt", "https://youtube.com"},
    {"gmail", "https://mail.google.com"}, {"google", "https://google.com"},
    {"github", "https://github.com"}, {"reddit", "https://reddit.com"},
    {"twitter", "https://twitter.com"}, {"facebook", "https://facebook.com"},
    {"instagram", "https://instagram.com"},
    {"wikipedia", "https://wikipedia.org"}, {"amazon", "https://amazon.com"},
    {"netflix", "https://netflix.com"}, {"twitch", "https://twitch.tv"},
    {"chatgpt", "https://chat.openai.com"},
    {"stackoverflow", "https://stackoverflow.com"},
};

// ═══════════════════ Constructor / Destructor ═══════════════════

VisionAI::VisionAI(QWidget* parent)
    : QMainWindow(parent)
    , safety_()
    , file_mgr_(safety_)
    , template_matcher_()
    , fast_handler_(*this)
    , router_(template_matcher_, fast_handler_) {
    
    // Load config
    config_.load();
    
    // Setup UI
    setupUI();
    setupTrayIcon();
    setupHotkey();
    
    // Auto-configure based on hardware
    auto recommended = profiler_.getRecommendedConfig();
    LOG_INFO("Device: {}", profiler_.getStatusString());
    
    // Load AI models in background (stored thread for safe shutdown)
    model_load_thread_ = std::thread([this]() { loadModels(); });
    
    // Start system stats timer
    stats_timer_ = new QTimer(this);
    connect(stats_timer_, &QTimer::timeout, this, &VisionAI::updateSystemStats);
    stats_timer_->start(5000);
    
    // Thread-safe message passing
    connect(this, &VisionAI::messageReady, this, &VisionAI::appendMessage);
    connect(this, &VisionAI::statusReady, this, &VisionAI::setStatusText);
    
    updateStatus("Ready | " + profiler_.getStatusString());
    addMessage("VISION", "Hello! I'm VISION AI (C++). Type a command or press Ctrl+Alt+Space to use voice.");
}

VisionAI::~VisionAI() {
    // Wait for model loading thread to finish
    if (model_load_thread_.joinable()) {
        model_load_thread_.join();
    }
    // Wait for command processing thread to finish
    if (cmd_thread_.joinable()) {
        cmd_thread_.join();
    }
#ifdef VISION_HAS_WHISPER
    // Wait for audio processing thread to finish
    if (audio_thread_.joinable()) {
        audio_thread_.join();
    }
#endif
    if (hotkey_registered_) {
        UnregisterHotKey((HWND)winId(), hotkey_id_);
    }
}

// ═══════════════════ UI Setup ═══════════════════

void VisionAI::setupUI() {
    setWindowTitle("VISION AI");
    setMinimumSize(700, 500);
    resize(800, 600);
    
    auto* central = new QWidget(this);
    setCentralWidget(central);
    auto* main_layout = new QVBoxLayout(central);
    main_layout->setContentsMargins(12, 12, 12, 12);
    main_layout->setSpacing(8);
    
    // ── Header ───────────────────────────────────────────────────
    auto* header = new QHBoxLayout();
    auto* title = new QLabel("🔮 VISION AI");
    title->setStyleSheet("font-size: 22px; font-weight: bold; color: #7B68EE;");
    
    cpu_label_ = new QLabel("CPU: --");
    cpu_label_->setStyleSheet("color: #888; font-size: 11px;");
    ram_label_ = new QLabel("RAM: --");
    ram_label_->setStyleSheet("color: #888; font-size: 11px;");
    
    auto* settings_btn = new QPushButton("⚙");
    settings_btn->setFixedSize(32, 32);
    settings_btn->setStyleSheet("font-size: 16px; border: 1px solid #555; border-radius: 6px;");
    connect(settings_btn, &QPushButton::clicked, this, &VisionAI::onSettingsClicked);
    
    auto* help_btn = new QPushButton("?");
    help_btn->setFixedSize(32, 32);
    help_btn->setStyleSheet("font-size: 16px; border: 1px solid #555; border-radius: 6px;");
    connect(help_btn, &QPushButton::clicked, this, &VisionAI::onHelpClicked);
    
    header->addWidget(title);
    header->addStretch();
    header->addWidget(cpu_label_);
    header->addWidget(ram_label_);
    header->addWidget(settings_btn);
    header->addWidget(help_btn);
    main_layout->addLayout(header);
    
    // ── Chat display ─────────────────────────────────────────────
    chat_display_ = new QTextEdit();
    chat_display_->setReadOnly(true);
    chat_display_->setStyleSheet(
        "background-color: #1a1a1a; border: 1px solid #333; border-radius: 8px; "
        "padding: 10px; font-family: 'Consolas', 'Courier New', monospace; font-size: 13px;");
    main_layout->addWidget(chat_display_, 1);
    
    // ── Quick presets ────────────────────────────────────────────
    auto* presets = new QHBoxLayout();
    const std::vector<std::pair<std::string, std::string>> preset_items = {
        {"📸 Screenshot", "take screenshot"},
        {"🔋 Battery", "battery"},
        {"📂 Downloads", "list downloads"},
        {"🔍 Health", "health scan"},
        {"🎯 Focus", "focus mode 25"},
    };
    for (const auto& [label, cmd] : preset_items) {
        auto* btn = new QPushButton(QString::fromStdString(label));
        btn->setStyleSheet(
            "background: #2a2a2a; border: 1px solid #444; border-radius: 6px; "
            "padding: 4px 10px; font-size: 11px; color: #ccc;");
        connect(btn, &QPushButton::clicked, [this, c = cmd]() {
            onPresetClicked(QString::fromStdString(c));
        });
        presets->addWidget(btn);
    }
    main_layout->addLayout(presets);
    
    // ── Input area ───────────────────────────────────────────────
    auto* input_layout = new QHBoxLayout();
    
    input_field_ = new QLineEdit();
    input_field_->setPlaceholderText("Type a command... (Enter to send, Ctrl+Alt+Space for voice)");
    input_field_->setStyleSheet(
        "background: #2a2a2a; border: 1px solid #555; border-radius: 8px; "
        "padding: 8px 14px; font-size: 14px; color: white;");
    connect(input_field_, &QLineEdit::returnPressed, this, &VisionAI::onSendCommand);
    
    auto* send_btn = new QPushButton("▶");
    send_btn->setFixedSize(40, 36);
    send_btn->setStyleSheet(
        "background: #7B68EE; border: none; border-radius: 8px; "
        "font-size: 16px; color: white;");
    connect(send_btn, &QPushButton::clicked, this, &VisionAI::onSendCommand);
    
    input_layout->addWidget(input_field_, 1);
    input_layout->addWidget(send_btn);
    main_layout->addLayout(input_layout);
    
    // ── Status bar ───────────────────────────────────────────────
    status_label_ = new QLabel("Ready");
    status_label_->setStyleSheet("color: #666; font-size: 11px; padding: 4px;");
    main_layout->addWidget(status_label_);
    
    // ── Keyboard shortcuts ───────────────────────────────────────
    auto* shortcut_clear = new QShortcut(QKeySequence("Ctrl+L"), this);
    connect(shortcut_clear, &QShortcut::activated, this, &VisionAI::onClearChat);
    
    auto* shortcut_copy = new QShortcut(QKeySequence("Ctrl+Shift+C"), this);
    connect(shortcut_copy, &QShortcut::activated, this, &VisionAI::onCopyLast);
}

// ═══════════════════ System Tray ═══════════════════

void VisionAI::setupTrayIcon() {
    tray_icon_ = new QSystemTrayIcon(this);
    tray_icon_->setToolTip("VISION AI — Ready");
    // Use the application window icon, or fall back to a Qt built-in
    QIcon appIcon = windowIcon();
    if (appIcon.isNull())
        appIcon = qApp->style()->standardIcon(QStyle::SP_ComputerIcon);
    tray_icon_->setIcon(appIcon);
    setWindowIcon(appIcon);  // Also set on the main window
    
    auto* tray_menu = new QMenu(this);
    tray_menu->addAction("Show", this, &VisionAI::showFromTray);
    tray_menu->addAction("Help", this, &VisionAI::showHelpDialog);
    tray_menu->addSeparator();
    tray_menu->addAction("Quit", qApp, &QApplication::quit);
    
    tray_icon_->setContextMenu(tray_menu);
    connect(tray_icon_, &QSystemTrayIcon::activated, this, &VisionAI::onTrayActivated);
    tray_icon_->show();
}


void VisionAI::setupHotkey() {
    // Ctrl+Alt+Space — voice toggle
    hotkey_registered_ = RegisterHotKey((HWND)winId(), hotkey_id_,
                                         MOD_CONTROL | MOD_ALT, VK_SPACE);
    if (hotkey_registered_) {
        LOG_INFO("Global hotkey registered: Ctrl+Alt+Space");
    } else {
        LOG_ERROR("Failed to register hotkey Ctrl+Alt+Space (err={})", GetLastError());
    }

    // Ctrl+Esc — Emergency Stop (kill switch for runaway generation)
    kill_switch_registered_ = RegisterHotKey((HWND)winId(), kill_switch_id_,
                                              MOD_CONTROL, VK_ESCAPE);
    if (kill_switch_registered_) {
        LOG_INFO("Kill switch registered: Ctrl+Esc");
    } else {
        LOG_WARN("Failed to register kill switch Ctrl+Esc (err={})", GetLastError());
    }
}

// ═══════════════════ Model Loading ═══════════════════

void VisionAI::loadModels() {
    emit statusReady("Loading AI models...", "#FFD700");
    
#ifdef VISION_HAS_WHISPER
    auto rec = profiler_.getRecommendedConfig();
    std::string whisper_model = rec.value("whisper_model", "base");
    whisper_engine_ = std::make_unique<WhisperEngine>(whisper_model);
    audio_capture_ = std::make_unique<AudioCapture>();
    
    if (whisper_engine_->loadModel()) {
        LOG_INFO("Whisper loaded: {}", whisper_engine_->getModelInfo());
    }
#endif
    
#ifdef VISION_HAS_LLM
    llm_controller_ = std::make_unique<LLMController>();

    // Apply dynamic context size and thread count from device profiler
    auto rec2 = profiler_.getRecommendedConfig();
    int ctx_size = rec2.value("context_size", 2048);
    int thread_count = rec2.value("thread_count", 2);
    llm_controller_->setContextSize(ctx_size);
    llm_controller_->setThreadCount(thread_count);
    LOG_INFO("LLM config: context={}, threads={}", ctx_size, thread_count);

    // Auto-set GPU layers: offload everything to GPU when Vulkan is available
#if defined(VISION_GPU_VULKAN) || defined(VISION_GPU_CUDA)
    llm_controller_->setGPULayers(99);
    LOG_INFO("GPU acceleration: offloading all layers to GPU");
#else
    llm_controller_->setGPULayers(rec2.value("gpu_layers", 0));
#endif
    
    if (llm_controller_->loadModel()) {
        LOG_INFO("LLM loaded: {}", llm_controller_->getModelInfo());
    }
    
    // Create ActionExecutor and ReActAgent under LLM guard (not OCR)
    action_executor_ = std::make_unique<ActionExecutor>(*this);
    react_agent_ = std::make_unique<ReActAgent>(
        *llm_controller_, *action_executor_, window_mgr_, *this);
    router_.setReActAgent(react_agent_.get());
    LOG_INFO("AI ReAct agent initialized");

    // ── Idle Timer: check every 60s, auto-unload LLM after 5min idle ──
    idle_timer_ = new QTimer(this);
    connect(idle_timer_, &QTimer::timeout, this, [this]() {
#ifdef VISION_HAS_LLM
        if (llm_controller_) llm_controller_->checkIdleUnload();
#endif
    });
    idle_timer_->start(60000);  // Check every 60 seconds
#endif

    emit statusReady("Ready | " + QString::fromStdString(profiler_.getStatusString()), "");
}

// ═══════════════════ Command Processing ═══════════════════

void VisionAI::onSendCommand() {
    QString text = input_field_->text().trimmed();
    if (text.isEmpty()) return;
    
    std::string command = text.toStdString();
    input_field_->clear();
    
    // Save to history (protected by mutex)
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        command_history_.push_front(command);
        if (command_history_.size() > 50) command_history_.pop_back();
        history_index_ = -1;
    }
    
    addMessage("You", command);
    
    // Track behavior
    behavior_.recordCommand(command, context_mgr_.getActiveApp().name);
    
    // Process in background thread (join previous if still running)
    if (cmd_thread_.joinable()) cmd_thread_.join();
    cmd_thread_ = std::thread([this, command]() {
        emit statusReady("Processing...", "#FFD700");
        
        // 1. Try fast complex handler FIRST ("open X and type/search Y")
        //    Must come before single-template match, otherwise "open_app"
        //    greedily captures the entire command including "and type Y".
        auto fast_result = fast_handler_.tryHandle(command);
        if (fast_result) {
            auto& [success, msg] = *fast_result;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                last_response_ = msg;
            }
            emit messageReady("VISION", QString::fromStdString(msg));
            emit statusReady("Ready", "");
            context_mgr_.recordCommand(command, msg);
            agent_memory_.recordTask(command, msg, success, {"fast_complex"});
            return;
        }
        
        // 2. Try chained commands ("open notepad then take screenshot")
        auto chained = template_matcher_.matchChained(command);
        if (chained.size() > 1) {
            std::string combined_result;
            for (const auto& match : chained) {
                auto result = instantExecute(match.template_name + " " +
                    [&]() {
                        std::string vars;
                        for (auto& [k, v] : match.variables) vars += v + " ";
                        return vars;
                    }());
                combined_result += result + "\n";
                // Small delay between chained commands
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            emit messageReady("VISION", QString::fromStdString(combined_result));
            emit statusReady("Ready", "");
            return;
        }
        
        // 3. Try single template match (simple commands)
        auto match = template_matcher_.match(command);
        if (match) {
            auto result = instantExecute(command);
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                last_response_ = result;
            }
            emit messageReady("VISION", QString::fromStdString(result));
            emit statusReady("Ready", "");
            
            // Record context
            context_mgr_.recordCommand(command, result);
            agent_memory_.recordTask(command, result, true, {match->template_name});
            return;
        }
        
        // 4. Route to agent (if available)
        auto [success, result] = router_.route(command);
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            last_response_ = result;
        }
        emit messageReady("VISION", QString::fromStdString(result));
        emit statusReady("Ready", "");
        context_mgr_.recordCommand(command, result);
        agent_memory_.recordTask(command, result, success, {"agent"});
        
    });
}

std::string VisionAI::instantExecute(const std::string& text) {
    auto match = template_matcher_.match(text);
    if (!match) return "Command not recognized: " + text;
    
    const auto& name = match->template_name;
    const auto& vars = match->variables;
    
    // ── App control ──────────────────────────────────────────────
    if (name == "open_app") {
        std::string app = vars.count("app_name") ? vars.at("app_name") : "";
        if (openApp(app)) return "Opened " + app;
        return "Couldn't find app: " + app;
    }
    if (name == "close_app") {
        std::string app = vars.count("app_name") ? vars.at("app_name") : "";
        return window_mgr_.closeWindow(app) ? "Closed " + app : "Window not found: " + app;
    }
    if (name == "switch_to") {
        std::string app = vars.count("app_name") ? vars.at("app_name") : "";
        return window_mgr_.focusWindow(app) ? "Switched to " + app : "Window not found: " + app;
    }
    
    // ── Window ───────────────────────────────────────────────────
    if (name == "minimize_window") {
        std::string w = vars.count("window") ? vars.at("window") : "";
        return window_mgr_.minimizeWindow(w) ? "Minimized " + w : "Not found: " + w;
    }
    if (name == "maximize_window") {
        std::string w = vars.count("window") ? vars.at("window") : "";
        return window_mgr_.maximizeWindow(w) ? "Maximized " + w : "Not found: " + w;
    }
    if (name == "minimize_all") { window_mgr_.showDesktop(); return "Desktop shown"; }
    if (name == "snap_left") { window_mgr_.snapWindowLeft(); return "Snapped left"; }
    if (name == "snap_right") { window_mgr_.snapWindowRight(); return "Snapped right"; }
    if (name == "tile_windows") { window_mgr_.tileAllWindows(); return "Windows tiled"; }
    
    // ── Volume ───────────────────────────────────────────────────
    if (name == "set_volume") {
        int lvl = 50; // default
        try { lvl = std::stoi(vars.at("level")); } catch (...) {}
        window_mgr_.setVolume(lvl);
        return "Volume set to " + std::to_string(lvl) + "%";
    }
    if (name == "volume_up") { window_mgr_.volumeUp(); return "Volume increased"; }
    if (name == "volume_down") { window_mgr_.volumeDown(); return "Volume decreased"; }
    if (name == "mute") { window_mgr_.muteToggle(); return "Mute toggled"; }
    
    // ── Brightness ───────────────────────────────────────────────
    if (name == "set_brightness") {
        int lvl = 50; // default
        try { lvl = std::stoi(vars.at("level")); } catch (...) {}
        auto [ok, msg] = sys_cmds_.setBrightness(lvl);
        return msg;
    }
    if (name == "brightness_up") { auto [ok, msg] = sys_cmds_.brightnessUp(); return msg; }
    if (name == "brightness_down") { auto [ok, msg] = sys_cmds_.brightnessDown(); return msg; }
    
    // ── System info ──────────────────────────────────────────────
    if (name == "battery") {
        auto info = sys_cmds_.getBatteryInfo();
        int pct = info.value("percent", -1);
        std::string status = info.value("ac_status", "unknown");
        return "Battery: " + std::to_string(pct) + "% (" + status + ")";
    }
    if (name == "storage") {
        auto drives = sys_cmds_.getStorageInfo();
        std::ostringstream ss; ss << "Storage:\n";
        for (auto& d : drives) {
            ss << "  " << d["drive"].get<std::string>() << " — "
               << std::fixed << std::setprecision(1)
               << d["free_gb"].get<double>() << " GB free / "
               << d["total_gb"].get<double>() << " GB total ("
               << (int)d["used_percent"].get<double>() << "% used)\n";
        }
        return ss.str();
    }
    if (name == "uptime") { return "Uptime: " + sys_cmds_.getUptime(); }
    if (name == "system_info") { return sys_cmds_.getSystemSummary().dump(2); }
    
    // ── Network ──────────────────────────────────────────────────
    if (name == "wifi_on") { auto [ok, msg] = sys_cmds_.toggleWifi(true); return msg; }
    if (name == "wifi_off") { auto [ok, msg] = sys_cmds_.toggleWifi(false); return msg; }
    if (name == "bluetooth_on") { auto [ok, msg] = sys_cmds_.toggleBluetooth(true); return msg; }
    if (name == "bluetooth_off") { auto [ok, msg] = sys_cmds_.toggleBluetooth(false); return msg; }
    if (name == "ip_address") { return "IP: " + sys_cmds_.getIPAddress(); }
    
    // ── Power ────────────────────────────────────────────────────
    if (name == "lock") { sys_cmds_.lockScreen(); return "Screen locked"; }
    if (name == "sleep") { sys_cmds_.sleepComputer(); return "Going to sleep..."; }
    
    // ── Files ────────────────────────────────────────────────────
    if (name == "list_downloads") {
        auto files = file_mgr_.listDownloads();
        std::ostringstream ss; ss << "Downloads (" << files.size() << " items):\n";
        for (size_t i = 0; i < std::min(files.size(), (size_t)20); i++) {
            ss << "  " << (files[i].is_directory ? "📁 " : "📄 ") << files[i].name;
            if (!files[i].is_directory) ss << " (" << FileManager::formatFileSize(files[i].size_bytes) << ")";
            ss << "\n";
        }
        return ss.str();
    }
    if (name == "list_desktop") {
        auto files = file_mgr_.listDesktop();
        std::ostringstream ss; ss << "Desktop (" << files.size() << " items):\n";
        for (auto& f : files) ss << "  " << (f.is_directory ? "📁 " : "📄 ") << f.name << "\n";
        return ss.str();
    }
    if (name == "list_documents") {
        auto files = file_mgr_.listDocuments();
        std::ostringstream ss; ss << "Documents (" << files.size() << " items):\n";
        for (auto& f : files) ss << "  " << (f.is_directory ? "📁 " : "📄 ") << f.name << "\n";
        return ss.str();
    }
    if (name == "search_files") {
        std::string query = vars.count("query") ? vars.at("query") : "";
        std::string dir = vars.count("directory") ? vars.at("directory") : "";
        if (dir.empty()) { char p[MAX_PATH]; SHGetFolderPathA(nullptr, CSIDL_PROFILE, nullptr, 0, p); dir = p; }
        auto results = file_mgr_.searchFiles(dir, query, true, 10);
        std::ostringstream ss; ss << "Found " << results.size() << " results:\n";
        for (auto& f : results) ss << "  " << f.full_path << "\n";
        return ss.str();
    }
    
    // ── Browser / Web ────────────────────────────────────────────
    if (name == "search_web") {
        std::string q = vars.count("query") ? vars.at("query") : "";
        // Offline-only: open search in the user's local browser
        sys_cmds_.searchInBrowser(q, "edge");
        return "🔍 Opened search in browser: " + q;
    }
    if (name == "open_url") {
        std::string url = vars.count("url") ? vars.at("url") : "";
        openUrl(url);
        return "Opened: " + url;
    }
    if (name == "search_in_browser") {
        std::string q = vars.count("query") ? vars.at("query") : "";
        std::string b = vars.count("browser") ? vars.at("browser") : "edge";
        sys_cmds_.searchInBrowser(q, b);
        return "Searching in " + b + ": " + q;
    }
    
    // ── Settings ─────────────────────────────────────────────────
    if (name == "open_settings") {
        std::string page = vars.count("page") ? vars.at("page") : "";
        sys_cmds_.openSettingsPage(page);
        return "Opened " + page + " settings";
    }
    if (name == "task_manager") { sys_cmds_.openTaskManager(); return "Task Manager opened"; }
    
    // ── Clipboard ────────────────────────────────────────────────
    if (name == "clipboard_get") { return "Clipboard: " + window_mgr_.getClipboard(); }
    if (name == "clipboard_set") {
        std::string t = vars.count("text") ? vars.at("text") : "";
        window_mgr_.setClipboard(t);
        return "Copied to clipboard";
    }
    
    // ── Timer / Stopwatch ────────────────────────────────────────
    if (name == "set_timer") {
        std::string amt = vars.count("amount") ? vars.at("amount") : "0";
        std::string unit = vars.count("unit") ? vars.at("unit") : "minutes";
        int secs = 60; // default
        try { secs = std::stoi(amt); } catch (...) {}
        if (unit.find("min") != std::string::npos) secs *= 60;
        else if (unit.find("hour") != std::string::npos) secs *= 3600;
        sys_cmds_.startTimer(secs, "Timer");
        return "Timer set for " + amt + " " + unit;
    }
    if (name == "start_stopwatch") { sys_cmds_.startStopwatch(); return "Stopwatch started"; }
    if (name == "stop_stopwatch") {
        std::string t = sys_cmds_.getStopwatchTime();
        sys_cmds_.stopStopwatch();
        return "Stopwatch stopped: " + t;
    }
    
    // ── Screenshot ───────────────────────────────────────────────
    if (name == "screenshot") {
        std::string path = window_mgr_.takeScreenshot();
        return "Screenshot saved: " + path;
    }
    
    // ── Type / Press key ─────────────────────────────────────────
    if (name == "press_key") {
        std::string k = vars.count("key") ? vars.at("key") : "";
        window_mgr_.pressKey(k);
        return "Pressed: " + k;
    }
    if (name == "type_text") {
        std::string t = vars.count("text") ? vars.at("text") : "";
        std::string target = vars.count("target") ? vars.at("target") : "";
        window_mgr_.typeText(t, 0.02f, target);
        return "Typed text";
    }
    
    // ── Focus mode ───────────────────────────────────────────────
    if (name == "focus_mode") {
        int mins = 25;
        if (vars.count("minutes") && !vars.at("minutes").empty())
            try { mins = std::stoi(vars.at("minutes")); } catch (...) {}
        sys_cmds_.startFocusMode(mins);
        return "Focus mode started for " + std::to_string(mins) + " minutes";
    }
    
    // ── Health scan ──────────────────────────────────────────────
    if (name == "health_scan") {
        auto report = sys_cmds_.systemHealthScan();
        std::ostringstream ss;
        ss << "System Health: " << report["status"].get<std::string>() << "\n"
           << "  RAM: " << report["ram_usage"].get<int>() << "% used\n"
           << "  Uptime: " << report["uptime"].get<std::string>() << "\n";
        if (!report["warnings"].empty()) {
            ss << "  ⚠️ Warnings:\n";
            for (auto& w : report["warnings"]) ss << "    - " << w.get<std::string>() << "\n";
        }
        return ss.str();
    }
    
    // ── Running apps ─────────────────────────────────────────────
    if (name == "running_apps") {
        auto apps = sys_cmds_.listRunningApps();
        std::ostringstream ss; ss << "Running apps (" << apps.size() << "):\n";
        for (auto& a : apps) {
            ss << "  " << a.name << " — " << a.window_title;
            if (a.memory_mb > 0) ss << " (" << (int)a.memory_mb << " MB)";
            ss << "\n";
        }
        return ss.str();
    }
    
    // ── Help ─────────────────────────────────────────────────────
    if (name == "help") { return getHelpTextStatic(); }
    
    return "Unhandled template: " + name;
}

// ═══════════════════ App / URL Opening ═══════════════════

bool VisionAI::openApp(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    
    // Check alias memory
    std::string resolved = agent_memory_.resolveAlias(lower);
    if (resolved != lower) lower = resolved;

    // 1. Check Microsoft Store / UWP apps FIRST (installed apps)
    auto store_it = STORE_APPS.find(lower);
    if (store_it != STORE_APPS.end()) {
        HINSTANCE r = ShellExecuteA(nullptr, "open", store_it->second.c_str(), nullptr, nullptr, SW_SHOW);
        if (reinterpret_cast<intptr_t>(r) > 32) return true;
    }
    
    // 2. Check app shortcuts (e.g. "notepad" → "notepad.exe")
    auto app_it = APP_SHORTCUTS.find(lower);
    if (app_it != APP_SHORTCUTS.end()) {
        ShellExecuteA(nullptr, "open", app_it->second.c_str(), nullptr, nullptr, SW_SHOW);
        return true;
    }

    // 3. Try as local file or folder via ShellExecute (handles paths and explorer)
    if (lower.find('\\') != std::string::npos || lower.find('/') != std::string::npos || lower.find(':') != std::string::npos) {
        HINSTANCE r = ShellExecuteA(nullptr, "open", lower.c_str(), nullptr, nullptr, SW_SHOW);
        if (reinterpret_cast<intptr_t>(r) > 32) return true;
    }
    
    // 4. Check URL shortcuts (only if no app matches)
    auto url_it = URL_SHORTCUTS.find(lower);
    if (url_it != URL_SHORTCUTS.end()) {
        ShellExecuteA(nullptr, "open", url_it->second.c_str(), nullptr, nullptr, SW_SHOW);
        return true;
    }
    
    // 5. Try opening as-is with fallback extension
    std::string exe = lower;
    if (exe.find('.') == std::string::npos) exe += ".exe";
    
    HINSTANCE result = ShellExecuteA(nullptr, "open", exe.c_str(), nullptr, nullptr, SW_SHOW);
    return reinterpret_cast<intptr_t>(result) > 32;
}

bool VisionAI::openUrl(const std::string& url, const std::string& browser) {
    std::string full_url = url;
    if (full_url.find("://") == std::string::npos) full_url = "https://" + full_url;
    
    if (!browser.empty()) {
        sys_cmds_.openUrlInBrowser(full_url, browser);
        return true;
    }
    
    ShellExecuteA(nullptr, "open", full_url.c_str(), nullptr, nullptr, SW_SHOW);
    return true;
}

// ═══════════════════ Chat UI ═══════════════════

void VisionAI::addMessage(const std::string& sender, const std::string& text) {
    emit messageReady(QString::fromStdString(sender), QString::fromStdString(text));
}

void VisionAI::appendMessage(const QString& sender, const QString& text) {
    QString color = (sender == "You") ? "#7B68EE" : "#4CAF50";
    QString html = QString("<p><b style='color:%1'>%2:</b> %3</p>")
                       .arg(color, sender, text.toHtmlEscaped().replace("\n", "<br>"));
    chat_display_->append(html);
    
    // Auto-scroll
    auto* sb = chat_display_->verticalScrollBar();
    sb->setValue(sb->maximum());
}

void VisionAI::updateStatus(const std::string& text, const std::string& color) {
    emit statusReady(QString::fromStdString(text),
                     QString::fromStdString(color));
}

void VisionAI::setStatusText(const QString& text, const QString& color) {
    status_label_->setText(text);
    if (!color.isEmpty()) {
        status_label_->setStyleSheet(
            QString("color: %1; font-size: 11px; padding: 4px;").arg(color));
    } else {
        status_label_->setStyleSheet("color: #666; font-size: 11px; padding: 4px;");
    }
}

void VisionAI::updateSystemStats() {
    MEMORYSTATUSEX mem{}; mem.dwLength = sizeof(mem);
    GlobalMemoryStatusEx(&mem);
    ram_label_->setText(QString("RAM: %1%").arg(mem.dwMemoryLoad));
    
    // CPU usage approximation (simple)
    static ULONGLONG last_idle = 0, last_kernel = 0, last_user = 0;
    FILETIME idle, kernel, user;
    GetSystemTimes(&idle, &kernel, &user);
    ULONGLONG i = *(ULONGLONG*)&idle, k = *(ULONGLONG*)&kernel, u = *(ULONGLONG*)&user;
    
    if (last_kernel > 0) {
        ULONGLONG di = i - last_idle;
        ULONGLONG dk = k - last_kernel;
        ULONGLONG du = u - last_user;
        ULONGLONG total = dk + du;
        int cpu_pct = total > 0 ? (int)(100.0 * (total - di) / total) : 0;
        cpu_label_->setText(QString("CPU: %1%").arg(cpu_pct));
    }
    last_idle = i; last_kernel = k; last_user = u;
}

// ═══════════════════ Events ═══════════════════

void VisionAI::closeEvent(QCloseEvent* event) {
    minimizeToTray();
    event->ignore();
}

bool VisionAI::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
    MSG* msg = static_cast<MSG*>(message);
    if (msg->message == WM_HOTKEY) {
        if (msg->wParam == (WPARAM)hotkey_id_) {
#ifdef VISION_HAS_WHISPER
            if (recording_) stopAndProcess();
            else startRecording();
#endif
            *result = 0;
            return true;
        }
        if (msg->wParam == (WPARAM)kill_switch_id_) {
            // Emergency Stop: cancel LLM generation + stop ReAct agent
            LOG_WARN("EMERGENCY STOP triggered (Ctrl+Esc)");
#ifdef VISION_HAS_LLM
            if (llm_controller_) llm_controller_->cancelGeneration();
            if (react_agent_) react_agent_->stop();
#endif
            addMessage("System", "⛔ Emergency Stop — generation cancelled");
            *result = 0;
            return true;
        }
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}

void VisionAI::minimizeToTray() { hide(); }
void VisionAI::showFromTray() { show(); activateWindow(); raise(); }

void VisionAI::onTrayActivated(QSystemTrayIcon::ActivationReason reason) {
    if (reason == QSystemTrayIcon::DoubleClick) showFromTray();
}

// ═══════════════════ Voice ═══════════════════

void VisionAI::startRecording() {
#ifdef VISION_HAS_WHISPER
    if (!audio_capture_ || !whisper_engine_) return;
    recording_ = true;
    audio_capture_->startRecording();
    
    // Visual feedback: change title, pulse input red, update tray tooltip
    setWindowTitle("🎤 LISTENING... — VISION AI");
    input_field_->setStyleSheet(
        "background: #2a2a2a; border: 2px solid #FF4444; border-radius: 8px; "
        "padding: 8px 14px; font-size: 14px; color: white;");
    input_field_->setPlaceholderText("🎤 Listening... Press Ctrl+Alt+Space to stop");
    if (tray_icon_) tray_icon_->setToolTip("VISION AI — 🎤 Listening...");
    updateStatus("🎤 Listening... (Ctrl+Alt+Space to stop)", "#FF4444");
#endif
}

void VisionAI::stopAndProcess() {
#ifdef VISION_HAS_WHISPER
    if (!recording_) return;
    recording_ = false;
    audio_capture_->stopRecording();
    
    // Reset visual indicators
    setWindowTitle("VISION AI");
    input_field_->setStyleSheet(
        "background: #2a2a2a; border: 1px solid #555; border-radius: 8px; "
        "padding: 8px 14px; font-size: 14px; color: white;");
    input_field_->setPlaceholderText("Type a command... (Enter to send, Ctrl+Alt+Space for voice)");
    if (tray_icon_) tray_icon_->setToolTip("VISION AI — Ready");
    updateStatus("🧠 Processing voice...", "#FFD700");
    
    // Store thread (not detached!) for safe shutdown
    if (audio_thread_.joinable()) audio_thread_.join();
    audio_thread_ = std::thread([this]() { processAudio(); });
#endif
}

void VisionAI::processAudio() {
#ifdef VISION_HAS_WHISPER
    auto audio_data = audio_capture_->getAudioData();
    if (audio_data.empty()) {
        updateStatus("No audio captured", "");
        return;
    }
    
    std::string text = whisper_engine_->transcribe(audio_data);
    if (text.empty()) {
        updateStatus("No speech detected", "");
        return;
    }
    
    // Route voice text through the full command pipeline (same as typed)
    addMessage("You (voice)", text);
    QMetaObject::invokeMethod(this, [this, text]() {
        input_field_->setText(QString::fromStdString(text));
        onSendCommand();
    }, Qt::QueuedConnection);
    updateStatus("Ready", "");
#endif
}

// ═══════════════════ Slots ═══════════════════

void VisionAI::onHistoryUp() {
    if (command_history_.empty()) return;
    history_index_ = std::min(history_index_ + 1, (int)command_history_.size() - 1);
    input_field_->setText(QString::fromStdString(command_history_[history_index_]));
}

void VisionAI::onHistoryDown() {
    if (history_index_ <= 0) { history_index_ = -1; input_field_->clear(); return; }
    history_index_--;
    input_field_->setText(QString::fromStdString(command_history_[history_index_]));
}

void VisionAI::onClearChat() { chat_display_->clear(); }

void VisionAI::onCopyLast() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!last_response_.empty()) window_mgr_.setClipboard(last_response_);
}

void VisionAI::onPresetClicked(const QString& preset) {
    input_field_->setText(preset);
    onSendCommand();
}

void VisionAI::onSettingsClicked() { showSettingsDialog(); }
void VisionAI::onHelpClicked() { showHelpDialog(); }

void VisionAI::showHelpDialog() {
    QMessageBox::information(this, "VISION AI Help", QString::fromStdString(getHelpTextStatic()));
}

void VisionAI::showSettingsDialog() {
    sys_cmds_.openSettingsPage("about");
}

static std::string getHelpTextStatic() {
    return "VISION AI Commands:\n\n"
           "Apps: open/close/switch to [app]\n"
           "Windows: minimize/maximize/snap left/right/tile\n"
           "Volume: volume up/down, set volume 50\n"
           "Brightness: brightness up/down, set brightness 70\n"
           "Clipboard: clipboard, copy [text]\n"
           "Files: list downloads/desktop/documents\n"
           "Search: search [query], open [url]\n"
           "System: battery, storage, uptime, health scan\n"
           "Screenshot: take screenshot\n"
           "Timer: set timer 5 minutes\n"
           "Focus: focus mode [minutes]\n"
           "Power: lock, sleep\n"
           "\nVoice: Press Ctrl+Alt+Space to toggle recording\n"
           "Keyboard: Ctrl+L clear, Ctrl+Shift+C copy last\n";
}

void VisionAI::setStartup(bool enable) {
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
                      "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE | KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS) {
        if (enable) RegSetValueExA(hKey, "VISION_AI", 0, REG_SZ, (BYTE*)exe, (DWORD)strlen(exe)+1);
        else RegDeleteValueA(hKey, "VISION_AI");
        RegCloseKey(hKey);
    }
}

} // namespace vision
