#pragma once
/**
 * @file model_downloader_wizard.h
 * @brief First-run LLM model download wizard
 *
 * On startup, if no LLM model is configured (or the file is missing),
 * this wizard presents hardware-aware model recommendations and downloads
 * the selected GGUF model from HuggingFace with a live progress bar.
 */

#include <QDialog>
#include <QFrame>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QButtonGroup>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QFile>

#include "device_profiler.h"
#include "config_manager.h"

namespace vision {

// ── Model descriptor ────────────────────────────────────────────────────────
struct ModelOption {
    QString name;           // e.g. "Qwen2.5-1.5B-Instruct"
    QString description;    // Short description of the model
    QString params;         // e.g. "1.5B parameters"
    QString file_size;      // e.g. "~1.1 GB"
    QString filename;       // e.g. "qwen2.5-1.5b-instruct-q4_k_m.gguf"
    QString url;            // Direct HuggingFace download URL
    DeviceProfiler::Tier recommended_tier;
};

// ── Wizard Dialog ───────────────────────────────────────────────────────────
class ModelDownloaderWizard : public QDialog {
    Q_OBJECT
public:
    explicit ModelDownloaderWizard(DeviceProfiler& profiler,
                                   ConfigManager&  config,
                                   QWidget*        parent = nullptr);
    ~ModelDownloaderWizard() override;

    /// Check whether the wizard needs to run (model missing or unconfigured)
    static bool isModelNeeded(ConfigManager& config);

    /// Returns the path to the downloaded model (empty if cancelled)
    QString downloadedModelPath() const { return downloaded_path_; }

private slots:
    void onDownloadClicked();
    void onCancelClicked();
    void onSkipClicked();
    void onDownloadProgress(qint64 received, qint64 total);
    void onDownloadReadyRead();
    void onDownloadFinished();
    void onNetworkError(QNetworkReply::NetworkError code);
    void onSelectionChanged(int id);

private:
    void buildUI();
    QFrame* createModelCard(int index, const ModelOption& opt, bool recommended);
    void styleCard(QFrame* card, bool selected, bool recommended);
    void setDownloading(bool active);
    void showError(const QString& title, const QString& message);
    QString modelsDir() const;

    // ── Data ─────────────────────────────────────────────────────
    DeviceProfiler& profiler_;
    ConfigManager&  config_;
    std::vector<ModelOption> models_;
    int selected_index_ = 0;

    // ── Network ──────────────────────────────────────────────────
    QNetworkAccessManager* net_manager_ = nullptr;
    QNetworkReply*         active_reply_ = nullptr;
    QFile*                 output_file_  = nullptr;
    QString                downloaded_path_;

    // ── UI Widgets ───────────────────────────────────────────────
    QLabel*        hw_info_label_   = nullptr;
    QButtonGroup*  model_group_     = nullptr;
    QFrame*        cards_[3]        = {};
    QLabel*        card_labels_[3]  = {};
    QProgressBar*  progress_bar_    = nullptr;
    QLabel*        progress_label_  = nullptr;
    QPushButton*   download_btn_    = nullptr;
    QPushButton*   cancel_btn_      = nullptr;
    QPushButton*   skip_btn_        = nullptr;
};

// ── Public entry point ──────────────────────────────────────────────────────
/**
 * Called from main() before constructing VisionAI.
 * If a model is already configured and the file exists → returns immediately.
 * Otherwise shows the download wizard.
 */
void runModelDownloadIfNeeded(DeviceProfiler& profiler,
                               ConfigManager&  config,
                               QWidget*        parent = nullptr);

} // namespace vision
