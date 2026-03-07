#pragma once
/**
 * @file settings_dialog.h
 * @brief App Settings Dialog — API Key, Engine Selection, Preferences
 *
 * Features:
 *   - API key input (password-masked, DPAPI encrypted on save)
 *   - Engine selector (Local / Cloud / Hybrid)
 *   - Cloud model picker (Groq models)
 *   - Local model path browser
 *   - Startup toggle + hotkey config
 *   - Discord dark theme (matches main app)
 *
 * Security:
 *   - API keys encrypted at rest via Windows CryptProtectData
 *   - Keys never stored in plain text JSON
 *   - Secure memory wipe (volatile char* overwrite) on dialog close
 */

#include <QDialog>
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <string>
#include <vector>

namespace vision {

class ConfigManager;

class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(ConfigManager& config, QWidget* parent = nullptr);
    ~SettingsDialog() override;

    /// Was the config modified?
    bool wasModified() const { return modified_; }

signals:
    /// Emitted when API key changes (to update CloudBackend in real-time)
    void apiKeyChanged(const QString& key);
    /// Emitted when engine mode changes
    void engineChanged(const QString& mode);

private slots:
    void onSave();
    void onCancel();
    void onEngineChanged(int index);
    void onBrowseModel();
    void onTestApiKey();

private:
    ConfigManager& config_;
    bool modified_ = false;

    // ── UI Widgets ─────────────────────────────────
    // Engine
    QComboBox*  engine_combo_ = nullptr;
    QLabel*     engine_desc_ = nullptr;

    // Cloud section
    QLineEdit*  api_key_edit_ = nullptr;
    QPushButton* test_key_btn_ = nullptr;
    QComboBox*  cloud_model_combo_ = nullptr;
    QLabel*     api_key_status_ = nullptr;

    // Local section
    QLineEdit*  model_path_edit_ = nullptr;
    QPushButton* browse_btn_ = nullptr;
    QSpinBox*   gpu_layers_spin_ = nullptr;
    QSpinBox*   context_spin_ = nullptr;

    // General
    QCheckBox*  startup_check_ = nullptr;
    QComboBox*  hotkey_combo_ = nullptr;

    // Buttons
    QPushButton* save_btn_ = nullptr;
    QPushButton* cancel_btn_ = nullptr;

    void setupUI();
    void loadFromConfig();
    void updateVisibility();

    // ── DPAPI Encryption ───────────────────────────
    static std::string encryptDPAPI(const std::string& plaintext);
    static std::string decryptDPAPI(const std::string& ciphertext_b64);
    static std::string toBase64(const std::vector<uint8_t>& data);
    static std::vector<uint8_t> fromBase64(const std::string& b64);
};

} // namespace vision
