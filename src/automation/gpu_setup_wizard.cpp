/**
 * @file gpu_setup_wizard.cpp
 * @brief First-run GPU backend selection wizard implementation
 */

#include "gpu_setup_wizard.h"
#include "device_profiler.h"

#include <QApplication>
#include <QEvent>
#include <QFont>
#include <QGraphicsOpacityEffect>
#include <QMessageBox>
#include <QScreen>
#include <QScrollArea>
#include <QSpacerItem>
#include <nlohmann/json.hpp>

#include <fstream>
#include <filesystem>
#include <windows.h>  // for nvidia-smi check

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace vision {

// ════════════════════════════════════════════════════════════
// GpuConfig — persistence
// ════════════════════════════════════════════════════════════

static const std::string CONFIG_PATH = "data/gpu_config.json";

std::string GpuConfig::backendName() const {
    switch (backend) {
        case GpuBackend::CUDA:   return "CUDA (NVIDIA)";
        case GpuBackend::Vulkan: return "Vulkan";
        case GpuBackend::CPU:    return "CPU Only";
    }
    return "CPU Only";
}

GpuConfig GpuConfig::load() {
    GpuConfig cfg;
    try {
        if (!fs::exists(CONFIG_PATH)) return cfg;
        std::ifstream f(CONFIG_PATH);
        json j = json::parse(f);
        std::string bk = j.value("backend", "cpu");
        if (bk == "cuda")   cfg.backend = GpuBackend::CUDA;
        else if (bk == "vulkan") cfg.backend = GpuBackend::Vulkan;
        else                cfg.backend = GpuBackend::CPU;
        cfg.gpu_name           = j.value("gpu_name", "");
        cfg.gpu_memory_mb      = j.value("gpu_memory_mb", 0);
        cfg.vendor             = j.value("vendor", "unknown");
        cfg.recommended_layers = j.value("recommended_layers", 0);
    } catch (...) {}
    return cfg;
}

void GpuConfig::save() const {
    try {
        fs::create_directories("data");
        json j;
        if (backend == GpuBackend::CUDA)        j["backend"] = "cuda";
        else if (backend == GpuBackend::Vulkan)  j["backend"] = "vulkan";
        else                                     j["backend"] = "cpu";
        j["gpu_name"]           = gpu_name;
        j["gpu_memory_mb"]      = gpu_memory_mb;
        j["vendor"]             = vendor;
        j["recommended_layers"] = recommended_layers;
        std::ofstream f(CONFIG_PATH);
        f << j.dump(4);
    } catch (...) {}
}

// ════════════════════════════════════════════════════════════
// Helper: detect CUDA availability at runtime
// ════════════════════════════════════════════════════════════
static bool cudaAvailable() {
    // nvidia-smi must exit 0 and return a name
    FILE* pipe = _popen("nvidia-smi --query-gpu=name --format=csv,noheader 2>nul", "r");
    if (!pipe) return false;
    char buf[128] = {};
    bool has_output = (fgets(buf, sizeof(buf), pipe) != nullptr);
    int ret = _pclose(pipe);
    return (ret == 0 && has_output && strlen(buf) > 2);
}

// ════════════════════════════════════════════════════════════
// GpuSetupWizard — NVIDIA-only dialog
// ════════════════════════════════════════════════════════════

GpuSetupWizard::GpuSetupWizard(const std::string& gpu_name, int gpu_mb, QWidget* parent)
    : QDialog(parent), gpu_name_(gpu_name), gpu_mb_(gpu_mb)
{
    setWindowTitle("VISION AI — GPU Setup");
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setFixedSize(760, 620);
    buildUI();

    // Center on screen
    if (auto* screen = QGuiApplication::primaryScreen()) {
        auto sg = screen->availableGeometry();
        move(sg.center() - rect().center());
    }
}

// ── Style helpers ────────────────────────────────────────────

static QString cardBase() {
    return R"(
        QFrame {
            border-radius: 14px;
            border: 2px solid #333;
            background: #1a1a2e;
            padding: 8px;
        }
    )";
}

static QString cardActive(const QString& accent) {
    return QString(R"(
        QFrame {
            border-radius: 14px;
            border: 2px solid %1;
            background: #0f0f23;
            padding: 8px;
        }
    )").arg(accent);
}

void GpuSetupWizard::styleOptionCard(QFrame* card, bool active) {
    QString accent = (card == cuda_card_) ? "#76b900" : "#ff6b35";  // NVIDIA green / Vulkan orange
    card->setStyleSheet(active ? cardActive(accent) : cardBase());
}

void GpuSetupWizard::animateSelection(QFrame* card) {
    // Simple border flash via palette
    auto* anim = new QPropertyAnimation(card, "minimumHeight", card);
    anim->setDuration(120);
    anim->setStartValue(card->minimumHeight() - 4);
    anim->setEndValue(card->minimumHeight());
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

// ── UI build ─────────────────────────────────────────────────

void GpuSetupWizard::buildUI() {
    // Root frame (the visible rounded window)
    auto* root = new QFrame(this);
    root->setGeometry(0, 0, 760, 620);
    root->setObjectName("root");
    root->setStyleSheet(R"(
        QFrame#root {
            background: qlineargradient(x1:0,y1:0,x2:1,y2:1,
                stop:0 #0d0d1a, stop:1 #1a0d2e);
            border-radius: 18px;
            border: 1px solid #333;
        }
    )");

    auto* main_layout = new QVBoxLayout(root);
    main_layout->setContentsMargins(32, 28, 32, 28);
    main_layout->setSpacing(16);

    // ── Header ────────────────────────────────────────────
    auto* header = new QLabel("🎮  GPU Setup");
    header->setStyleSheet("color: #ffffff; font-size: 26px; font-weight: 700;");

    auto* sub = new QLabel(
        QString("NVIDIA GPU detected: <b style='color:#76b900'>%1</b>  "
                "(%2 MB VRAM)<br>"
                "<span style='color:#888;font-size:13px;'>"
                "Choose the AI acceleration engine that suits you best.</span>")
        .arg(QString::fromStdString(gpu_name_))
        .arg(gpu_mb_));
    sub->setTextFormat(Qt::RichText);
    sub->setWordWrap(true);
    sub->setStyleSheet("color: #ccc; font-size: 14px;");

    main_layout->addWidget(header);
    main_layout->addWidget(sub);

    // ── Divider ───────────────────────────────────────────
    auto* divider = new QFrame;
    divider->setFrameShape(QFrame::HLine);
    divider->setStyleSheet("background:#333; max-height:1px;");
    main_layout->addWidget(divider);

    // ── Option cards (side-by-side) ───────────────────────
    auto* cards_row = new QHBoxLayout;
    cards_row->setSpacing(16);

    // — CUDA card —
    cuda_card_ = new QFrame;
    cuda_card_->setMinimumHeight(280);
    auto* cuda_layout = new QVBoxLayout(cuda_card_);
    cuda_layout->setContentsMargins(20, 20, 20, 16);
    cuda_layout->setSpacing(10);

    auto* cuda_icon   = new QLabel("⚡");  cuda_icon->setStyleSheet("font-size:36px;");
    auto* cuda_title  = new QLabel("<b>CUDA</b>  <span style='color:#76b900;font-size:12px;'>NVIDIA EXCLUSIVE</span>");
    cuda_title->setTextFormat(Qt::RichText);
    cuda_title->setStyleSheet("font-size:18px; color:#fff;");

    auto* cuda_badge = new QLabel("🏆  FASTEST AVAILABLE");
    cuda_badge->setStyleSheet("background:#1a3a00; color:#76b900; padding:4px 10px; border-radius:8px; font-size:11px; font-weight:700;");
    cuda_badge->setAlignment(Qt::AlignCenter);

    auto* cuda_pros = new QLabel(
        "✅  <b>Direct access</b> to NVIDIA hardware<br>"
        "✅  Uses <b>Tensor Cores</b> (RTX series = 3× faster)<br>"
        "✅  <b>cuBLAS</b> math — no translation overhead<br>"
        "✅  Best for <b>large LLMs</b> (7B+ models)<br>"
        "✅  <b>Ultra-low latency</b> AI responses");
    cuda_pros->setTextFormat(Qt::RichText);
    cuda_pros->setWordWrap(true);
    cuda_pros->setStyleSheet("color:#ccc; font-size:13px; line-height:1.6;");

    auto* cuda_cons = new QLabel(
        "⚠️  Requires <b>CUDA Toolkit</b> installed<br>"
        "⚠️  <b>NVIDIA-only</b> — non-portable");
    cuda_cons->setTextFormat(Qt::RichText);
    cuda_cons->setWordWrap(true);
    cuda_cons->setStyleSheet("color:#888; font-size:12px;");

    auto* cuda_radio = new QRadioButton("Select CUDA");
    cuda_radio->setStyleSheet("color:#76b900; font-weight:700; font-size:14px;");
    cuda_radio->setChecked(true);

    cuda_layout->addWidget(cuda_icon);
    cuda_layout->addWidget(cuda_title);
    cuda_layout->addWidget(cuda_badge);
    cuda_layout->addWidget(cuda_pros);
    cuda_layout->addWidget(cuda_cons);
    cuda_layout->addStretch();
    cuda_layout->addWidget(cuda_radio);
    styleOptionCard(cuda_card_, true);

    // — Vulkan card —
    vulkan_card_ = new QFrame;
    vulkan_card_->setMinimumHeight(280);
    auto* vulkan_layout = new QVBoxLayout(vulkan_card_);
    vulkan_layout->setContentsMargins(20, 20, 20, 16);
    vulkan_layout->setSpacing(10);

    auto* vk_icon   = new QLabel("🌋");  vk_icon->setStyleSheet("font-size:36px;");
    auto* vk_title  = new QLabel("<b>Vulkan</b>  <span style='color:#ff6b35;font-size:12px;'>UNIVERSAL</span>");
    vk_title->setTextFormat(Qt::RichText);
    vk_title->setStyleSheet("font-size:18px; color:#fff;");

    auto* vk_badge = new QLabel("🌍  UNIVERSAL GPU");
    vk_badge->setStyleSheet("background:#3a1a00; color:#ff6b35; padding:4px 10px; border-radius:8px; font-size:11px; font-weight:700;");
    vk_badge->setAlignment(Qt::AlignCenter);

    auto* vk_pros = new QLabel(
        "✅  Works on <b>any modern GPU</b><br>"
        "✅  <b>5–10% slower</b> than CUDA (barely noticeable)<br>"
        "✅  <b>No CUDA Toolkit</b> required<br>"
        "✅  Share your app — works on any PC<br>"
        "✅  Same llama.cpp engine, just different driver");
    vk_pros->setTextFormat(Qt::RichText);
    vk_pros->setWordWrap(true);
    vk_pros->setStyleSheet("color:#ccc; font-size:13px; line-height:1.6;");

    auto* vk_cons = new QLabel(
        "⚠️  Slightly slower than CUDA on NVIDIA<br>"
        "⚠️  Requires <b>Vulkan SDK</b> installed");
    vk_cons->setTextFormat(Qt::RichText);
    vk_cons->setWordWrap(true);
    vk_cons->setStyleSheet("color:#888; font-size:12px;");

    auto* vk_radio = new QRadioButton("Select Vulkan");
    vk_radio->setStyleSheet("color:#ff6b35; font-weight:700; font-size:14px;");

    vulkan_layout->addWidget(vk_icon);
    vulkan_layout->addWidget(vk_title);
    vulkan_layout->addWidget(vk_badge);
    vulkan_layout->addWidget(vk_pros);
    vulkan_layout->addWidget(vk_cons);
    vulkan_layout->addStretch();
    vulkan_layout->addWidget(vk_radio);
    styleOptionCard(vulkan_card_, false);

    cards_row->addWidget(cuda_card_);
    cards_row->addWidget(vulkan_card_);
    main_layout->addLayout(cards_row);

    // ── Status label ──────────────────────────────────────
    status_lbl_ = new QLabel("💡  Tip: You can change this later in Settings → AI → GPU Backend");
    status_lbl_->setStyleSheet("color:#555; font-size:12px;");
    status_lbl_->setAlignment(Qt::AlignCenter);
    main_layout->addWidget(status_lbl_);

    // ── Confirm button ─────────────────────────────────────
    confirm_btn_ = new QPushButton("⚡  Use CUDA (Recommended)");
    confirm_btn_->setFixedHeight(48);
    confirm_btn_->setStyleSheet(R"(
        QPushButton {
            background: qlineargradient(x1:0,y1:0,x2:1,y2:0,
                stop:0 #4a7a00, stop:1 #76b900);
            color: white;
            border: none;
            border-radius: 10px;
            font-size: 15px;
            font-weight: 700;
        }
        QPushButton:hover { background: qlineargradient(x1:0,y1:0,x2:1,y2:0,
            stop:0 #5a9000, stop:1 #8ad600); }
        QPushButton:pressed { background: #3a6000; }
    )");

    main_layout->addWidget(confirm_btn_);

    // ── Connect signals ────────────────────────────────────
    connect(cuda_radio,   &QRadioButton::clicked, this, &GpuSetupWizard::onCudaSelected);
    connect(vk_radio,     &QRadioButton::clicked, this, &GpuSetupWizard::onVulkanSelected);
    connect(confirm_btn_, &QPushButton::clicked,  this, &GpuSetupWizard::onConfirm);

    // Card click also toggles (click anywhere on the card)
    cuda_card_->installEventFilter(this);
    vulkan_card_->installEventFilter(this);
    cuda_radio->setChecked(true);
    selected_ = GpuBackend::CUDA;
}

bool GpuSetupWizard::eventFilter(QObject* obj, QEvent* event) {
    if (event->type() == QEvent::MouseButtonPress) {
        if (obj == cuda_card_)   onCudaSelected();
        if (obj == vulkan_card_) onVulkanSelected();
    }
    return QDialog::eventFilter(obj, event);
}

void GpuSetupWizard::onCudaSelected() {
    selected_ = GpuBackend::CUDA;
    styleOptionCard(cuda_card_,   true);
    styleOptionCard(vulkan_card_, false);
    animateSelection(cuda_card_);
    confirm_btn_->setText("⚡  Use CUDA (Recommended)");
    confirm_btn_->setStyleSheet(R"(
        QPushButton {
            background: qlineargradient(x1:0,y1:0,x2:1,y2:0,
                stop:0 #4a7a00, stop:1 #76b900);
            color: white; border: none; border-radius: 10px;
            font-size: 15px; font-weight: 700;
        }
        QPushButton:hover { background: qlineargradient(x1:0,y1:0,x2:1,y2:0,
            stop:0 #5a9000, stop:1 #8ad600); }
    )");
}

void GpuSetupWizard::onVulkanSelected() {
    selected_ = GpuBackend::Vulkan;
    styleOptionCard(cuda_card_,   false);
    styleOptionCard(vulkan_card_, true);
    animateSelection(vulkan_card_);
    confirm_btn_->setText("🌋  Use Vulkan");
    confirm_btn_->setStyleSheet(R"(
        QPushButton {
            background: qlineargradient(x1:0,y1:0,x2:1,y2:0,
                stop:0 #cc4400, stop:1 #ff6b35);
            color: white; border: none; border-radius: 10px;
            font-size: 15px; font-weight: 700;
        }
        QPushButton:hover { background: qlineargradient(x1:0,y1:0,x2:1,y2:0,
            stop:0 #e05500, stop:1 #ff7f50); }
    )");
}

void GpuSetupWizard::onConfirm() {
    accept();
}

void GpuSetupWizard::setStatusMessage(const QString& msg) {
    if (status_lbl_) {
        status_lbl_->setText(msg);
        status_lbl_->setStyleSheet("color:#cc8800; font-size:12px; font-weight:600;");
    }
}

// ════════════════════════════════════════════════════════════
// runGpuSetupIfNeeded — public entry point
// ════════════════════════════════════════════════════════════

GpuConfig runGpuSetupIfNeeded(QWidget* parent) {
    // ── 1. If already configured, skip everything ────────────
    if (fs::exists(CONFIG_PATH)) {
        return GpuConfig::load();
    }

    // ── 2. Detect hardware ───────────────────────────────────
    DeviceProfiler profiler;
    GpuConfig cfg;
    cfg.gpu_name      = profiler.getGPUName();
    cfg.gpu_memory_mb = profiler.getGPUMemoryMB();
    cfg.vendor        = profiler.hasNvidiaGPU() ? "nvidia"
                      : profiler.hasAMDGPU()    ? "amd"
                                                : "intel";

    // Recommended GPU layers based on VRAM
    if (cfg.gpu_memory_mb >= 4096)      cfg.recommended_layers = 32;
    else if (cfg.gpu_memory_mb >= 2048) cfg.recommended_layers = 18;
    else if (cfg.gpu_memory_mb >= 1024) cfg.recommended_layers = 10;
    else                                cfg.recommended_layers = 0;

    // ── 3. NVIDIA: show choice dialog ───────────────────────
    if (profiler.hasNvidiaGPU()) {
        bool cuda_ok = cudaAvailable();

        GpuSetupWizard wizard(cfg.gpu_name, cfg.gpu_memory_mb, parent);

        // If CUDA isn't installed, pre-select Vulkan and explain why
        if (!cuda_ok) {
            wizard.onVulkanSelected();
            wizard.setStatusMessage(
                "⚠️  CUDA Toolkit not found on this PC — Vulkan pre-selected. "
                "Install CUDA Toolkit if you want to switch later.");
        }

        wizard.exec();
        cfg.backend = wizard.selectedBackend();
        cfg.save();

        // Notify user of their choice
        QString msg = (cfg.backend == GpuBackend::CUDA)
            ? "✅  <b>CUDA</b> selected!<br>AI will use your NVIDIA GPU at maximum speed."
            : "✅  <b>Vulkan</b> selected!<br>AI will use your NVIDIA GPU via the universal Vulkan driver.";
        QMessageBox info(parent);
        info.setWindowTitle("GPU Setup Complete");
        info.setText(msg);
        info.setTextFormat(Qt::RichText);
        info.setIcon(QMessageBox::Information);
        info.exec();
        return cfg;
    }

    // ── 4. AMD: auto-select Vulkan, show brief notice ────────
    if (profiler.hasAMDGPU()) {
        cfg.backend = GpuBackend::Vulkan;
        cfg.save();

        QMessageBox info(parent);
        info.setWindowTitle("GPU Detected — Vulkan Enabled");
        info.setTextFormat(Qt::RichText);
        info.setText(
            QString("<b style='color:#ff6b35;'>🌋  AMD Radeon GPU Detected</b><br><br>"
                    "<b>%1</b>  (%2 MB VRAM)<br><br>"
                    "Vulkan has been automatically enabled as your AI acceleration engine.<br>"
                    "This is the optimal choice for AMD GPUs and works great!<br><br>"
                    "<span style='color:#76b900;'>You can change this in <b>Settings → AI → GPU Backend</b> at any time.</span>")
            .arg(QString::fromStdString(cfg.gpu_name))
            .arg(cfg.gpu_memory_mb));
        info.setIcon(QMessageBox::Information);
        info.exec();
        return cfg;
    }

    // ── 5. Intel / no GPU: auto Vulkan with a note ──────────
    cfg.backend = GpuBackend::Vulkan;
    cfg.recommended_layers = 0;  // integrated GPU — don't offload layers
    cfg.save();

    QMessageBox info(parent);
    info.setWindowTitle("GPU Setup");
    info.setTextFormat(Qt::RichText);
    info.setText(
        QString("<b>GPU Detected: %1</b><br><br>"
                "Vulkan has been enabled for AI acceleration.<br>"
                "Integrated/Intel GPUs have limited VRAM, so the AI will run mostly on CPU.<br><br>"
                "<span style='color:#aaa;'>For better performance, an NVIDIA or AMD dedicated GPU is recommended.</span>")
        .arg(QString::fromStdString(cfg.gpu_name)));
    info.setIcon(QMessageBox::Information);
    info.exec();
    return cfg;
}

} // namespace vision
