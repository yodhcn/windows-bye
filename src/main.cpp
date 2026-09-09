#include <QApplication>
#include <QFont>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMetaType>
#include <QString>

#include "EngineWorker.h"
#include "MainWindow.h"

namespace {
// 单实例命名空间：按用户名区分，避免不同登录会话/用户互相"检测到"。
QString instanceName() {
    return QStringLiteral("WindowsBye-%1")
        .arg(QString::fromUtf8(qgetenv("USERNAME")).isEmpty()
                 ? QStringLiteral("default")
                 : QString::fromUtf8(qgetenv("USERNAME")));
}
}  // namespace

int main(int argc, char* argv[]) {
    // 高 DPI 感知，保证图标与界面清晰。
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);

    QApplication app(argc, argv);
    QApplication::setApplicationName(kAppName);
    QApplication::setOrganizationName(QStringLiteral("WindowsBye"));
    QApplication::setQuitOnLastWindowClosed(false);  // 后台运行：关闭窗口不退出，托盘常驻

    // 注册跨线程队列连接的元类型。
    qRegisterMetaType<DetectionOut>("DetectionOut");
    qRegisterMetaType<QVector<DetectionOut>>("QVector<DetectionOut>");

    // 单实例：若已有实例在运行，通知其弹出主界面，然后本实例直接退出。
    {
        QLocalSocket probe;
        probe.connectToServer(instanceName());
        if (probe.waitForConnected(300)) {
            probe.write("show");
            probe.flush();
            probe.waitForBytesWritten(300);
            return 0;
        }
    }

    // 可选项：-d <秒> 指定默认离开延迟。
    const QStringList args = QApplication::arguments();
    int delayMs = 30 * 1000;
    for (int i = 1; i + 1 < args.size(); ++i) {
        if (args[i] == u"-d" || args[i] == QStringLiteral("--delay")) {
            bool ok = false;
            const int s = args[i + 1].toInt(&ok);
            if (ok && s > 0)
                delayMs = s * 1000;
        }
    }

    // 开机自启（注册表附带 --tray）：仅创建托盘图标后台运行，不弹出主界面。
    const bool trayOnly = args.contains(QStringLiteral("--tray"));

    // 主实例：监听本地管道，接收"已有实例请展示主界面"的请求。
    QLocalServer::removeServer(instanceName());
    QLocalServer ipcServer;
    ipcServer.listen(instanceName());

    MainWindow w(delayMs);
    if (!trayOnly)
        w.show();

    QObject::connect(&ipcServer, &QLocalServer::newConnection, [&] {
        QLocalSocket* c = ipcServer.nextPendingConnection();
        if (c) {
            c->waitForReadyRead(500);
            if (c->readAll().contains("show"))
                w.showFromTray();
            c->deleteLater();
        }
    });

    return app.exec();
}