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
    /// 是否"够近"（人脸框宽度 ≥ 最小占比阈值），够近的人脸才参与"用户在场"判定。
    /// 用于排除坐在后排/经过的远处人脸，避免误判当前用户仍在座位上。
    /// 注意命名 isNear 而非 near：Windows SDK minwindef.h 把 near/far 定义成空宏，
    /// 若叫 near 会在 .cpp(含 windows.h) 中被展开成空而报 C2059。
    bool isNear = false;
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
    /// 设置"够近"判定阈值：人脸框宽度须 ≥ 画面宽度的该百分比（1~100）才算用户在场。
    /// 例：15 表示人脸宽度至少占画面 15%（正常坐姿通常 25~35%，太远/后排常 <10%）。
    void setMinFaceWidthPct(int pct);

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
    // "够近"阈值(百分比)：detectLoop 运行中可被 UI 实时修改，故用 atomic。
    std::atomic<int> m_minFaceWidthPct{15};

    // 最新帧共享（采集写/检测读）。
    std::mutex m_frameMutex;
    cv::Mat m_latestFrame;

    FaceEngine m_engine;

    std::thread m_captureThread;
    std::thread m_detectThread;
    bool m_started = false;
};
