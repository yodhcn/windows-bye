#pragma once

#include <QImage>
#include <QObject>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <mutex>
#include <thread>

#include <opencv2/core.hpp>

#include "FaceEngine.h"

/// 一次检测输出（矩形坐标与预览图同一像素空间：摄像头采集分辨率）。
struct DetectionOut {
    QRect rect;
    float score = 0.f;
};

/// 后台引擎：一个采集线程 + 一个检测线程。
/// 采集线程读摄像头并发布预览帧；检测线程做人脸区域检测并回传结果（不识别身份）。
/// 支持锁定前释放摄像头、解锁后恢复（pauseCapture / resumeCapture）。
class EngineWorker : public QObject {
    Q_OBJECT
public:
    EngineWorker(QString modelPath, QString appRoot, QObject* parent = nullptr);
    ~EngineWorker();

    void start();   ///< 启动采集+检测线程（非阻塞）。
    void stop();    ///< 停止并回收。

    /// 枚举本机可用摄像头设备名（与 OpenCV 设备索引顺序一致）。
    static QStringList cameraDevices();
    /// 设置使用的设备索引（需配合 stop/start 重启采集以生效）。
    void setCameraIndex(int idx);

    /// 释放摄像头并暂停检测（锁定时调用，立即返回）。
    void pauseCapture();
    /// 恢复摄像头采集与检测（解锁后调用）。
    void resumeCapture();
    /// 开关预览帧发布：仅当主界面可见（有消费者）时才做解码并 emit，否则跳过以降低 CPU。
    void setPreviewEnabled(bool on);

signals:
    void previewFrame(QImage frame);
    void detections(int srcW, int srcH, QVector<DetectionOut> dets);
    void stateChanged(bool present, quint64 timestampMs);
    void engineReadyChanged(bool ready, QString message);
    void cameraReady(bool ok);

private:
    void captureLoop();
    void detectLoop();

    QString m_modelPath;
    QString m_appRoot;

    // 设备索引：仅在 stop 后（线程已回收）修改，start 后采集线程读取。
    int m_cameraIndex = 0;

    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_paused{false};
    // 预览发布开关：窗口可见（有消费者）才为 true；为 false 时采集线程跳过预览解码/emit。
    std::atomic<bool> m_previewEnabled{false};

    // 最新帧共享（采集写/检测读）。
    std::mutex m_frameMutex;
    cv::Mat m_latestFrame;

    FaceEngine m_engine;

    std::thread m_captureThread;
    std::thread m_detectThread;
    bool m_started = false;
};
