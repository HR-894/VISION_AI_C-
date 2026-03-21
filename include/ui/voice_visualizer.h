#pragma once

#include <QWidget>
#include <QTimer>
#include <QPainter>

namespace vision {

class VoiceVisualizer : public QWidget {
    Q_OBJECT
public:
    explicit VoiceVisualizer(QWidget* parent = nullptr);

public slots:
    /// Slot to receive RMS audio levels from VoiceManager
    void onAudioLevelChanged(float rms);

protected:
    void paintEvent(QPaintEvent* event) override;

private slots:
    void animate();

private:
    float current_rms_ = 0.0f;
    float target_rms_  = 0.0f;
    
    float current_radius_ = 10.0f;
    float target_radius_  = 10.0f;
    
    QTimer* animation_timer_ = nullptr;
    
    // Lerping parameters
    static constexpr float kSmoothingFactor = 0.2f;
};

} // namespace vision
