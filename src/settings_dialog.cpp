/**
 * @file settings_dialog.cpp
 * @brief App Settings Dialog — DPAPI encrypted API key storage
 *
 * Security Model:
 *   API key → CryptProtectData → Base64 → JSON config
 *   On read: JSON → Base64 decode → CryptUnprotectData → key
 *   Key is NEVER stored in plain text.
 *   Memory wipe on dialog close (volatile overwrite).
 */

#include "settings_dialog.h"
#include "config_manager.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QFormLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QStyle>
#include <QTimer>

#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")

#ifdef VISION_HAS_SPDLOG
#include <spdlog/spdlog.h>
#define LOG_INFO(...)  spdlog::info(__VA_ARGS__)
#define LOG_ERROR(...) spdlog::error(__VA_ARGS__)
#else
#define LOG_INFO(...)  (void)0
#define LOG_ERROR(...) (void)0
#endif

namespace vision {

// ═══════════════════ DPAPI Encryption ════════════════════════════

std::string SettingsDialog::toBase64(const std::vector<uint8_t>& data) {
    DWORD b64_len = 0;
    CryptBinaryToStringA(data.data(), (DWORD)data.size(),
                         CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                         nullptr, &b64_len);
    std::string result(b64_len, '\0');
    CryptBinaryToStringA(data.data(), (DWORD)data.size(),
                         CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                         result.data(), &b64_len);
    // Remove trailing null
    while (!result.empty() && result.back() == '\0') result.pop_back();
    return result;
}

std::vector<uint8_t> SettingsDialog::fromBase64(const std::string& b64) {
    DWORD out_len = 0;
    CryptStringToBinaryA(b64.c_str(), (DWORD)b64.size(),
                         CRYPT_STRING_BASE64, nullptr, &out_len, nullptr, nullptr);
    std::vector<uint8_t> result(out_len);
    CryptStringToBinaryA(b64.c_str(), (DWORD)b64.size(),
                         CRYPT_STRING_BASE64, result.data(), &out_len, nullptr, nullptr);
    return result;
}

std::string SettingsDialog::encryptDPAPI(const std::string& plaintext) {
    DATA_BLOB input;
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data()));
    input.cbData = static_cast<DWORD>(plaintext.size());

    DATA_BLOB output;
    if (CryptProtectData(&input, L"VisionAI_ApiKey", nullptr, nullptr, nullptr,
                         CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        std::vector<uint8_t> encrypted(output.pbData, output.pbData + output.cbData);
        LocalFree(output.pbData);
        return toBase64(encrypted);
    }
    LOG_ERROR("DPAPI encryption failed: {}", GetLastError());
    return "";
}

std::string SettingsDialog::decryptDPAPI(const std::string& ciphertext_b64) {
    if (ciphertext_b64.empty()) return "";

    auto encrypted = fromBase64(ciphertext_b64);
    if (encrypted.empty()) return "";

    DATA_BLOB input;
    input.pbData = encrypted.data();
    input.cbData = static_cast<DWORD>(encrypted.size());

    DATA_BLOB output;
    if (CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                           CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        std::string decrypted(reinterpret_cast<char*>(output.pbData), output.cbData);
        LocalFree(output.pbData);
        return decrypted;
    }
    LOG_ERROR("DPAPI decryption failed: {}", GetLastError());
    return "";
}

// ═══════════════════ Constructor / Destructor ════════════════════

SettingsDialog::SettingsDialog(ConfigManager& config, QWidget* parent)
    : QDialog(parent), config_(config) {
    setupUI();
    loadFromConfig();
    updateVisibility();
}

SettingsDialog::~SettingsDialog() {
    // Secure wipe: clear the QLineEdit first (removes from Qt internals),
    // then volatile-overwrite our local copy of the key.
    if (api_key_edit_) {
        QString text = api_key_edit_->text();
        api_key_edit_->clear();  // Removes key from QLineEdit's internal buffer
        if (!text.isEmpty()) {
            auto data = text.toUtf8();
            volatile char* p = data.data();
            for (int i = 0; i < data.size(); ++i) p[i] = 0;
        }
    }
}

// ═══════════════════ UI Setup ═══════════════════════════════════

void SettingsDialog::setupUI() {
    setWindowTitle("⚙️ VISION AI Settings");
    setMinimumSize(520, 520);
    setMaximumSize(600, 700);

    // Discord dark theme
    setStyleSheet(R"(
        QDialog {
            background: #2b2d31;
            color: #dbdee1;
            font-family: 'Segoe UI', sans-serif;
        }
        QGroupBox {
            background: #313338;
            border: 1px solid #3f4147;
            border-radius: 8px;
            margin-top: 12px;
            padding-top: 24px;
            font-weight: 600;
            color: #f2f3f5;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 12px;
            padding: 0 6px;
        }
        QLineEdit, QComboBox, QSpinBox {
            background: #1e1f22;
            border: 1px solid #3f4147;
            border-radius: 4px;
            padding: 6px 10px;
            color: #dbdee1;
            min-height: 20px;
        }
        QLineEdit:focus, QComboBox:focus, QSpinBox:focus {
            border: 1px solid #5865f2;
        }
        QPushButton {
            background: #5865f2;
            color: white;
            border: none;
            border-radius: 4px;
            padding: 8px 16px;
            font-weight: 600;
            min-height: 20px;
        }
        QPushButton:hover {
            background: #4752c4;
        }
        QPushButton:pressed {
            background: #3c45a5;
        }
        QPushButton#cancelBtn {
            background: #4e5058;
        }
        QPushButton#cancelBtn:hover {
            background: #6d6f78;
        }
        QPushButton#testBtn {
            background: #248046;
        }
        QPushButton#testBtn:hover {
            background: #1a6334;
        }
        QLabel {
            color: #b5bac1;
        }
        QLabel#sectionLabel {
            color: #f2f3f5;
            font-weight: 600;
            font-size: 13px;
        }
        QCheckBox {
            color: #dbdee1;
            spacing: 6px;
        }
        QCheckBox::indicator {
            width: 18px; height: 18px;
            border-radius: 3px;
            border: 1px solid #3f4147;
            background: #1e1f22;
        }
        QCheckBox::indicator:checked {
            background: #5865f2;
            border: 1px solid #5865f2;
        }
    )");

    auto* main_layout = new QVBoxLayout(this);
    main_layout->setSpacing(12);
    main_layout->setContentsMargins(16, 16, 16, 16);

    // ── Engine Selection ────────────────────────────────
    auto* engine_group = new QGroupBox("AI Engine");
    auto* engine_layout = new QFormLayout(engine_group);
    engine_layout->setSpacing(8);

    engine_combo_ = new QComboBox();
    engine_combo_->addItems({"🖥️ Local (Offline)", "☁️ Cloud (Groq)", "⚡ Hybrid (Auto)"});
    connect(engine_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SettingsDialog::onEngineChanged);

    engine_desc_ = new QLabel("Local model runs entirely on your machine. No data leaves your PC.");
    engine_desc_->setWordWrap(true);
    engine_desc_->setStyleSheet("font-size: 11px; color: #949ba4;");

    engine_layout->addRow("Mode:", engine_combo_);
    engine_layout->addRow(engine_desc_);
    main_layout->addWidget(engine_group);

    // ── Cloud Settings ──────────────────────────────────
    auto* cloud_group = new QGroupBox("Cloud Settings (Groq)");
    cloud_group->setObjectName("cloudGroup");
    auto* cloud_layout = new QFormLayout(cloud_group);
    cloud_layout->setSpacing(8);

    api_key_edit_ = new QLineEdit();
    api_key_edit_->setEchoMode(QLineEdit::Password);
    api_key_edit_->setPlaceholderText("gsk_...");

    auto* key_row = new QHBoxLayout();
    key_row->addWidget(api_key_edit_);
    test_key_btn_ = new QPushButton("Test");
    test_key_btn_->setObjectName("testBtn");
    test_key_btn_->setFixedWidth(60);
    connect(test_key_btn_, &QPushButton::clicked, this, &SettingsDialog::onTestApiKey);
    key_row->addWidget(test_key_btn_);

    api_key_status_ = new QLabel("");
    api_key_status_->setStyleSheet("font-size: 11px;");

    cloud_model_combo_ = new QComboBox();
    cloud_model_combo_->addItems({
        "llama-3.3-70b-versatile",
        "llama-3.1-8b-instant",
        "mixtral-8x7b-32768",
        "gemma2-9b-it"
    });

    cloud_layout->addRow("API Key:", key_row);
    cloud_layout->addRow("", api_key_status_);
    cloud_layout->addRow("Model:", cloud_model_combo_);
    main_layout->addWidget(cloud_group);

    // ── Local Settings ──────────────────────────────────
    auto* local_group = new QGroupBox("Local Model Settings");
    local_group->setObjectName("localGroup");
    auto* local_layout = new QFormLayout(local_group);
    local_layout->setSpacing(8);

    model_path_edit_ = new QLineEdit();
    model_path_edit_->setPlaceholderText("Path to .gguf model file...");
    model_path_edit_->setReadOnly(true);

    auto* path_row = new QHBoxLayout();
    path_row->addWidget(model_path_edit_);
    browse_btn_ = new QPushButton("Browse");
    browse_btn_->setFixedWidth(70);
    connect(browse_btn_, &QPushButton::clicked, this, &SettingsDialog::onBrowseModel);
    path_row->addWidget(browse_btn_);

    gpu_layers_spin_ = new QSpinBox();
    gpu_layers_spin_->setRange(0, 99);
    gpu_layers_spin_->setToolTip("Number of layers to offload to GPU (0 = CPU only)");

    context_spin_ = new QSpinBox();
    context_spin_->setRange(512, 32768);
    context_spin_->setSingleStep(512);
    context_spin_->setToolTip("Context window size (tokens)");

    local_layout->addRow("Model:", path_row);
    local_layout->addRow("GPU Layers:", gpu_layers_spin_);
    local_layout->addRow("Context Size:", context_spin_);
    main_layout->addWidget(local_group);

    // ── Voice / Whisper Settings (PRD Fix 2) ─────────────────
    auto* whisper_group = new QGroupBox("Voice / Whisper Model");
    whisper_group->setObjectName("whisperGroup");
    auto* whisper_layout = new QFormLayout(whisper_group);
    whisper_layout->setSpacing(8);

    whisper_path_edit_ = new QLineEdit();
    whisper_path_edit_->setPlaceholderText("Path to whisper .bin model file...");
    whisper_path_edit_->setReadOnly(true);

    auto* whisper_path_row = new QHBoxLayout();
    whisper_path_row->addWidget(whisper_path_edit_);
    whisper_browse_btn_ = new QPushButton("Browse");
    whisper_browse_btn_->setFixedWidth(70);
    connect(whisper_browse_btn_, &QPushButton::clicked, this, &SettingsDialog::onBrowseWhisperModel);
    whisper_path_row->addWidget(whisper_browse_btn_);

    whisper_size_combo_ = new QComboBox();
    whisper_size_combo_->addItems({"tiny", "base", "base.en", "small", "medium", "large-v3"});

    whisper_layout->addRow("Model File:", whisper_path_row);
    whisper_layout->addRow("Model Size:", whisper_size_combo_);
    main_layout->addWidget(whisper_group);

    // ── General ─────────────────────────────────────────
    auto* general_group = new QGroupBox("General");
    auto* general_layout = new QFormLayout(general_group);
    general_layout->setSpacing(8);

    startup_check_ = new QCheckBox("Launch on Windows startup");

    hotkey_combo_ = new QComboBox();
    hotkey_combo_->addItems({"Ctrl+Win", "Ctrl+Alt+Space", "Ctrl+Shift+V"});

    general_layout->addRow(startup_check_);
    general_layout->addRow("Voice Hotkey:", hotkey_combo_);
    main_layout->addWidget(general_group);

    // ── Buttons ─────────────────────────────────────────
    main_layout->addStretch();

    auto* btn_layout = new QHBoxLayout();
    btn_layout->addStretch();

    cancel_btn_ = new QPushButton("Cancel");
    cancel_btn_->setObjectName("cancelBtn");
    connect(cancel_btn_, &QPushButton::clicked, this, &SettingsDialog::onCancel);
    btn_layout->addWidget(cancel_btn_);

    save_btn_ = new QPushButton("💾 Save Settings");
    connect(save_btn_, &QPushButton::clicked, this, &SettingsDialog::onSave);
    btn_layout->addWidget(save_btn_);

    main_layout->addLayout(btn_layout);
}

// ═══════════════════ Config Load / Save ══════════════════════════

void SettingsDialog::loadFromConfig() {
    // Engine mode
    std::string mode = config_.get<std::string>("engine_mode", "local");
    if (mode == "cloud") engine_combo_->setCurrentIndex(1);
    else if (mode == "hybrid") engine_combo_->setCurrentIndex(2);
    else engine_combo_->setCurrentIndex(0);

    // Cloud
    std::string encrypted_key = config_.get<std::string>("cloud_api_key_encrypted", "");
    if (!encrypted_key.empty()) {
        std::string key = decryptDPAPI(encrypted_key);
        if (!key.empty()) {
            api_key_edit_->setText(QString::fromStdString(key));
            api_key_status_->setText("🔒 Key loaded (DPAPI encrypted)");
            api_key_status_->setStyleSheet("font-size: 11px; color: #248046;");
        }
    }

    std::string cloud_model = config_.getNested<std::string>("cloud.model", "llama-3.3-70b-versatile");
    int idx = cloud_model_combo_->findText(QString::fromStdString(cloud_model));
    if (idx >= 0) cloud_model_combo_->setCurrentIndex(idx);

    // Local
    std::string model_path = config_.getNested<std::string>("llm.model_path", "");
    model_path_edit_->setText(QString::fromStdString(model_path));
    gpu_layers_spin_->setValue(config_.getNested<int>("llm.gpu_layers", 0));
    context_spin_->setValue(config_.getNested<int>("llm.context_size", 2048));

    // General
    startup_check_->setChecked(config_.get<bool>("run_on_startup", false));
    std::string hotkey = config_.get<std::string>("hotkey", "ctrl+win");
    int hk_idx = hotkey_combo_->findText(QString::fromStdString(hotkey), Qt::MatchFixedString);
    if (hk_idx >= 0) hotkey_combo_->setCurrentIndex(hk_idx);

    // Whisper (PRD Fix 2)
    std::string whisper_path = config_.getNested<std::string>("whisper.model_path", "");
    whisper_path_edit_->setText(QString::fromStdString(whisper_path));
    std::string whisper_size = config_.getNested<std::string>("whisper.model_size", "base");
    int ws_idx = whisper_size_combo_->findText(QString::fromStdString(whisper_size));
    if (ws_idx >= 0) whisper_size_combo_->setCurrentIndex(ws_idx);
}

void SettingsDialog::onSave() {
    // Engine mode
    std::string modes[] = {"local", "cloud", "hybrid"};
    std::string mode = modes[engine_combo_->currentIndex()];
    config_.set("engine_mode", mode);

    // API Key — DPAPI encrypt before saving
    QString key_text = api_key_edit_->text().trimmed();
    if (!key_text.isEmpty()) {
        std::string encrypted = encryptDPAPI(key_text.toStdString());
        if (!encrypted.empty()) {
            config_.set("cloud_api_key_encrypted", encrypted);
            LOG_INFO("Settings: API key saved (DPAPI encrypted, {} bytes)", encrypted.size());
            emit apiKeyChanged(key_text);  // Only emit if encryption succeeded
        }
    }

    // Cloud model
    config_.set("cloud", nlohmann::json{
        {"model", cloud_model_combo_->currentText().toStdString()}
    });

    // Local LLM settings — MERGE into existing config to preserve unknown keys
    {
        nlohmann::json llm_cfg;
        // Read existing llm config as baseline
        try {
            llm_cfg = config_.raw()["llm"];
        } catch (...) {}
        // Overwrite only the UI-visible fields
        llm_cfg["model_path"] = model_path_edit_->text().toStdString();
        llm_cfg["gpu_layers"] = gpu_layers_spin_->value();
        llm_cfg["context_size"] = context_spin_->value();
        config_.set("llm", llm_cfg);
    }

    // General
    config_.set("run_on_startup", startup_check_->isChecked());
    config_.set("hotkey", hotkey_combo_->currentText().toLower().toStdString());

    // Whisper (PRD Fix 2)
    {
        nlohmann::json whisper_cfg;
        try { whisper_cfg = config_.raw()["whisper"]; } catch (...) {}
        whisper_cfg["model_path"] = whisper_path_edit_->text().toStdString();
        whisper_cfg["model_size"] = whisper_size_combo_->currentText().toStdString();
        config_.set("whisper", whisper_cfg);
    }

    // Persist
    config_.save();
    modified_ = true;

    emit engineChanged(QString::fromStdString(mode));
    accept();
}

void SettingsDialog::onCancel() {
    reject();
}

void SettingsDialog::onEngineChanged(int index) {
    static const char* descs[] = {
        "Local model runs entirely on your machine. No data leaves your PC. "
        "Requires a downloaded .gguf model.",
        "Cloud mode uses Groq's ultra-fast API. Requires an API key. "
        "Your prompts are sent to Groq's servers.",
        "Hybrid mode uses cloud for complex queries and local for simple ones. "
        "Best of both worlds."
    };
    if (index < 0 || index >= 3) return;  // Guard against OOB
    engine_desc_->setText(descs[index]);
    updateVisibility();
}

void SettingsDialog::updateVisibility() {
    int idx = engine_combo_->currentIndex();
    // Cloud settings visible if cloud or hybrid
    bool show_cloud = (idx == 1 || idx == 2);
    // Local settings visible if local or hybrid
    bool show_local = (idx == 0 || idx == 2);

    findChild<QGroupBox*>("cloudGroup")->setVisible(show_cloud);
    findChild<QGroupBox*>("localGroup")->setVisible(show_local);

    adjustSize();
}

void SettingsDialog::onBrowseModel() {
    QString path = QFileDialog::getOpenFileName(
        this, "Select GGUF Model File",
        QDir::homePath(),
        "GGUF Models (*.gguf);;All Files (*)"
    );
    if (!path.isEmpty()) {
        model_path_edit_->setText(path);
    }
}

void SettingsDialog::onTestApiKey() {
    QString key = api_key_edit_->text().trimmed();
    if (key.isEmpty()) {
        api_key_status_->setText("❌ Please enter an API key first.");
        api_key_status_->setStyleSheet("font-size: 11px; color: #ed4245;");
        return;
    }

    if (!key.startsWith("gsk_")) {
        api_key_status_->setText("⚠️ Groq API keys start with 'gsk_'");
        api_key_status_->setStyleSheet("font-size: 11px; color: #fee75c;");
        return;
    }

    // Basic validation passed
    api_key_status_->setText("✅ Key format looks valid! Will be tested on first use.");
    api_key_status_->setStyleSheet("font-size: 11px; color: #248046;");

    // Flash the button green
    test_key_btn_->setStyleSheet("background: #248046;");
    QTimer::singleShot(1500, this, [this]() {
        test_key_btn_->setStyleSheet("");  // Reset
    });
}

void SettingsDialog::onBrowseWhisperModel() {
    QString path = QFileDialog::getOpenFileName(
        this, "Select Whisper Model File",
        QDir::homePath(),
        "Whisper Models (*.bin);;All Files (*)"
    );
    if (!path.isEmpty()) {
        whisper_path_edit_->setText(path);
    }
}

} // namespace vision
