#pragma once

#include <QMainWindow>
#include <QVector>

#include "EngineWorker.h"
#include "PreviewWidget.h"

class QSpinBox;
class QPushButton;
class QCheckBox;
class QLabel;
class QComboBox;
class QSystemTrayIcon;
class QTimer;
class QCloseEvent;

/// 应用显示名称常量（多处复用）。
inline const QString kAppName = QStringLiteral("Windows Bye");

/// 主窗口：单页面看护 + 系统托盘（后台运行）+ 离开超时锁屏。
/// 仅做人脸存在检测（SCRFD-500M），不含身份识别；锁定前释放摄像头，解锁后恢复。
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(int defaultDelayMs, QWidget* parent = nullptr);
    ~MainWindow() override;

    /// 从托盘/最小化/后台状态弹出主界面并带到前台（供单实例"已有实例"请求时调用）。
    void showFromTray();

protected:
    void closeEvent(QCloseEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private slots:
    void onPreviewFrame(const QImage& frame);
    void onDetections(int srcW, int srcH, QVector<DetectionOut> dets);
    void onStateChanged(bool present, quint64 ts);
    void onEngineReady(bool ready, const QString& msg);
    void onCameraReady(bool ready);

    void onWatchTimer();
    void onPauseToggled(bool paused);
    void onAutostartToggled(bool checked);
    void onDelayChanged(int sec);
    void onDeviceChanged(int idx);
    void onTrayShow();
    void onTrayExit();
    void onMinWidthChanged(int pct);

private:
    void buildUi();
    void buildTray();
    QIcon makeAppIcon();
    void updateStatusText();
    void applyTheme();
    void onSessionLocked();
    void onSessionUnlocked();

    QString m_appRoot;
    EngineWorker* m_worker = nullptr;

    bool m_engineReady = false;
    bool m_cameraReady = false;
    bool m_present     = false;   // 是否有"够近"的人脸(由 EngineWorker 按阈值判定)
    bool m_anyFace     = false;   // 最近一次检测是否检到任意人脸(无论远近)，用于状态文案
    bool m_locked      = false;

    // 看护状态。
    int  m_delayMs       = 30000;
    int  m_cameraIndex   = 0;
    int  m_minFaceWidthPct = 15;  // "够近"阈值：人脸框宽须占画面宽的该百分比才算在场
    bool m_paused        = false;   // 用户手动"暂停看护"
    qint64 m_absentSince = 0;
    qint64 m_lastLockMs  = -100000;  // 距上次锁屏节约时间

    // UI。
    PreviewWidget* m_preview = nullptr;
    QSpinBox* m_delaySpin    = nullptr;
    QSpinBox* m_minWSpin     = nullptr;  // 最小人脸尺寸阈值(占画面宽 %)
    QPushButton* m_pauseBtn  = nullptr;
    QCheckBox* m_autostartChk = nullptr;
    QComboBox* m_deviceCombo = nullptr;
    QLabel* m_statusLabel    = nullptr;
    QSystemTrayIcon* m_tray  = nullptr;
    QTimer* m_watchTimer     = nullptr;
    bool m_exiting           = false;
    bool m_uiReady           = false;
};
