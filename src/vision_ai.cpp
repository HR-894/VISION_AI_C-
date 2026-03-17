/**
 * @file vision_ai.cpp
 * @brief Main application class — Qt6 GUI, command pipeline, system tray, hotkeys
 */

#include "vision_ai.h"
#include "settings_dialog.h"
#include "doctor.h"
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
#include <eh.h>       // _set_se_translator: converts SEH → C++ exceptions
#include <QtConcurrent>  // PRD Fix 1: async command execution

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
    
    // Req 2: Real-time UI streaming
    connect(this, &VisionAI::tokenGenerated, chat_widget_, &ChatWidget::streamToken, Qt::QueuedConnection);

    // PRD Fix 1: Create async command watcher (signals/slots, no blocking)
    cmd_watcher_ = new QFutureWatcher<void>(this);

    updateStatus("Ready | " + profiler_.getStatusString());
    addMessage("VISION", "Hello! I'm VISION AI (C++)). Type a command or press Ctrl+Alt+Space to use voice.");
}

VisionAI::~VisionAI() {
    // ═══ FIX B6: GRACEFUL SHUTDOWN ═══════════════════════════════
    // STEP 1: Signal ALL background threads to stop IMMEDIATELY.
    // This MUST be the first line — prevents use-after-free when
    // background threads try to access destroyed members.
    is_shutting_down_ = true;

    // STEP 2: Cancel any in-flight Groq API curl request.
    // Without this, the cmd_thread_ could be stuck in curl_easy_perform()
    // for up to 30 seconds, making the app "Not Responding" on close.
#ifdef VISION_HAS_LLM
    if (llm_controller_) {
        // Cancel cloud generation — triggers curl's progress callback to abort
        llm_controller_->cancelGeneration();
    }
#endif

    // STEP 3: Join all background threads safely.
    if (model_load_thread_.joinable()) model_load_thread_.join();

    // PRD Fix 1: Wait for async command future (non-blocking during normal
    // operation, only blocks here during shutdown after cancelGeneration)
    if (cmd_future_.isRunning()) {
        cmd_future_.waitForFinished();
    }

#ifdef VISION_HAS_WHISPER
    if (audio_capture_) audio_capture_->stopRecording();  // Stop mic stream
    if (audio_thread_.joinable())      audio_thread_.join();
#endif

    // Stop Screen Observer (before destroying OCR engine)
    if (screen_observer_) screen_observer_->stop();

    // STEP 4: Unregister hotkeys
    if (hotkey_registered_) {
        UnregisterHotKey((HWND)winId(), hotkey_id_);
    }
    if (kill_switch_registered_) {
        UnregisterHotKey((HWND)winId(), kill_switch_id_);
    }

    // STEP 5: Destroy AI subsystems in reverse dependency order
    // (pointers are destroyed here explicitly before stack members)
#ifdef VISION_HAS_LLM
    react_agent_.reset();
    action_executor_.reset();
    llm_controller_.reset();  // Shuts down backends, frees VRAM
#endif
#ifdef VISION_HAS_WHISPER
    audio_capture_.reset();
    whisper_engine_.reset();
#endif
    screen_observer_.reset();  // After OCR shutdown

    // Save vector memory to disk before shutdown
    if (vector_memory_) {
        std::string mem_path = (fs::path(config_.getDataDir()) / "vector_memory.bin").string();
        vector_memory_->save(mem_path);
        vector_memory_.reset();
    }
    // Stack members (router_, template_matcher_, etc.) destroyed by
    // compiler-generated destructor in reverse declaration order.
    // ═══════════════════════════════════════════════════════════════
}

// ═══════════════════ UI Setup ═══════════════════

void VisionAI::setupUI() {
    setWindowTitle("VISION AI");
    setMinimumSize(700, 500);
    resize(850, 650);

    // ── Global Discord Dark Theme ────────────────────────────────
    setStyleSheet(
        "QMainWindow, QWidget { background-color: #1e1f22; color: #dbdee1; }"
        "QToolTip { background-color: #2b2d31; color: #dbdee1; border: 1px solid #3f3f46; "
        "  border-radius: 6px; padding: 4px 8px; font-size: 12px; }"
    );

    auto* central = new QWidget(this);
    setCentralWidget(central);
    auto* main_layout = new QVBoxLayout(central);
    main_layout->setContentsMargins(14, 14, 14, 14);
    main_layout->setSpacing(10);
    
    // ── Header ───────────────────────────────────────────────────
    auto* header = new QHBoxLayout();
    auto* title = new QLabel("🔮 VISION AI");
    title->setStyleSheet(
        "font-size: 22px; font-weight: bold; color: #5865F2; "
        "font-family: 'Inter', 'Segoe UI', sans-serif;");

    cpu_label_ = new QLabel("CPU: --");
    cpu_label_->setStyleSheet("color: #949ba4; font-size: 11px; font-family: 'Inter', 'Segoe UI';");
    ram_label_ = new QLabel("RAM: --");
    ram_label_->setStyleSheet("color: #949ba4; font-size: 11px; font-family: 'Inter', 'Segoe UI';");

    auto* settings_btn = new QPushButton("⚙");
    settings_btn->setFixedSize(34, 34);
    settings_btn->setStyleSheet(
        "QPushButton { font-size: 16px; border: 1px solid #3f3f46; border-radius: 8px; "
        "  background: #2b2d31; color: #dbdee1; }"
        "QPushButton:hover { background: #3f3f46; border-color: #5865F2; }");
    connect(settings_btn, &QPushButton::clicked, this, &VisionAI::onSettingsClicked);

    auto* help_btn = new QPushButton("?");
    help_btn->setFixedSize(34, 34);
    help_btn->setStyleSheet(
        "QPushButton { font-size: 16px; border: 1px solid #3f3f46; border-radius: 8px; "
        "  background: #2b2d31; color: #dbdee1; }"
        "QPushButton:hover { background: #3f3f46; border-color: #5865F2; }");
    connect(help_btn, &QPushButton::clicked, this, &VisionAI::onHelpClicked);

    header->addWidget(title);
    header->addStretch();
    
    // ── Req 1: AI Preset Selector ──
    preset_selector_ = new QComboBox();
    preset_selector_->setStyleSheet(
        "QComboBox { background: #2b2d31; border: 1px solid #3f3f46; border-radius: 8px; "
        "  padding: 4px 10px; font-size: 12px; color: #dbdee1; "
        "  font-family: 'Inter', 'Segoe UI'; min-width: 140px; }"
        "QComboBox::drop-down { border: none; }"
        "QComboBox QAbstractItemView { background: #2b2d31; color: #dbdee1; "
        "  selection-background-color: #5865F2; outline: none; border: 1px solid #3f3f46; }"
    );
    // Load presets from external presets.json
    nlohmann::json config_presets = nlohmann::json::array();
    std::string presets_path = (std::filesystem::path(config_.getDataDir()) / "presets.json").string();
    std::ifstream preset_file(presets_path);
    if (preset_file.is_open()) {
        try { config_presets = nlohmann::json::parse(preset_file); } catch (...) {}
    }
    for (const auto& preset : config_presets) {
        if (preset.contains("name")) {
            preset_selector_->addItem(QString::fromStdString(preset["name"].get<std::string>()));
        }
    }
    if (preset_selector_->count() == 0) {
        preset_selector_->addItem("Default Agent"); // Fallback
    }
    connect(preset_selector_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &VisionAI::onPresetChanged);
    
    header->addWidget(preset_selector_);
    header->addSpacing(10);
    // ───────────────────────────────

    header->addWidget(cpu_label_);
    header->addWidget(ram_label_);
    header->addWidget(settings_btn);
    header->addWidget(help_btn);
    main_layout->addLayout(header);
    
    // ── Chat display (QListView + Custom Delegate) ───────────────
    chat_widget_ = new ChatWidget(this);
    main_layout->addWidget(chat_widget_, 1);
    
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
            "QPushButton { background: #2b2d31; border: 1px solid #3f3f46; border-radius: 8px; "
            "  padding: 6px 14px; font-size: 11px; color: #dbdee1; "
            "  font-family: 'Inter', 'Segoe UI'; }"
            "QPushButton:hover { background: #3f3f46; border-color: #5865F2; color: #fff; }"
            "QPushButton:pressed { background: #5865F2; }");
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
        "QLineEdit { background: #2b2d31; border: 2px solid #3f3f46; border-radius: 10px; "
        "  padding: 10px 16px; font-size: 14px; color: #dbdee1; "
        "  font-family: 'Inter', 'Segoe UI', sans-serif; }"
        "QLineEdit:focus { border-color: #5865F2; }"
        "QLineEdit::placeholder { color: #949ba4; }");
    connect(input_field_, &QLineEdit::returnPressed, this, &VisionAI::onSendCommand);

    auto* send_btn = new QPushButton("▶");
    send_btn->setFixedSize(42, 40);
    send_btn->setStyleSheet(
        "QPushButton { background: #5865F2; border: none; border-radius: 10px; "
        "  font-size: 16px; color: white; }"
        "QPushButton:hover { background: #4752C4; }"
        "QPushButton:pressed { background: #3C45A5; }");
    connect(send_btn, &QPushButton::clicked, this, &VisionAI::onSendCommand);

    input_layout->addWidget(input_field_, 1);
    input_layout->addWidget(send_btn);
    main_layout->addLayout(input_layout);

    // ── Status bar ───────────────────────────────────────────────
    status_label_ = new QLabel("● Ready");
    status_label_->setStyleSheet(
        "color: #57F287; font-size: 11px; padding: 4px 8px; "
        "font-family: 'Inter', 'Segoe UI';");
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

    // PRD Fix 2: Read user-configured whisper settings from config
    std::string whisper_size = config_.getNested<std::string>("whisper.model_size", whisper_model);
    whisper_engine_ = std::make_unique<WhisperEngine>(whisper_size);

    std::string saved_whisper_path = config_.getNested<std::string>("whisper.model_path", "");
    if (!saved_whisper_path.empty()) {
        whisper_engine_->setModelPath(saved_whisper_path);
    }

    audio_capture_ = std::make_unique<AudioCapture>();
    
    if (whisper_engine_->loadModel()) {
        LOG_INFO("Whisper loaded: {}", whisper_engine_->getModelInfo());
    }
#endif
    
#ifdef VISION_HAS_LLM
    // [FIX] Read the saved model path from config so the local backend actually
    // finds the GGUF file the user selected in Settings, instead of searching
    // for a default that doesn't exist on fresh installs.
    std::string saved_model_path = config_.getNested<std::string>("llm.model_path", "");
    llm_controller_ = std::make_unique<LLMController>(saved_model_path);

    // ── Restore saved API key from encrypted config ──────────────
    // The SettingsDialog stores the key as DPAPI-encrypted Base64.
    // We decrypt it here so Cloud Mode works immediately on restart.
    {
        std::string encrypted_b64 = config_.get<std::string>("cloud_api_key_encrypted", "");
        if (!encrypted_b64.empty()) {
            std::string decrypted_key = SettingsDialog::decryptDPAPI(encrypted_b64);
            if (!decrypted_key.empty()) {
                llm_controller_->setCloudApiKey(decrypted_key);
                LOG_INFO("Cloud API key restored from encrypted config");
            } else {
                LOG_WARN("Failed to decrypt saved cloud API key — user must re-enter");
            }
        }
    }

    // Also restore backend mode from config
    {
        std::string engine_mode = config_.get<std::string>("engine_mode", "local");
        if (engine_mode == "cloud") {
            llm_controller_->setBackend(BackendType::Cloud);
            LOG_INFO("Engine mode restored: Cloud");
        } else if (engine_mode == "hybrid") {
            // Hybrid: start with cloud, fall back to local
            llm_controller_->setBackend(BackendType::Cloud);
            LOG_INFO("Engine mode restored: Hybrid (cloud-first)");
        }
        // else: default is Local, already set
    }

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

    // Wire LLM into AgentMemory for embedding generation
    agent_memory_.setLLM(llm_controller_.get());
    
    // Create ActionExecutor and ReActAgent under LLM guard (not OCR)
    action_executor_ = std::make_unique<ActionExecutor>(*this);
    react_agent_ = std::make_unique<ReActAgent>(
        *llm_controller_, *action_executor_, window_mgr_, *this);

    // Req 3: Expose internal monologue — emit live status during ReAct loop
    react_agent_->setStepCallback(
        [this](int step, const std::string& phase, const std::string& detail) {
            emit statusReady(
                QString("🤖 Step %1: %2 — %3")
                    .arg(step)
                    .arg(QString::fromStdString(phase))
                    .arg(QString::fromStdString(detail)),
                "#5865F2"
            );
        }
    );

    router_.setReActAgent(react_agent_.get());
    LOG_INFO("AI ReAct agent initialized (with UI step callback)");

    // ── Idle Timer: check every 60s, auto-unload LLM after 5min idle ──
    idle_timer_ = new QTimer(this);
    connect(idle_timer_, &QTimer::timeout, this, [this]() {
#ifdef VISION_HAS_LLM
        if (llm_controller_) llm_controller_->checkIdleUnload();
#endif
    });
    idle_timer_->start(60000);  // Check every 60 seconds
#endif

    // ── Screen Awareness: Start Lazy Observer ────────────────────
    screen_observer_ = std::make_unique<ScreenObserver>();
#ifdef VISION_HAS_OCR
    // Wire OCR callback — reuses ActionExecutor's persistent Tesseract (B1 fix)
    if (action_executor_ && action_executor_->isOCRAvailable()) {
        screen_observer_->start(
            [this](const uint8_t* bgra, int w, int h) -> std::string {
                if (is_shutting_down_.load()) return "";
                // Convert BGRA → cv::Mat for Tesseract
                cv::Mat img(h, w, CV_8UC4, const_cast<uint8_t*>(bgra));
                cv::Mat bgr;
                cv::cvtColor(img, bgr, cv::COLOR_BGRA2BGR);
                auto result = action_executor_->runOCR(bgr, true);
                return result.valid ? result.full_text : "";
            },
            500  // Poll every 500ms
        );
        LOG_INFO("Screen Observer started (DXGI + pHash + OCR-on-delta)");
    } else {
        // No OCR — start without callback (just captures hashes for change detection)
        LOG_WARN("Screen Observer: No OCR available, running in hash-only mode");
    }
#else
    LOG_WARN("Screen Observer: OCR not compiled, running in hash-only mode");
#endif

    // ── Vector Memory: Init + Load persisted data ───────────────
    vector_memory_ = std::make_unique<VectorMemory>();

    // PRD Fix 6: Lightweight hash-based embeddings — zero model load, zero OOM risk.
    // Uses character trigram hashing to produce a 128-dim vector.
    // This revives the AVX2 SIMD search without loading the heavy LLM.
    vector_memory_->setEmbeddingFn(
        [](const std::string& text) -> std::vector<float> {
            const int DIM = 128;
            std::vector<float> vec(DIM, 0.0f);
            // Character trigram hashing
            std::string lower = text;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            for (size_t i = 0; i + 2 < lower.size(); i++) {
                uint32_t hash = ((uint32_t)(unsigned char)lower[i] * 31u +
                                 (uint32_t)(unsigned char)lower[i+1]) * 31u +
                                 (uint32_t)(unsigned char)lower[i+2];
                vec[hash % DIM] += 1.0f;
            }
            // Also hash word-level features
            std::istringstream iss(lower);
            std::string word;
            while (iss >> word) {
                uint32_t wh = 0;
                for (char c : word) wh = wh * 37u + (uint32_t)(unsigned char)c;
                vec[wh % DIM] += 2.0f;  // Words get higher weight
            }
            // L2 normalize
            float norm = 0.0f;
            for (float v : vec) norm += v * v;
            norm = std::sqrt(norm);
            if (norm > 0.0f) for (float& v : vec) v /= norm;
            return vec;
        }
    );
    // Load persisted memories
    std::string mem_path = (fs::path(config_.getDataDir()) / "vector_memory.bin").string();
    if (fs::exists(mem_path)) {
        vector_memory_->load(mem_path);
        LOG_INFO("Vector Memory loaded: {} entries", vector_memory_->size());
    }

    // ── Confidence Scorer: Init with callbacks ──────────────────
    confidence_scorer_ = std::make_unique<ConfidenceScorer>();
    confidence_scorer_->setVectorMemory(vector_memory_.get());
    confidence_scorer_->setAppListFn([this]() -> std::vector<std::string> {
        auto windows = window_mgr_.listWindows();
        std::vector<std::string> names;
        names.reserve(windows.size());
        for (const auto& w : windows) {
            if (!w.exe_name.empty()) names.push_back(w.exe_name);
        }
        return names;
    });
    confidence_scorer_->setTemplateScoreFn([this](const std::string& cmd) -> float {
        auto match = template_matcher_.match(cmd);
        return match ? match->confidence : 0.0f;
    });

    // ── HITL Timeout Timer (60s ghost state prevention) ────────
    hitl_timeout_timer_ = new QTimer(this);
    hitl_timeout_timer_->setSingleShot(true);
    connect(hitl_timeout_timer_, &QTimer::timeout, this, [this]() {
        if (confidence_scorer_ && confidence_scorer_->hasPending()) {
            confidence_scorer_->clearPending();
            addMessage("SYSTEM", "⏳ Request timed out. Operation cancelled.");
            emit statusReady("Ready", "");
        }
    });
    LOG_INFO("Confidence Scorer initialized");

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

    // ── HITL: Check if there's a pending action to resolve ──────
    if (confidence_scorer_ && confidence_scorer_->hasPending()) {
        hitl_timeout_timer_->stop();  // Cancel timeout

        auto resolve = confidence_scorer_->tryResolve(command);
        switch (resolve.result) {
            case ConfidenceScorer::ResolveResult::Matched: {
                // User confirmed — execute the resolved command
                addMessage("SYSTEM", "✅ Confirmed. Executing: " + resolve.resolved_command);
                command = resolve.resolved_command;
                break;  // Fall through to normal execution
            }
            case ConfidenceScorer::ResolveResult::Bailout: {
                addMessage("SYSTEM", "❌ Operation cancelled.");
                emit statusReady("Ready", "");
                return;
            }
            case ConfidenceScorer::ResolveResult::NoMatch: {
                // Invalid option — re-ask
                auto pending = confidence_scorer_->getPending();
                if (pending) {
                    addMessage("VISION", "❓ That didn't match. " + pending->clarification_question);
                    hitl_timeout_timer_->start(60000);  // Restart timeout
                }
                return;
            }
            case ConfidenceScorer::ResolveResult::NoPending:
                break;  // Expired, treat as new command
        }
    }
    
    // Track behavior
    behavior_.recordCommand(command, context_mgr_.getActiveApp().name);
    
    // ── Slash commands (instant, no background thread needed) ────
    if (!command.empty() && command[0] == '/') {
        std::string lower_cmd = command;
        std::transform(lower_cmd.begin(), lower_cmd.end(), lower_cmd.begin(), ::tolower);
        
        if (lower_cmd == "/doctor" || lower_cmd == "/health") {
#ifdef VISION_HAS_LLM
            std::string model_path = config_.getNested<std::string>("llm.model_path", "");
            std::string whisper_path = config_.getNested<std::string>("whisper.model_path", "");
            std::string api_key = config_.getNested<std::string>("cloud.api_key", "");
            bool local_loaded = llm_controller_ ? llm_controller_->isModelLoaded() : false;
            bool cloud_init = false;
            int gpu_layers = config_.getNested<int>("llm.gpu_layers", 0);
            size_t mem_entries = 0;
            size_t conv_size = llm_controller_ ? llm_controller_->getConversation().size() : 0;
            
            std::string report = Doctor::runFullDiagnostic(
                model_path, whisper_path, api_key, "",
                local_loaded, cloud_init, gpu_layers, mem_entries, conv_size
            );
            addMessage("DOCTOR", report);
#else
            addMessage("DOCTOR", "LLM module not compiled. Doctor requires VISION_HAS_LLM.");
#endif
            emit statusReady("Ready", "");
            return;
        }
        
        if (lower_cmd == "/status") {
#ifdef VISION_HAS_LLM
            std::string status = "=== VISION AI Status ===\n";
            status += "Backend: " + std::string(llm_controller_ ? 
                (llm_controller_->getActiveBackend() == BackendType::Local ? "Local (llama.cpp)" : "Cloud (Groq)") : "N/A") + "\n";
            status += "Model loaded: " + std::string(llm_controller_ && llm_controller_->isModelLoaded() ? "Yes" : "No") + "\n";
            status += "Failover: " + std::string(llm_controller_ && llm_controller_->isFailoverEnabled() ? "Enabled" : "Disabled") + "\n";
            status += "Failover count: " + std::to_string(llm_controller_ ? llm_controller_->getFailoverCount() : 0) + "\n";
            status += "Conversation: " + std::to_string(llm_controller_ ? llm_controller_->getConversation().size() : 0) + " messages";
            addMessage("STATUS", status);
#else
            addMessage("STATUS", "LLM module not compiled.");
#endif
            emit statusReady("Ready", "");
            return;
        }
        
        if (lower_cmd == "/clear") {
#ifdef VISION_HAS_LLM
            if (llm_controller_) llm_controller_->clearConversation();
            addMessage("SYSTEM", "Conversation cleared.");
#endif
            emit statusReady("Ready", "");
            return;
        }
    }
    
    // ═══ PRD Fix 1: Event-Driven Async Command Execution ═══════════════
    // Cancel any in-flight LLM generation — it will exit within milliseconds.
    // Unlike the old pattern (cmd_thread_.join()), this does NOT block the UI.
#ifdef VISION_HAS_LLM
    if (llm_controller_) {
        llm_controller_->cancelGeneration();
    }
#endif
    // If a previous future is still running, cancel will cause it to exit soon.
    // QtConcurrent manages the thread pool — no manual join needed.
    cmd_future_ = QtConcurrent::run([this, command]() {
      // Convert Windows SEH (access violations from llama.cpp) into C++ exceptions
      // so they're caught by catch(...) below instead of killing the process.
      // _set_se_translator is per-thread and MSVC-compatible with C++ destructors.
      _set_se_translator([](unsigned int code, EXCEPTION_POINTERS*) {
          throw std::runtime_error(
              "Access violation in AI engine (SEH code: 0x" +
              ([code]() -> std::string {
                  char buf[16];
                  snprintf(buf, sizeof(buf), "%08X", code);
                  return buf;
              })() + "). Try switching to Cloud backend or restart.");
      });
      try {
        // FIX B6: Check shutdown flag before touching any member
        if (is_shutting_down_.load()) return;
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
                // FIX B4: Check shutdown flag inside chained macros to prevent
                // app destruction wait hangs
                if (is_shutting_down_.load()) return;
                
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

            // Safety net: if the template matched but execution failed
            // (e.g. "open chrome youtube" → "Couldn't find app 'chrome youtube'"),
            // don't return — fall through to the LLM agent for smarter intent parsing
            std::string lower_result = result;
            std::transform(lower_result.begin(), lower_result.end(),
                           lower_result.begin(), ::tolower);
            if (lower_result.find("couldn't find app") != std::string::npos ||
                lower_result.find("not found") != std::string::npos ||
                lower_result.find("failed to open") != std::string::npos) {
                LOG_WARN("Template '{}' failed for '{}' — falling through to agent",
                         match->template_name, command);
                // Fall through to Step 4 (agent)
            } else {
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
        }
        
        // 4. Route to AI agent — the AI decides everything
        // ── Safety-Only Gate: block truly dangerous operations ──────
        // Only hard-block commands containing dangerous keywords (format, delete system32, etc.)
        // Everything else goes directly to the ReAct agent which has full
        // command+chat capability via its dual-mode system prompt.
        if (confidence_scorer_) {
            auto conf = confidence_scorer_->score(command);
            if (conf.safety_blocked) {
                // Dangerous command — still require human confirmation
                PendingAction pending;
                pending.original_command = command;
                pending.confidence = conf;
                pending.clarification_question =
                    "⚠️ This looks like a dangerous operation: \"" + command +
                    "\". Type 'yes' to confirm or 'cancel' to abort.";
                pending.options = {"yes", "confirm", "do it", "haan"};
                pending.resolved_command = command;

                confidence_scorer_->setPending(std::move(pending));
                emit messageReady("VISION", QString::fromStdString(
                    confidence_scorer_->getPending()->clarification_question));
                emit statusReady("Waiting for confirmation...", "#FEE75C");
                QMetaObject::invokeMethod(hitl_timeout_timer_, "start",
                    Qt::QueuedConnection, Q_ARG(int, 60000));
                return;
            }
        }

        std::string routed_command = command;

        // Inject vector memory context
        if (vector_memory_ && vector_memory_->size() > 0) {
            std::string mem_ctx = vector_memory_->getRelevantContext(command, 3);
            if (!mem_ctx.empty()) {
                routed_command += "\n\n" + mem_ctx;
                LOG_INFO("Injected vector memory context ({} entries)",
                         vector_memory_->size());
            }
        }

        // Inject screen context for screen-related queries
        if (screen_observer_ && screen_observer_->snapshotCount() > 0) {
            std::string lower = command;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if (lower.find("screen") != std::string::npos ||
                lower.find("see") != std::string::npos ||
                lower.find("looking at") != std::string::npos ||
                lower.find("display") != std::string::npos ||
                lower.find("read") != std::string::npos ||
                lower.find("what's on") != std::string::npos ||
                lower.find("notification") != std::string::npos) {
                std::string ctx = screen_observer_->getContextText(3);
                routed_command = command + "\n\n" + ctx;
                LOG_INFO("Injected screen context ({} snapshots)",
                         screen_observer_->snapshotCount());
            }
        }
        // Direct agent call — skip router (it re-runs matchers we already tried)
#ifdef VISION_HAS_LLM
        // Req 2: Create an empty message bubble for the AI, then stream into it
        QMetaObject::invokeMethod(this, [this]() {
            addMessage("VISION", ""); 
        }, Qt::QueuedConnection);
        
        auto stream_cb = [this](const std::string& piece) {
            emit tokenGenerated(QString::fromStdString(piece));
        };
        
        auto [success, result] = react_agent_
            ? react_agent_->executeTask(routed_command, stream_cb)
            : std::make_pair(false, std::string("AI agent not available. Try a simpler command."));
#else
        auto [success, result] = std::make_pair(false, std::string("AI not compiled in this build."));
#endif

        // Store command+result in vector memory for future context
        if (vector_memory_ && !command.empty()) {
            vector_memory_->store(command, result, {"agent"});
        }
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            last_response_ = result;
        }
        
        // Note: We don't emit messageReady here if we streamed it, but we do 
        // it as a fallback / to ensure the final state is synchronized. 
        // For now, streamToken appended it. We'll let it be.
        emit statusReady("Ready", "");
        context_mgr_.recordCommand(command, result);
        agent_memory_.recordTask(command, result, success, {"agent"});

      } catch (const std::exception& e) {
          LOG_ERROR("Background thread crashed: {}", e.what());
          emit messageReady("SYSTEM", QString::fromStdString(
              "\u274c Error executing command: " + std::string(e.what())));
          emit statusReady("Ready", "");
      } catch (...) {
          LOG_ERROR("Background thread crashed with unknown exception");
          emit messageReady("SYSTEM",
              "\u274c Unknown critical error occurred in background thread.");
          emit statusReady("Ready", "");
      }
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

    // 5. Fuzzy match: scan APP_SHORTCUTS and STORE_APPS for typo tolerance
    //    Uses Levenshtein distance ≤ 2 to auto-correct minor typos
    auto levenshtein = [](const std::string& a, const std::string& b) -> int {
        int m = (int)a.size(), n = (int)b.size();
        std::vector<std::vector<int>> dp(m + 1, std::vector<int>(n + 1));
        for (int i = 0; i <= m; i++) dp[i][0] = i;
        for (int j = 0; j <= n; j++) dp[0][j] = j;
        for (int i = 1; i <= m; i++) {
            for (int j = 1; j <= n; j++) {
                int cost = (a[i-1] == b[j-1]) ? 0 : 1;
                dp[i][j] = std::min({dp[i-1][j] + 1, dp[i][j-1] + 1, dp[i-1][j-1] + cost});
            }
        }
        return dp[m][n];
    };

    std::string best_match;
    std::string best_exe;
    int best_dist = 3;  // Only accept distance ≤ 2
    bool is_store_app = false;

    for (const auto& [key, exe] : APP_SHORTCUTS) {
        int d = levenshtein(lower, key);
        if (d < best_dist) {
            best_dist = d;
            best_match = key;
            best_exe = exe;
            is_store_app = false;
        }
    }
    for (const auto& [key, uri] : STORE_APPS) {
        int d = levenshtein(lower, key);
        if (d < best_dist) {
            best_dist = d;
            best_match = key;
            best_exe = uri;
            is_store_app = true;
        }
    }

    if (best_dist <= 2 && !best_match.empty()) {
        LOG_INFO("Fuzzy match: '{}' → '{}' (distance {})", lower, best_match, best_dist);
        HINSTANCE r = ShellExecuteA(nullptr, "open", best_exe.c_str(), nullptr, nullptr, SW_SHOW);
        if (reinterpret_cast<intptr_t>(r) > 32) {
            // Learn the alias so future uses are instant
            agent_memory_.learnAlias(lower, best_match);
            return true;
        }
    }
    
    // 6. Try opening as-is with fallback extension
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
    MessageType type = (sender == "You") ? MessageType::User : MessageType::AI;
    if (sender == "SYSTEM" || sender == "System") type = MessageType::System;
    chat_widget_->addMessage(sender, text, type);
}

void VisionAI::updateStatus(const std::string& text, const std::string& color) {
    emit statusReady(QString::fromStdString(text),
                     QString::fromStdString(color));
}

void VisionAI::setStatusText(const QString& text, const QString& color) {
    // Prefix with colored dot indicator
    QString dotColor = color.isEmpty() ? "#57F287" : color;
    QString displayText = QString("● %1").arg(text);
    status_label_->setText(displayText);
    status_label_->setStyleSheet(
        QString("color: %1; font-size: 11px; padding: 4px 8px; "
                "font-family: 'Inter', 'Segoe UI';").arg(dotColor));
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

    // Check LLM idle timeout — auto-unload after 5min of inactivity
#ifdef VISION_HAS_LLM
    if (llm_controller_) llm_controller_->checkIdleUnload();
#endif
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

void VisionAI::onClearChat() { chat_widget_->clear(); }

void VisionAI::onPresetChanged(int index) {
#ifdef VISION_HAS_LLM
    if (!llm_controller_ || index < 0) return;

    // Fetch the presets array from presets.json
    nlohmann::json presets = nlohmann::json::array();
    std::string presets_path = (std::filesystem::path(config_.getDataDir()) / "presets.json").string();
    std::ifstream preset_file(presets_path);
    if (preset_file.is_open()) {
        try { presets = nlohmann::json::parse(preset_file); } catch (...) {}
    }
    if (index >= (int)presets.size()) return;

    const auto& preset = presets[index];
    
    // Extract parameters with safe fallbacks
    std::string name = preset.value("name", "Unknown Preset");
    std::string prompt = preset.value("system_prompt", "");
    float temp = preset.value("temperature", 0.7f);
    float top_p = preset.value("top_p", 0.9f);

    // Apply strictly to LLM Controller
    llm_controller_->setSystemPrompt(prompt);
    llm_controller_->setTemperature(temp);
    llm_controller_->setTopP(top_p);

    // Provide UI feedback without cluttering chat history
    updateStatus("Persona Loaded: " + name, "#5865F2");
    LOG_INFO("Applied AI Preset: {} (temp:{}, top_p:{})", name, temp, top_p);
#endif
}

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
    SettingsDialog dlg(config_, this);

    // Wire API key changes to CloudBackend in real-time
    connect(&dlg, &SettingsDialog::apiKeyChanged, this, [this](const QString& key) {
#ifdef VISION_HAS_LLM
        if (llm_controller_) {
            llm_controller_->setCloudApiKey(key.toStdString());
            LOG_INFO("Settings: API key updated in CloudBackend");
        }
#endif
    });

    // Wire engine mode changes
    connect(&dlg, &SettingsDialog::engineChanged, this, [this](const QString& mode) {
        LOG_INFO("Settings: Engine mode changed to '{}'", mode.toStdString());
#ifdef VISION_HAS_LLM
        if (llm_controller_) {
            if (mode == "cloud") {
                llm_controller_->setBackend(BackendType::Cloud);
            } else if (mode == "local") {
                llm_controller_->setBackend(BackendType::Local);
            }
            // Hybrid = let LLMController auto-decide
        }
#endif
    });

    dlg.exec();

    // If settings were modified, check for first-run prompt
    if (dlg.wasModified()) {
        std::string engine = config_.get<std::string>("engine_mode", "local");
        std::string key = config_.get<std::string>("cloud_api_key_encrypted", "");

        if ((engine == "cloud" || engine == "hybrid") && key.empty()) {
            addMessage("VISION", "💡 To use Cloud mode, please click ⚙️ and enter your Groq API Key.\n"
                       "Get one free at: https://console.groq.com/keys");
        } else {
            addMessage("SYSTEM", "✅ Settings saved successfully.");
        }
    }
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
