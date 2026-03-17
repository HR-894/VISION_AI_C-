#pragma once
/**
 * @file vision_ai.h
 * @brief Main application class — Qt6 MainWindow with all subsystems
 * 
 * The central hub that owns all subsystem instances, manages the GUI,
 * system tray, hotkey listener, and provides the command processing pipeline.
 */

#include <string>
#include <vector>
#include <memory>
#include <deque>
#include <atomic>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <QMainWindow>
#include <QLineEdit>
#include <QLabel>
#include <QTimer>
#include <QSystemTrayIcon>
#include <QCloseEvent>
#include <QFutureWatcher>
#include <QFuture>
#include <QStyle>

#include "config_manager.h"
#include "device_profiler.h"
#include "safety_guard.h"
#include "window_manager.h"
#include "system_commands.h"
#include "file_manager.h"
#include "context_manager.h"
#include "agent_memory.h"
#include "user_behavior.h"
#include "web_search.h"
#include "smart_template_matcher.h"
#include "fast_complex_handler.h"
#include "command_router.h"
#include "chat_widget.h"
#include "screen_observer.h"
#include "vector_memory.h"
#include "confidence_scorer.h"

#ifdef VISION_HAS_WHISPER
#include "whisper_engine.h"
#include "audio_capture.h"
#endif

#ifdef VISION_HAS_LLM
#include "llm_controller.h"
#include "react_agent.h"
#include "action_executor.h"
#endif

namespace vision {

class VisionAI : public QMainWindow {
    Q_OBJECT

public:
    explicit VisionAI(QWidget* parent = nullptr);
    ~VisionAI() override;

    // ── Public subsystem access (for other components) ───────────
    ConfigManager& config() { return config_; }
    DeviceProfiler& profiler() { return profiler_; }
    SafetyGuard& safety() { return safety_; }
    WindowManager& windowMgr() { return window_mgr_; }
    SystemCommands& sysCmds() { return sys_cmds_; }
    FileManager& fileMgr() { return file_mgr_; }
    ContextManager& contextMgr() { return context_mgr_; }
    AgentMemory& memory() { return agent_memory_; }
    UserBehaviorTracker& behavior() { return behavior_; }
    WebSearch& webSearch() { return web_search_; }
    SmartTemplateMatcher& templateMatcher() { return template_matcher_; }
    FastComplexHandler& fastHandler() { return fast_handler_; }
    CommandRouter& router() { return router_; }

#ifdef VISION_HAS_LLM
    LLMController& llm() { return *llm_controller_; }
    ReActAgent& agent() { return *react_agent_; }
    ActionExecutor& executor() { return *action_executor_; }
#endif
#ifdef VISION_HAS_WHISPER
    WhisperEngine& whisper() { return *whisper_engine_; }
    AudioCapture& audio() { return *audio_capture_; }
#endif

    // ── Chat UI ──────────────────────────────────────────────────
    void addMessage(const std::string& sender, const std::string& text);
    void updateStatus(const std::string& text, const std::string& color = "");

    // ── Command execution ────────────────────────────────────────
    /// Instant execute for simple commands (open app, system info, etc.)
    std::string instantExecute(const std::string& text);

    // ── Static data maps ─────────────────────────────────────────
    static const std::unordered_map<std::string, std::string> APP_SHORTCUTS;
    static const std::unordered_map<std::string, std::string> STORE_APPS;
    static const std::unordered_map<std::string, std::string> URL_SHORTCUTS;

signals:
    void messageReady(const QString& sender, const QString& text);
    void statusReady(const QString& text, const QString& color);
    void tokenGenerated(const QString& piece); // Req 2: Real-time UI streaming

protected:
    void closeEvent(QCloseEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;

private slots:
    void onSendCommand();
    void onHistoryUp();
    void onHistoryDown();
    void onClearChat();
    void onCopyLast();
    void onSettingsClicked();
    void onHelpClicked();
    void onPresetClicked(const QString& preset);
    void onPresetChanged(int index); // Req 1: Handle combo box changes
    void updateSystemStats();
    void onTrayActivated(QSystemTrayIcon::ActivationReason reason);
    void appendMessage(const QString& sender, const QString& text);
    void setStatusText(const QString& text, const QString& color);

private:
    // ── Subsystems (owned, initialization order matters) ─────────
    // NOTE: SmartTemplateMatcher no longer takes VisionAI& — it's standalone
    // CommandRouter takes SmartTemplateMatcher& and FastComplexHandler&
    ConfigManager config_;
    DeviceProfiler profiler_;
    SafetyGuard safety_;
    WindowManager window_mgr_;
    SystemCommands sys_cmds_;
    FileManager file_mgr_;           // takes SafetyGuard&
    ContextManager context_mgr_;
    AgentMemory agent_memory_;
    UserBehaviorTracker behavior_;
    WebSearch web_search_;
    SmartTemplateMatcher template_matcher_;  // standalone, no args
    FastComplexHandler fast_handler_;         // takes VisionAI&
    CommandRouter router_;                    // takes SmartTemplateMatcher&, FastComplexHandler&

    // AI subsystems (conditionally compiled, heap-allocated)
#ifdef VISION_HAS_LLM
    std::unique_ptr<LLMController> llm_controller_;
    std::unique_ptr<ReActAgent> react_agent_;
    std::unique_ptr<ActionExecutor> action_executor_;
#endif
#ifdef VISION_HAS_WHISPER
    std::unique_ptr<WhisperEngine> whisper_engine_;
    std::unique_ptr<AudioCapture> audio_capture_;
    std::atomic<bool> recording_{false};
    std::thread audio_thread_;  // Stored for safe shutdown (not detached)
#endif

    // Screen Awareness (Lazy Observer)
    std::unique_ptr<ScreenObserver> screen_observer_;

    // Vector Memory (Flat Search + AVX2)
    std::unique_ptr<VectorMemory> vector_memory_;

    // Confidence Scorer + HITL State Machine
    std::unique_ptr<ConfidenceScorer> confidence_scorer_;
    QTimer* hitl_timeout_timer_ = nullptr;  // 60s ghost state prevention

    // ── UI Widgets ───────────────────────────────────────────────
    ChatWidget* chat_widget_ = nullptr;
    QLineEdit* input_field_ = nullptr;
    QLabel* raw_status_label_{nullptr};
    QLabel* memory_label_{nullptr};
    QComboBox* preset_selector_{nullptr}; // Req 1: Dropdown for AI Persona presets
    QLabel* cpu_label_ = nullptr;
    QLabel* ram_label_ = nullptr;
    QSystemTrayIcon* tray_icon_ = nullptr;
    QTimer* stats_timer_{nullptr};

    // ── Command history ──────────────────────────────────────────
    std::deque<std::string> command_history_;
    int history_index_ = -1;
    std::string last_response_;
    mutable std::mutex state_mutex_;  // Protects command_history_ and last_response_

    // ── Hotkey ───────────────────────────────────────────────────
    int hotkey_id_ = 1;
    int kill_switch_id_ = 2;  // Ctrl+Esc emergency stop
    bool hotkey_registered_ = false;
    bool kill_switch_registered_ = false;
    QTimer* idle_timer_ = nullptr;  // LLM idle auto-unload timer
    std::thread model_load_thread_;  // Joined in destructor (C3 fix)
    QFutureWatcher<void>* cmd_watcher_ = nullptr;  // Async command watcher (PRD Fix 1)
    QFuture<void> cmd_future_;                      // Async command future

    // FIX B6: Graceful shutdown flag — ALL background threads must check
    // this before accessing `this` or any member pointers.
    // Set FIRST in destructor, before joining any threads.
    std::atomic<bool> is_shutting_down_{false};

    // ── Setup methods ────────────────────────────────────────────
    void setupUI();
    void setupTrayIcon();
    void setupHotkey();
    void loadModels();

    // ── Voice processing ─────────────────────────────────────────
    void startRecording();
    void stopAndProcess();
    void processAudio();

    // ── UI helpers ───────────────────────────────────────────────
    void minimizeToTray();
    void showFromTray();
    void showHelpDialog();
    void showSettingsDialog();
    void setStartup(bool enable);

    // ── Open app/URL (public — used by ActionExecutor) ─────────
public:
    bool openApp(const std::string& name);
    bool openUrl(const std::string& url, const std::string& browser = "");
};

} // namespace vision
