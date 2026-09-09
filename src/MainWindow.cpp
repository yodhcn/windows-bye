#include "MainWindow.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFont>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QShowEvent>
#include <QSpinBox>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QVBoxLayout>

#include <windows.h>
#include <WtsApi32.h>

#pragma comment(lib, "Wtsapi32.lib")

namespace {

QLabel* makeLabel(const QString& text, QWidget* parent = nullptr, void* = nullptr) {
    auto* l = new QLabel(text, parent);
    l->setAlignment(Qt::AlignCenter);
    return l;
}

constexpr char kRunValueName[] = "WindowsBye";
constexpr char kRunKeyPath[] =
    "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";

// 开机自启是否已启用（读注册表 Run 项）。
bool autostartEnabled() {
    QSettings s(QString::fromLatin1(kRunKeyPath), QSettings::NativeFormat);
    return s.contains(QString::fromLatin1(kRunValueName));
}

// 写入/移除开机自启：带 --tray 参数 → 开机后仅以托盘图标运行，不弹出主界面。
void setAutostartEnabled(bool on) {
    QSettings s(QString::fromLatin1(kRunKeyPath), QSettings::NativeFormat);
    if (on) {
        const QString exe =
            QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
        const QString cmd = QStringLiteral("\"%1\" --tray").arg(exe);
        s.setValue(QString::fromLatin1(kRunValueName), cmd);
    } else {
        s.remove(QString::fromLatin1(kRunValueName));
    }
    s.sync();
}

// 设置文件路径（放在应用目录内，随目录一同便携携带）。
QString settingsFilePath() {
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("settings.ini"));
}

}  // namespace

MainWindow::MainWindow(int defaultDelayMs, QWidget* parent)
    : QMainWindow(parent), m_appRoot(QCoreApplication::applicationDirPath()),
      m_delayMs(defaultDelayMs) {
    setWindowTitle(kAppName);
    setWindowIcon(makeAppIcon());
    resize(860, 620);

    // 读取上次保存的离开延迟（持久化）。
    {
        QSettings cfg(settingsFilePath(), QSettings::IniFormat);
        m_delayMs = qBound(1000, cfg.value(QStringLiteral("leaveDelaySec"), defaultDelayMs / 1000).toInt() * 1000,
                           600 * 1000);
        m_cameraIndex = cfg.value(QStringLiteral("cameraIndex"), 0).toInt();
    }

    m_worker = new EngineWorker(QStringLiteral("models/app.pack"), m_appRoot, this);
    m_worker->setCameraIndex(m_cameraIndex);

    buildUi();
    buildTray();
    applyTheme();

    // 信号连接。
    connect(m_worker, &EngineWorker::previewFrame, this, &MainWindow::onPreviewFrame);
    connect(m_worker, &EngineWorker::detections, this, &MainWindow::onDetections);
    connect(m_worker, &EngineWorker::stateChanged, this, &MainWindow::onStateChanged);
    connect(m_worker, &EngineWorker::engineReadyChanged, this, &MainWindow::onEngineReady);
    connect(m_worker, &EngineWorker::cameraReady, this, &MainWindow::onCameraReady);

    m_watchTimer = new QTimer(this);
    m_watchTimer->setInterval(500);  // 看护倒计时以秒级推进，500ms 精度足够，降低主线程空转
    connect(m_watchTimer, &QTimer::timeout, this, &MainWindow::onWatchTimer);
    m_watchTimer->start();

    // 注册会话锁屏/解锁通知（用于锁定前释放摄像头、解锁后恢复）。
    WTSRegisterSessionNotification(reinterpret_cast<HWND>(winId()), NOTIFY_FOR_THIS_SESSION);

    m_uiReady = true;  // 确保初始填充设备下拉框时不会触发重启
    m_worker->start();
}

MainWindow::~MainWindow() {
    WTSUnRegisterSessionNotification(reinterpret_cast<HWND>(winId()));
    if (m_worker)
        m_worker->stop();
}

QIcon MainWindow::makeAppIcon() {
    QPixmap pm(64, 64);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setBrush(QColor(30, 42, 60));
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(0, 0, 64, 64, 14, 14);
    // 人脸：圆 + 眼 + 微笑。
    p.setBrush(QColor(110, 180, 255));
    p.drawEllipse(32, 28, 16, 16);
    p.setBrush(QColor(30, 42, 60));
    p.drawEllipse(26, 20, 4, 4);
    p.drawEllipse(38, 20, 4, 4);
    p.setPen(QPen(QColor(30, 42, 60), 2));
    p.drawArc(28, 28, 8, 6, 200 * 16, 140 * 16);
    return QIcon(pm);
}

void MainWindow::buildUi() {
    auto* central = new QWidget(this);
    auto* root    = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // 看护页（唯一页面）。
    auto* watchPage = new QWidget;
    auto* wLay      = new QVBoxLayout(watchPage);
    wLay->setContentsMargins(12, 10, 12, 12);
    m_preview = new PreviewWidget;
    wLay->addWidget(m_preview, 1);

    // 看护控制区。
    auto* ctrlRow = new QHBoxLayout;
    ctrlRow->addWidget(makeLabel(QStringLiteral("离开延迟(秒)"), watchPage, /*placeholder*/ nullptr));
    m_delaySpin = new QSpinBox(watchPage);
    m_delaySpin->setRange(1, 600);
    m_delaySpin->setValue(m_delayMs / 1000);
    m_delaySpin->setSuffix(QStringLiteral(" 秒"));
    connect(m_delaySpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            &MainWindow::onDelayChanged);
    ctrlRow->addWidget(m_delaySpin);

    // 摄像头选择（可记忆）。
    ctrlRow->addWidget(makeLabel(QStringLiteral("摄像头"), watchPage, /*placeholder*/ nullptr));
    m_deviceCombo = new QComboBox(watchPage);
    {
        const QStringList devs = EngineWorker::cameraDevices();
        if (devs.isEmpty())
            m_deviceCombo->addItem(QStringLiteral("（未找到摄像头）"));
        else
            m_deviceCombo->addItems(devs);
    }
    m_deviceCombo->setCurrentIndex(qBound(0, m_cameraIndex, m_deviceCombo->count() - 1));
    m_deviceCombo->setToolTip(QStringLiteral("选择用于看护的摄像头设备（自动记忆）"));
    ctrlRow->addWidget(m_deviceCombo, 1);
    connect(m_deviceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &MainWindow::onDeviceChanged);

    m_pauseBtn = new QPushButton(QStringLiteral("暂停看护"), watchPage);
    m_pauseBtn->setCheckable(true);
    m_pauseBtn->setCursor(Qt::PointingHandCursor);
    connect(m_pauseBtn, &QPushButton::toggled, this, &MainWindow::onPauseToggled);
    ctrlRow->addWidget(m_pauseBtn);

    m_autostartChk = new QCheckBox(QStringLiteral("开机自启"), watchPage);
    m_autostartChk->setCursor(Qt::PointingHandCursor);
    m_autostartChk->setChecked(autostartEnabled());
    connect(m_autostartChk, &QCheckBox::toggled, this, &MainWindow::onAutostartToggled);
    ctrlRow->addWidget(m_autostartChk);

    ctrlRow->addStretch();

    auto* infoRow = new QHBoxLayout;
    m_statusLabel = makeLabel(QStringLiteral("正在初始化…"), watchPage, nullptr);
    m_statusLabel->setAlignment(Qt::AlignRight);
    infoRow->addStretch();
    infoRow->addWidget(m_statusLabel, 1);

    wLay->addLayout(ctrlRow);
    wLay->addLayout(infoRow);

    root->addWidget(watchPage, 1);

    setCentralWidget(central);
}

void MainWindow::buildTray() {
    m_tray = new QSystemTrayIcon(makeAppIcon(), this);
    m_tray->setToolTip(kAppName);
    auto* menu = new QMenu(this);
    auto* show = menu->addAction(QStringLiteral("显示主界面"));
    auto* quit = menu->addAction(QStringLiteral("退出"));
    connect(show, &QAction::triggered, this, &MainWindow::onTrayShow);
    connect(quit, &QAction::triggered, this, &MainWindow::onTrayExit);
    m_tray->setContextMenu(menu);
    connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
        if (r == QSystemTrayIcon::Trigger || r == QSystemTrayIcon::DoubleClick)
            onTrayShow();
    });
    m_tray->show();
}

void MainWindow::onPreviewFrame(const QImage& frame) {
    m_preview->setImage(frame);
}

void MainWindow::onDetections(int, int, QVector<DetectionOut> dets) {
    m_preview->setDetections(dets);
}

void MainWindow::onStateChanged(bool present, quint64 /*ts*/) {
    m_present = present;
    updateStatusText();
}

void MainWindow::onEngineReady(bool ready, const QString& msg) {
    m_engineReady = ready;
    if (!ready)
        m_statusLabel->setText(QStringLiteral("引擎错误: %1").arg(msg));
    updateStatusText();
}

void MainWindow::onWatchTimer() {
    // 延迟以秒为单位，转成毫秒。
    if (m_delaySpin)
        m_delayMs = m_delaySpin->value() * 1000;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    // 锁定期间：摄像头已释放，不参与看护计时。
    if (m_locked) {
        m_absentSince = 0;
        return;
    }
    if (!m_engineReady || !m_cameraReady || m_paused) {
        m_absentSince = 0;
        updateStatusText();
        return;
    }
    if (m_present) {
        m_absentSince = 0;
        updateStatusText();
        return;
    }
    if (m_absentSince == 0)
        m_absentSince = now;

    const qint64 elapsed = now - m_absentSince;
    updateStatusText();

    // 画面中无人脸超过延迟，且距上次锁屏 10 秒以上 → 锁屏。
    if (elapsed >= m_delayMs && (now - m_lastLockMs) >= 10000) {
        LockWorkStation();
        m_lastLockMs = now;
        m_absentSince = now;  // 锁屏后重新计时（恢复后仍需马上辨识）
    }
}

void MainWindow::updateStatusText() {
    if (!m_statusLabel)
        return;
    if (!m_pauseBtn)
        return;

    QString text;
    if (m_locked)
        text = QStringLiteral("已锁定，摄像头已释放（解锁后恢复）");
    else if (!m_cameraReady)
        text = QStringLiteral("等待摄像头就绪…");
    else if (!m_engineReady)
        text = QStringLiteral("引擎加载中…");
    else if (m_pauseBtn->isChecked())
        text = QStringLiteral("已暂停看护");
    else if (m_present)
        text = QStringLiteral("检测到人脸，安全，不锁屏");
    else {
        const qint64 elapsed = m_absentSince ? QDateTime::currentMSecsSinceEpoch() - m_absentSince : 0;
        if (elapsed <= 0)
            text = QStringLiteral("画面中未检测到人脸");
        else
            text = QStringLiteral("无人脸 %1 秒 / %2 秒后锁屏")
                       .arg(elapsed / 1000)
                       .arg(m_delayMs / 1000);
    }
    m_statusLabel->setText(text);
}

void MainWindow::onPauseToggled(bool paused) {
    m_paused = paused;
    m_absentSince = 0;
    updateStatusText();
}

void MainWindow::onAutostartToggled(bool checked) {
    setAutostartEnabled(checked);
    m_statusLabel->setText(checked ? QStringLiteral("已开启开机自启（下次登录生效）")
                                   : QStringLiteral("已关闭开机自启"));
    updateStatusText();
}

void MainWindow::onCameraReady(bool ready) {
    m_cameraReady = ready;
    if (!ready)
        m_absentSince = 0;
    updateStatusText();
}

void MainWindow::onDelayChanged(int sec) {
    // 持久化离开延迟设置。
    QSettings cfg(settingsFilePath(), QSettings::IniFormat);
    cfg.setValue(QStringLiteral("leaveDelaySec"), sec);
    cfg.sync();
}

void MainWindow::onDeviceChanged(int idx) {
    if (!m_uiReady || idx < 0 || idx == m_cameraIndex)
        return;
    m_cameraIndex = idx;
    m_worker->setCameraIndex(idx);
    // 持久化摄像头选择。
    QSettings cfg(settingsFilePath(), QSettings::IniFormat);
    cfg.setValue(QStringLiteral("cameraIndex"), idx);
    cfg.sync();
    // 重启采集/检测线程以应用新设备。
    m_worker->stop();
    m_worker->start();
    m_cameraReady = false;
    updateStatusText();
}

void MainWindow::onSessionLocked() {
    if (m_locked)
        return;
    m_locked = true;
    m_present = false;
    m_absentSince = 0;
    m_worker->pauseCapture();   // 主动释放摄像头资源
    m_preview->setDetections({});
    updateStatusText();
}

void MainWindow::onSessionUnlocked() {
    if (!m_locked)
        return;
    m_locked = false;
    m_absentSince = 0;
    m_worker->resumeCapture();  // 解锁后恢复采集与检测
    updateStatusText();
}

bool MainWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
    if (eventType == "windows_generic_MSG") {
        MSG* msg = static_cast<MSG*>(message);
        if (msg->message == WM_WTSSESSION_CHANGE) {
            if (msg->wParam == WTS_SESSION_LOCK) {
                onSessionLocked();
            } else if (msg->wParam == WTS_SESSION_UNLOCK) {
                onSessionUnlocked();
            }
            *result = 0;
            return true;
        }
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}

void MainWindow::onTrayShow() {
    showNormal();
    raise();
    activateWindow();
}

void MainWindow::onTrayExit() {
    m_exiting = true;
    if (m_worker)
        m_worker->stop();
    if (m_tray)
        m_tray->hide();
    QApplication::quit();
}

void MainWindow::showFromTray() {
    // 从托盘/最小化/后台（含 --tray 启动）把主界面带回前台。
    show();
    setWindowState((windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
    raise();
    activateWindow();
    // 部分 Windows 环境需下一事件循环后再激活才会真正置顶。
    QTimer::singleShot(0, this, [this] {
        raise();
        activateWindow();
    });
}

void MainWindow::closeEvent(QCloseEvent* event) {
    // 仅托盘退出时真正关闭；点关闭按钮则隐藏到托盘后台运行。
    if (m_exiting) {
        event->accept();
        return;
    }
    event->ignore();
    hide();
}

void MainWindow::showEvent(QShowEvent* event) {
    QMainWindow::showEvent(event);
    // 主界面可见时才发布预览帧（有消费者），降低隐藏/后台时的 CPU 占用。
    if (m_worker)
        m_worker->setPreviewEnabled(true);
}

void MainWindow::hideEvent(QHideEvent* event) {
    QMainWindow::hideEvent(event);
    // 隐藏到托盘（后台运行）时无人消费预览 → 关闭预览帧发布。
    if (m_worker)
        m_worker->setPreviewEnabled(false);
}

void MainWindow::applyTheme() {
    const QString qss = QStringLiteral(R"(
        * { font-family: "Microsoft YaHei UI"; }
        QWidget { background-color: #181a20; color: #e6e6e6; }
        QStackedWidget QWidget { background-color: #20232b; }
        QPushButton { background:#2a2f3a; border:1px solid #3a4150; border-radius:6px; padding:7px 16px; }
        QPushButton:hover { background:#333a48; }
        QPushButton:checked { background:#3b6ea5; border-color:#4d8dc9; }
        QSpinBox { background:#2a2f3a; border:1px solid #3a4150; border-radius:6px; padding:5px; }
        QLabel { color:#e6e6e6; }
    )");
    qApp->setStyleSheet(qss);
    QApplication::setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 10));
}
