#include "voice_visualizer.h"
#include <QPainterPath>
#include <algorithm>
#include <cmath>

namespace vision {

VoiceVisualizer::VoiceVisualizer(QWidget* parent)
    : QWidget(parent) {
    setMinimumSize(80, 80);
    setMaximumSize(120, 120);

    animation_timer_ = new QTimer(this);
    connect(animation_timer_, &QTimer::timeout, this, &VoiceVisualizer::animate);
    animation_timer_->start(16); // ~60fps smooth animation
}

void VoiceVisualizer::onAudioLevelChanged(float rms) {
    // Expected RMS is roughy 0.0 to 1.0 (clamped).
    // Often it stays low so we boost it for visual effect.
    float boosted = std::clamp(rms * 2.5f, 0.0f, 1.0f);
    target_rms_ = boosted;
    
    // Base radius + additional radius based on volume
    target_radius_ = 20.0f + (boosted * 35.0f);
}

void VoiceVisualizer::animate() {
    // Lerp RMS and radius for smooth, gooey kinetic feel
    current_rms_ += (target_rms_ - current_rms_) * kSmoothingFactor;
    current_radius_ += (target_radius_ - current_radius_) * kSmoothingFactor;
    
    if (std::abs(target_radius_ - current_radius_) > 0.5f || current_rms_ > 0.01f) {
        update();
    }
}

void VoiceVisualizer::paintEvent(QPaintEvent* event) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    int cx = width() / 2;
    int cy = height() / 2;

    // Glowing orb effect: intense center, faded edges
    QRadialGradient gradient(cx, cy, current_radius_);
    
    // Core color: Bright Cyan/Green depending on intensity
    QColor coreColor(57, 255, 20 + static_cast<int>(current_rms_ * 100), 200);
    // Edge color: Fades to transparent
    QColor edgeColor(57, 255, 20, 0);

    gradient.setColorAt(0.0, coreColor);
    gradient.setColorAt(0.6, QColor(57, 255, 20, static_cast<int>(80 + current_rms_ * 50)));
    gradient.setColorAt(1.0, edgeColor);

    painter.setBrush(QBrush(gradient));
    painter.setPen(Qt::NoPen);

    // Draw the pulsating orb
    painter.drawEllipse(QPointF(cx, cy), current_radius_, current_radius_);
}

} // namespace vision
