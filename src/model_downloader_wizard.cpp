/**
 * @file model_downloader_wizard.cpp
 * @brief First-run LLM model download wizard — implementation
 *
 * Presents a hardware-aware, card-based UI for selecting and downloading
 * a GGUF model from HuggingFace.  Uses chunked streaming so even multi-GB
 * models never fill RAM.
 */

#include "model_downloader_wizard.h"

#include <QApplication>
#include <QMessageBox>
#include <QGraphicsDropShadowEffect>
#include <QPropertyAnimation>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStyle>
#include <QTimer>

#include <filesystem>

#ifdef VISION_HAS_SPDLOG
#include <spdlog/spdlog.h>
#define LOG_INFO(...)  spdlog::info(__VA_ARGS__)
#define LOG_WARN(...)  spdlog::warn(__VA_ARGS__)
#define LOG_ERROR(...) spdlog::error(__VA_ARGS__)
#else
#define LOG_INFO(...)  (void)0
#define LOG_WARN(...)  (void)0
#define LOG_ERROR(...) (void)0
#endif

namespace fs = std::filesystem;

namespace vision {

// ═══════════════════ Static helpers ═══════════════════

static QString humanSize(qint64 bytes) {
    if (bytes < 1024)              return QString::number(bytes) + " B";
    if (bytes < 1024 * 1024)       return QString::number(bytes / 1024.0, 'f', 1) + " KB";
    if (bytes < 1024LL * 1024 * 1024)
        return QString::number(bytes / (1024.0 * 1024.0), 'f', 1) + " MB";
    return QString::number(bytes / (1024.0 * 1024.0 * 1024.0), 'f', 2) + " GB";
}

// ═══════════════════ isModelNeeded ═══════════════════

bool ModelDownloaderWizard::isModelNeeded(ConfigManager& config) {
    config.load();
    std::string path = config.getNested<std::string>("llm.model_path", "");
    if (path.empty()) return true;
    return !fs::exists(path);
}

// ═══════════════════ Constructor / Destructor ═══════════════════

ModelDownloaderWizard::ModelDownloaderWizard(DeviceProfiler& profiler,
                                             ConfigManager&  config,
                                             QWidget*        parent)
    : QDialog(parent)
    , profiler_(profiler)
    , config_(config)
    , net_manager_(new QNetworkAccessManager(this))
{
    // ── Populate model options ────────────────────────────────────
    models_ = {
        {
            "Qwen2.5-1.5B-Instruct",
            "Compact & fast — ideal for low-end systems. "
            "Great for simple tasks, quick commands, and basic reasoning.",
            "1.5B parameters",
            "~1.1 GB",
            "qwen2.5-1.5b-instruct-q4_k_m.gguf",
            "https://huggingface.co/Qwen/Qwen2.5-1.5B-Instruct-GGUF/resolve/main/"
            "qwen2.5-1.5b-instruct-q4_k_m.gguf",
            DeviceProfiler::Tier::Low
        },
        {
            "Llama-3.2-3B-Instruct",
            "Balanced performance — best for most systems. "
            "Strong reasoning, tool use, and multi-step planning.",
            "3B parameters",
            "~2.0 GB",
            "Llama-3.2-3B-Instruct-Q4_K_M.gguf",
            "https://huggingface.co/bartowski/Llama-3.2-3B-Instruct-GGUF/resolve/main/"
            "Llama-3.2-3B-Instruct-Q4_K_M.gguf",
            DeviceProfiler::Tier::Mid
        },
        {
            "Mistral-7B-Instruct-v0.3",
            "Maximum capability — for powerful hardware. "
            "Best reasoning, complex tool chains, and code generation.",
            "7B parameters",
            "~4.1 GB",
            "Mistral-7B-Instruct-v0.3.Q4_K_M.gguf",
            "https://huggingface.co/MaziyarPanahi/Mistral-7B-Instruct-v0.3-GGUF/resolve/main/"
            "Mistral-7B-Instruct-v0.3.Q4_K_M.gguf",
            DeviceProfiler::Tier::High
        }
    };

    // Pre-select the recommended model
    auto tier = profiler_.getTier();
    for (int i = 0; i < (int)models_.size(); ++i) {
        if (models_[i].recommended_tier == tier) {
            selected_index_ = i;
            break;
        }
    }

    buildUI();
}

ModelDownloaderWizard::~ModelDownloaderWizard() {
    if (active_reply_) {
        active_reply_->abort();
        active_reply_->deleteLater();
        active_reply_ = nullptr;
    }
    if (output_file_) {
        output_file_->close();
        delete output_file_;
        output_file_ = nullptr;
    }
}

// ═══════════════════ UI Construction ═══════════════════

void ModelDownloaderWizard::buildUI() {
    setWindowTitle("VISION AI — Model Setup");
    setFixedSize(620, 680);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    setStyleSheet("QDialog { background-color: #1a1a1a; }");

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(28, 24, 28, 20);
    root->setSpacing(12);

    // ── Title ────────────────────────────────────────────────────
    auto* title = new QLabel("🧠 AI Model Setup");
    title->setStyleSheet("font-size: 22px; font-weight: bold; color: #7B68EE;");
    title->setAlignment(Qt::AlignCenter);
    root->addWidget(title);

    auto* subtitle = new QLabel(
        "VISION AI needs a language model to power its intelligence.\n"
        "Select a model below based on your hardware — we'll download it for you.");
    subtitle->setStyleSheet("font-size: 13px; color: #aaa; padding: 0 10px;");
    subtitle->setAlignment(Qt::AlignCenter);
    subtitle->setWordWrap(true);
    root->addWidget(subtitle);

    // ── Hardware info bar ─────────────────────────────────────────
    auto* hw_frame = new QFrame();
    hw_frame->setStyleSheet(
        "QFrame { background: #252525; border: 1px solid #3a3a3a; "
        "border-radius: 8px; padding: 10px; }");
    auto* hw_layout = new QHBoxLayout(hw_frame);
    hw_layout->setContentsMargins(12, 6, 12, 6);

    int ram_mb  = profiler_.getRAMTotalMB();
    int vram_mb = profiler_.getGPUMemoryMB();
    QString gpu = QString::fromStdString(profiler_.getGPUName());
    QString tier_str = QString::fromStdString(profiler_.getTierString());

    hw_info_label_ = new QLabel(
        QString("💻 <b>RAM:</b> %1 GB  •  <b>GPU:</b> %2  •  <b>VRAM:</b> %3 MB  •  "
                "<b>Tier:</b> <span style='color:#7B68EE;'>%4</span>")
        .arg(ram_mb / 1024.0, 0, 'f', 1)
        .arg(gpu.isEmpty() ? "Integrated / Unknown" : gpu)
        .arg(vram_mb)
        .arg(tier_str));
    hw_info_label_->setStyleSheet("color: #ccc; font-size: 12px;");
    hw_info_label_->setTextFormat(Qt::RichText);
    hw_layout->addWidget(hw_info_label_);
    root->addWidget(hw_frame);

    // ── Model selection cards ────────────────────────────────────
    model_group_ = new QButtonGroup(this);
    model_group_->setExclusive(true);
    connect(model_group_, &QButtonGroup::idClicked,
            this, &ModelDownloaderWizard::onSelectionChanged);

    auto tier = profiler_.getTier();
    for (int i = 0; i < (int)models_.size(); ++i) {
        bool is_rec = (models_[i].recommended_tier == tier);
        cards_[i] = createModelCard(i, models_[i], is_rec);
        root->addWidget(cards_[i]);
    }

    // ── Progress area ────────────────────────────────────────────
    progress_label_ = new QLabel("");
    progress_label_->setStyleSheet("color: #aaa; font-size: 11px;");
    progress_label_->setVisible(false);
    root->addWidget(progress_label_);

    progress_bar_ = new QProgressBar();
    progress_bar_->setRange(0, 100);
    progress_bar_->setValue(0);
    progress_bar_->setVisible(false);
    progress_bar_->setStyleSheet(
        "QProgressBar { background: #2a2a2a; border: 1px solid #444; "
        "border-radius: 6px; height: 22px; text-align: center; color: white; font-size: 11px; }"
        "QProgressBar::chunk { background: qlineargradient("
        "x1:0,y1:0,x2:1,y2:0, stop:0 #7B68EE, stop:1 #5B4FCF); border-radius: 5px; }");
    root->addWidget(progress_bar_);

    root->addStretch();

    // ── Button row ───────────────────────────────────────────────
    auto* btn_row = new QHBoxLayout();

    skip_btn_ = new QPushButton("Skip for Now");
    skip_btn_->setStyleSheet(
        "QPushButton { background: transparent; border: 1px solid #555; "
        "border-radius: 8px; padding: 8px 18px; color: #888; font-size: 13px; }"
        "QPushButton:hover { border-color: #999; color: #ccc; }");
    connect(skip_btn_, &QPushButton::clicked, this, &ModelDownloaderWizard::onSkipClicked);

    cancel_btn_ = new QPushButton("Cancel Download");
    cancel_btn_->setVisible(false);
    cancel_btn_->setStyleSheet(
        "QPushButton { background: #3a2020; border: 1px solid #cc4444; "
        "border-radius: 8px; padding: 8px 18px; color: #ff6666; font-size: 13px; }"
        "QPushButton:hover { background: #4a2525; }");
    connect(cancel_btn_, &QPushButton::clicked, this, &ModelDownloaderWizard::onCancelClicked);

    download_btn_ = new QPushButton("⬇  Download Selected Model");
    download_btn_->setStyleSheet(
        "QPushButton { background: qlineargradient("
        "x1:0,y1:0,x2:1,y2:0, stop:0 #7B68EE, stop:1 #6A5ACD); "
        "border: none; border-radius: 8px; padding: 10px 28px; "
        "color: white; font-size: 14px; font-weight: bold; }"
        "QPushButton:hover { background: qlineargradient("
        "x1:0,y1:0,x2:1,y2:0, stop:0 #8B7BFF, stop:1 #7B6BDD); }"
        "QPushButton:disabled { background: #444; color: #888; }");
    connect(download_btn_, &QPushButton::clicked,
            this, &ModelDownloaderWizard::onDownloadClicked);

    btn_row->addWidget(skip_btn_);
    btn_row->addStretch();
    btn_row->addWidget(cancel_btn_);
    btn_row->addWidget(download_btn_);
    root->addLayout(btn_row);
}

QFrame* ModelDownloaderWizard::createModelCard(int index,
                                                const ModelOption& opt,
                                                bool recommended) {
    auto* card = new QFrame();
    card->setCursor(Qt::PointingHandCursor);

    auto* layout = new QHBoxLayout(card);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(12);

    // Radio button
    auto* radio = new QRadioButton();
    radio->setStyleSheet("QRadioButton::indicator { width: 16px; height: 16px; }");
    model_group_->addButton(radio, index);
    if (index == selected_index_) radio->setChecked(true);
    layout->addWidget(radio);

    // Text content
    auto* text_col = new QVBoxLayout();
    text_col->setSpacing(3);

    // Title row with recommended badge
    auto* title_row = new QHBoxLayout();
    auto* name_lbl = new QLabel(
        QString("<b style='font-size:14px; color:white;'>%1</b>"
                "  <span style='color:#888; font-size:11px;'>%2  •  %3</span>")
        .arg(opt.name, opt.params, opt.file_size));
    name_lbl->setTextFormat(Qt::RichText);
    title_row->addWidget(name_lbl);

    if (recommended) {
        auto* badge = new QLabel("⭐ Recommended");
        badge->setStyleSheet(
            "background: #2d1f5e; color: #a78bfa; font-size: 10px; "
            "font-weight: bold; padding: 2px 8px; border-radius: 4px;");
        title_row->addWidget(badge);
    }
    title_row->addStretch();
    text_col->addLayout(title_row);

    auto* desc_lbl = new QLabel(opt.description);
    desc_lbl->setStyleSheet("color: #999; font-size: 11px;");
    desc_lbl->setWordWrap(true);
    text_col->addWidget(desc_lbl);

    layout->addLayout(text_col, 1);

    // Apply initial style
    styleCard(card, index == selected_index_, recommended);

    return card;
}

void ModelDownloaderWizard::styleCard(QFrame* card, bool selected, bool recommended) {
    QString border_color = selected ? "#7B68EE" : (recommended ? "#3d3360" : "#333");
    QString bg = selected ? "#252040" : "#222";
    card->setStyleSheet(
        QString("QFrame { background: %1; border: %2px solid %3; "
                "border-radius: 10px; }")
        .arg(bg)
        .arg(selected ? 2 : 1)
        .arg(border_color));
}

// ═══════════════════ Slots ═══════════════════

void ModelDownloaderWizard::onSelectionChanged(int id) {
    selected_index_ = id;
    auto tier = profiler_.getTier();
    for (int i = 0; i < (int)models_.size(); ++i) {
        bool is_rec = (models_[i].recommended_tier == tier);
        styleCard(cards_[i], i == id, is_rec);
    }
}

void ModelDownloaderWizard::onSkipClicked() {
    LOG_WARN("User skipped model download wizard");
    reject();
}

void ModelDownloaderWizard::onCancelClicked() {
    if (active_reply_) {
        active_reply_->abort();
    }
    // Cleanup partial file
    if (output_file_) {
        QString path = output_file_->fileName();
        output_file_->close();
        delete output_file_;
        output_file_ = nullptr;
        QFile::remove(path);
        LOG_INFO("Cancelled — removed partial file: {}", path.toStdString());
    }
    setDownloading(false);
    progress_bar_->setValue(0);
    progress_label_->setText("Download cancelled.");
}

void ModelDownloaderWizard::onDownloadClicked() {
    if (selected_index_ < 0 || selected_index_ >= (int)models_.size()) return;

    const auto& model = models_[selected_index_];

    // Ensure target directory exists
    QString dir = modelsDir();
    QDir().mkpath(dir);

    QString file_path = dir + "/" + model.filename;

    // Check if file already exists and is non-empty
    QFileInfo fi(file_path);
    if (fi.exists() && fi.size() > 1024) {
        auto answer = QMessageBox::question(this, "Model Already Exists",
            QString("The file '%1' already exists (%2).\n\nUse this file?")
            .arg(model.filename, humanSize(fi.size())),
            QMessageBox::Yes | QMessageBox::No);
        if (answer == QMessageBox::Yes) {
            // Save to config and finish
            downloaded_path_ = file_path;
            nlohmann::json llm_cfg = config_.getNested<nlohmann::json>("llm", {});
            llm_cfg["model_path"] = file_path.toStdString();
            config_.set("llm", llm_cfg);
            config_.save();
            LOG_INFO("Using existing model: {}", file_path.toStdString());
            accept();
            return;
        }
        // Otherwise re-download (overwrite)
    }

    // Open file for writing
    output_file_ = new QFile(file_path);
    if (!output_file_->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        showError("File Error",
                  QString("Cannot write to:\n%1\n\n%2")
                  .arg(file_path, output_file_->errorString()));
        delete output_file_;
        output_file_ = nullptr;
        return;
    }

    // Start download
    setDownloading(true);
    progress_label_->setText(
        QString("Connecting to HuggingFace — downloading %1 ...").arg(model.name));
    LOG_INFO("Starting download: {} → {}", model.url.toStdString(),
             file_path.toStdString());

    QNetworkRequest request(QUrl(model.url));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader, "VISION-AI/3.0");

    active_reply_ = net_manager_->get(request);

    connect(active_reply_, &QNetworkReply::downloadProgress,
            this, &ModelDownloaderWizard::onDownloadProgress);
    connect(active_reply_, &QNetworkReply::readyRead,
            this, &ModelDownloaderWizard::onDownloadReadyRead);
    connect(active_reply_, &QNetworkReply::finished,
            this, &ModelDownloaderWizard::onDownloadFinished);
    connect(active_reply_, &QNetworkReply::errorOccurred,
            this, &ModelDownloaderWizard::onNetworkError);
}

void ModelDownloaderWizard::onDownloadProgress(qint64 received, qint64 total) {
    if (total > 0) {
        int pct = static_cast<int>((received * 100) / total);
        progress_bar_->setValue(pct);
        progress_label_->setText(
            QString("Downloading %1  —  %2 / %3  (%4%)")
            .arg(models_[selected_index_].name,
                 humanSize(received), humanSize(total))
            .arg(pct));
    } else {
        // Unknown total — show bytes received
        progress_bar_->setRange(0, 0);  // Indeterminate
        progress_label_->setText(
            QString("Downloading %1  —  %2 received...")
            .arg(models_[selected_index_].name, humanSize(received)));
    }
}

void ModelDownloaderWizard::onDownloadReadyRead() {
    if (active_reply_ && output_file_) {
        QByteArray data = active_reply_->readAll();
        output_file_->write(data);
    }
}

void ModelDownloaderWizard::onDownloadFinished() {
    if (!active_reply_) return;

    bool had_error = (active_reply_->error() != QNetworkReply::NoError);
    active_reply_->deleteLater();
    active_reply_ = nullptr;

    if (output_file_) {
        output_file_->flush();
        QString path = output_file_->fileName();
        qint64 size  = output_file_->size();
        output_file_->close();
        delete output_file_;
        output_file_ = nullptr;

        if (had_error) {
            // Remove partial file on error
            QFile::remove(path);
            setDownloading(false);
            LOG_ERROR("Download failed — removed partial file: {}", path.toStdString());
            return;
        }

        if (size < 1024) {
            // Suspiciously small — likely an error page, not a model
            QFile::remove(path);
            showError("Download Error",
                      "Downloaded file is too small — the URL may be invalid.\n"
                      "Please check your internet connection and try again.");
            setDownloading(false);
            return;
        }

        // ── Success! ─────────────────────────────────────────────
        downloaded_path_ = path;

        // Save to config
        nlohmann::json llm_cfg = config_.getNested<nlohmann::json>("llm", {});
        llm_cfg["model_path"] = path.toStdString();
        config_.set("llm", llm_cfg);
        config_.save();

        LOG_INFO("Model downloaded successfully: {} ({})",
                 path.toStdString(), humanSize(size).toStdString());

        progress_bar_->setValue(100);
        progress_label_->setText(
            QString("✅ Download complete — %1 (%2)")
            .arg(models_[selected_index_].name, humanSize(size)));

        // Brief pause to show completion, then close
        QTimer::singleShot(800, this, &QDialog::accept);
    }
}

void ModelDownloaderWizard::onNetworkError(QNetworkReply::NetworkError code) {
    if (code == QNetworkReply::OperationCanceledError) return; // User cancelled
    QString msg = active_reply_ ? active_reply_->errorString()
                                : QString("Network error code: %1").arg(static_cast<int>(code));
    showError("Download Error",
              QString("Failed to download model:\n%1\n\n"
                      "Please check your internet connection and try again.").arg(msg));
    LOG_ERROR("Network error: {} (code {})", msg.toStdString(), static_cast<int>(code));
}

// ═══════════════════ Helpers ═══════════════════

void ModelDownloaderWizard::setDownloading(bool active) {
    progress_bar_->setVisible(active);
    progress_bar_->setRange(0, 100);
    progress_label_->setVisible(true); // Always visible once triggered
    download_btn_->setEnabled(!active);
    download_btn_->setVisible(!active);
    cancel_btn_->setVisible(active);
    skip_btn_->setEnabled(!active);

    // Disable model selection during download
    for (int i = 0; i < (int)models_.size(); ++i) {
        auto* radio = model_group_->button(i);
        if (radio) radio->setEnabled(!active);
    }
}

void ModelDownloaderWizard::showError(const QString& title, const QString& message) {
    QMessageBox::warning(this, title, message);
}

QString ModelDownloaderWizard::modelsDir() const {
    return QString::fromStdString(config_.getDataDir()) + "/models";
}

// ═══════════════════ Public entry point ═══════════════════

void runModelDownloadIfNeeded(DeviceProfiler& profiler,
                               ConfigManager&  config,
                               QWidget*        parent) {
    if (!ModelDownloaderWizard::isModelNeeded(config)) {
        LOG_INFO("LLM model found — skipping download wizard");
        return;
    }

    LOG_INFO("No LLM model configured — launching download wizard");
    ModelDownloaderWizard wizard(profiler, config, parent);
    int result = wizard.exec();

    if (result == QDialog::Accepted) {
        LOG_INFO("Model download wizard completed: {}",
                 wizard.downloadedModelPath().toStdString());
    } else {
        LOG_WARN("Model download wizard skipped — LLM will not be available");
    }
}

} // namespace vision
