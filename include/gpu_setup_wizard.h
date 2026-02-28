#pragma once
/**
 * @file gpu_setup_wizard.h
 * @brief First-run GPU backend selection wizard
 *
 * Logic:
 *  - AMD/Intel GPU  → auto-selects Vulkan, no dialog shown
 *  - NVIDIA GPU     → shows choice dialog (CUDA vs Vulkan) with full explanation
 *  - No GPU / CPU   → shows notice and uses CPU
 *
 * The choice is persisted to data/gpu_config.json so the wizard never appears again.
 */

#include <QDialog>
#include <QButtonGroup>
#include <QLabel>
#include <QRadioButton>
#include <QPushButton>
#include <QFrame>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QProgressBar>
#include <QPropertyAnimation>
#include <QTimer>
#include <string>

namespace vision {

// ── GPU Config ──────────────────────────────────────────────────────────────
enum class GpuBackend {
    CUDA,    // NVIDIA — fastest, requires CUDA Toolkit
    Vulkan,  // Universal — AMD/Intel/NVIDIA, slightly slower than CUDA
    CPU      // Fallback — no GPU or driver issues
};

struct GpuConfig {
    GpuBackend backend = GpuBackend::CPU;
    std::string gpu_name;
    int         gpu_memory_mb = 0;
    std::string vendor;          // "nvidia" | "amd" | "intel" | "unknown"
    int         recommended_layers = 0;

    static GpuConfig load();    // loads from data/gpu_config.json
    void save() const;          // saves to data/gpu_config.json
    std::string backendName() const;
};

// ── Wizard Dialog (NVIDIA only) ──────────────────────────────────────────────
class GpuSetupWizard : public QDialog {
    Q_OBJECT
public:
    explicit GpuSetupWizard(const std::string& gpu_name,
                             int                gpu_mb,
                             QWidget*           parent = nullptr);

    GpuBackend selectedBackend() const { return selected_; }

public slots:
    void onCudaSelected();
    void onVulkanSelected();

private slots:
    void onConfirm();

public:
    void setStatusMessage(const QString& msg);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    void buildUI();
    void styleOptionCard(QFrame* card, bool active);
    void animateSelection(QFrame* card);

    std::string gpu_name_;
    int         gpu_mb_;
    GpuBackend  selected_ = GpuBackend::CUDA;  // default for NVIDIA

    QFrame*      cuda_card_   = nullptr;
    QFrame*      vulkan_card_ = nullptr;
    QPushButton* confirm_btn_ = nullptr;
    QLabel*      status_lbl_  = nullptr;
};

// ── Public entry point ───────────────────────────────────────────────────────
/**
 * Called from main() before showing VisionAI.
 * If config already exists → returns it immediately.
 * If first run → runs hardware detection + wizard (if NVIDIA).
 */
GpuConfig runGpuSetupIfNeeded(QWidget* parent = nullptr);

} // namespace vision
