#pragma once

#include <QImage>
#include <QVector>
#include <QWidget>

#include "EngineWorker.h"

/// 摄像头实时预览：绘制最新帧，并按同一像素空间叠加人脸检测框。
class PreviewWidget : public QWidget {
    Q_OBJECT
public:
    explicit PreviewWidget(QWidget* parent = nullptr);

    void setImage(const QImage& img);
    void setDetections(const QVector<DetectionOut>& dets);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QImage m_image;
    QVector<DetectionOut> m_dets;
};