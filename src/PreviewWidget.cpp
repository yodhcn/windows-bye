#include "PreviewWidget.h"

#include <QPainter>

#include <cmath>

PreviewWidget::PreviewWidget(QWidget* parent)
    : QWidget(parent), m_image(640, 480, QImage::Format_RGB888) {
    setMinimumSize(320, 240);
}

void PreviewWidget::setImage(const QImage& img) {
    if (img.isNull())
        return;
    m_image = img;
    update();
}

void PreviewWidget::setDetections(const QVector<DetectionOut>& dets) {
    m_dets = dets;
    update();
}

void PreviewWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), QColor(24, 26, 32));

    if (m_image.isNull())
        return;

    const double iw      = m_image.width();
    const double ih      = m_image.height();
    const double scale   = std::min(width() / iw, height() / ih);
    const int drawW      = (int)std::round(iw * scale);
    const int drawH      = (int)std::round(ih * scale);
    const int offX       = (width() - drawW) / 2;
    const int offY       = (height() - drawH) / 2;
    const QRect target(offX, offY, drawW, drawH);

    p.drawImage(target, m_image);

    // 叠加人脸检测框（坐标与图像同像素空间）。
    for (const auto& d : m_dets) {
        QRect r((int)(d.rect.x() * scale) + offX, (int)(d.rect.y() * scale) + offY,
                (int)(d.rect.width() * scale), (int)(d.rect.height() * scale));
        p.setPen(QPen(QColor(0, 200, 120), 2));
        p.drawRect(r.adjusted(1, 1, -1, -1));
    }
}