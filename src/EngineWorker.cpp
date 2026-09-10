#include "EngineWorker.h"

#include <QDateTime>
#include <QDir>

#include <algorithm>
#include <chrono>
#include <cstdint>

#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <dshow.h>
#include <windows.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "strmiids.lib")

namespace {
int64_t nowMs() {
    return QDateTime::currentMSecsSinceEpoch();
}

// 判断画面是否"纯黑/被串流静音"（摄像头被其它进程占用时，DirectShow 常返回黑帧或花屏帧）。
bool isFrameBlack(const cv::Mat& frame, double threshold = 20.0) {
    if (frame.empty())
        return true;
    cv::Mat gray;
    cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
    return cv::mean(gray)[0] < threshold;
}

// 检测节流间隔：人脸存在检测无需高帧率，降低 CPU 占用（固定 ~1.25fps）。
// 与看护超时判断节奏一致：超时延迟默认 30s、最小 1s，远大于检测粒度，
// 故 800ms 的检测分辨率不会影响锁屏决策，还能显著降低检测推理频次。
constexpr qint64 kDetectIntervalMs = 800;

// 后台（预览隐藏到托盘）时采集抓帧周期：此时预览无消费者，只需在检测需要新帧前供帧，
// 故采集降到 ~1.67fps（仍略快于检测 1.25fps，保证检测拿到的帧足够新），
// 避免以摄像头默认高帧率全速抓取+clone 空转。
// 前台（预览可见）时采集满速（不节流），保证预览画面流畅跟手。
constexpr qint64 kBackgroundCaptureIntervalMs = 600;
}  // namespace

EngineWorker::EngineWorker(QString modelPath, QString appRoot, QObject* parent)
    : QObject(parent), m_modelPath(std::move(modelPath)), m_appRoot(std::move(appRoot)) {}

EngineWorker::~EngineWorker() { stop(); }

void EngineWorker::pauseCapture() {
    m_paused = true;
    emit cameraReady(false);
}

void EngineWorker::resumeCapture() {
    m_paused = false;
}

void EngineWorker::setPreviewEnabled(bool on) {
    m_previewEnabled.store(on);
}

void EngineWorker::start() {
    if (m_started)
        return;
    m_stop = false;   // 支持 stop 后再次 start（切换摄像头时重启采集/检测）
    m_paused = false;
    m_started = true;
    m_captureThread = std::thread([this] { captureLoop(); });
    m_detectThread  = std::thread([this] { detectLoop(); });
}

void EngineWorker::setCameraIndex(int idx) {
    m_cameraIndex = idx < 0 ? 0 : idx;
}

void EngineWorker::setMinFaceWidthPct(int pct) {
    m_minFaceWidthPct.store(qBound(1, pct, 100));
}

QStringList EngineWorker::cameraDevices() {
    // 用 DirectShow 枚举视频输入设备（返回的次序与 OpenCV 设备索引一致）。
    QStringList names;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // S_OK / S_FALSE 均需配套 CoUninitialize
    ICreateDevEnum* pDevEnum = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&pDevEnum)))) {
        IEnumMoniker* pEnum = nullptr;
        if (SUCCEEDED(pDevEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &pEnum, 0)) &&
            pEnum) {
            IMoniker* pMoniker = nullptr;
            while (pEnum->Next(1, &pMoniker, nullptr) == S_OK) {
                IPropertyBag* pPropBag = nullptr;
                if (SUCCEEDED(pMoniker->BindToStorage(0, 0, IID_PPV_ARGS(&pPropBag)))) {
                    VARIANT var;
                    VariantInit(&var);
                    if (SUCCEEDED(pPropBag->Read(L"FriendlyName", &var, nullptr)) &&
                        var.vt == VT_BSTR)
                        names << QString::fromWCharArray(var.bstrVal);
                    VariantClear(&var);
                    pPropBag->Release();
                }
                pMoniker->Release();
            }
            pEnum->Release();
        }
        pDevEnum->Release();
    }
    CoUninitialize();
    return names;
}

void EngineWorker::stop() {
    if (!m_started)
        return;
    m_stop = true;
    if (m_captureThread.joinable())
        m_captureThread.join();
    if (m_detectThread.joinable())
        m_detectThread.join();
    m_engine.shutdown();
    m_started = false;
}

void EngineWorker::captureLoop() {
    // 优先用 Media Foundation 读取默认摄像头；打不开/中途断开/被其它进程占用都会自动重试。
    int rejectCount = 0;  // 连续读失败计数，判定摄像头中途丢失
    int blackCount   = 0; // 连续纯黑帧计数（DirectShow 回退分支判定"被其它进程静音"）
    while (!m_stop) {
        // 锁定/暂停期间：不持有摄像头，空闲等待恢复。
        if (m_paused.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        cv::VideoCapture cap;
        // 优先用 Media Foundation：被占用/不可用时 open() 会直接失败，是更底层可靠的信号。
        bool opened = cap.open(m_cameraIndex, cv::CAP_MSMF);
        // 个别老摄像头仅支持 DirectShow：回退打开，并辅以黑帧兜底校验。
        if (!opened)
            opened = cap.open(m_cameraIndex, cv::CAP_DSHOW);
        if (!opened) {
            emit cameraReady(false);
            // 摄像头被占用/未插入：短等待后自动重试（约每 3 秒一次）。
            for (int i = 0; i < 30 && !m_stop && !m_paused.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        cap.set(cv::CAP_PROP_FRAME_WIDTH, 640);
        cap.set(cv::CAP_PROP_FRAME_HEIGHT, 480);

        cv::Mat frame;
        int64_t lastFrameProcessed = 0;  // 上次成功处理帧的时刻，用于后台时控制抓帧上限
        bool cameraOk = false;  // 读到"有实际画面"的帧后才视为摄像头正常工作
        while (!m_stop) {
            // 锁定前主动释放摄像头资源。
            if (m_paused.load()) {
                cap.release();
                break;  // 回到外层循环空闲等待恢复
            }
            // 采集抓帧节奏随"预览是否可见"自适应：
            //  - 前台（预览可见）：满速不节流，保证预览画面流畅跟手；
            //  - 后台（隐藏到托盘，预览无消费者）：降频到 kBackgroundCaptureIntervalMs，
            //    只需在检测需要新帧前供帧，避免以摄像头默认高帧率全速抓取空转。
            // 摄像头读失败会走下方重试分支，此时 lastFrameProcessed 较旧，不会误触发节流。
            if (!m_previewEnabled.load(std::memory_order_relaxed) && lastFrameProcessed != 0) {
                const int64_t sinceFrame = nowMs() - lastFrameProcessed;
                if (sinceFrame < kBackgroundCaptureIntervalMs) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(kBackgroundCaptureIntervalMs - sinceFrame));
                    continue;
                }
            }
            if (!cap.read(frame) || frame.empty()) {
                if (++rejectCount >= 25) {  // 连续约 0.5s 读不到帧 → 判定摄像头释放/断开
                    if (cameraOk) {
                        cameraOk = false;
                        emit cameraReady(false);
                    }
                    rejectCount = 0;
                    break;  // 回到外层重新打开并重试
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            rejectCount = 0;
            lastFrameProcessed = nowMs();  // 记录本次成功处理，控制采集抓帧上限
            if (isFrameBlack(frame)) {
                // 持续输出纯黑帧 → 摄像头正被其它进程串流占用/静音（DirectShow 回退时常见）。
                if (++blackCount >= 10 && cameraOk) {
                    cameraOk = false;
                    emit cameraReady(false);
                }
            } else {
                blackCount = 0;
                if (!cameraOk) {
                    cameraOk = true;
                    emit cameraReady(true);  // 摄像头输出实际画面后才开始远程监测
                }
            }
            {
                std::lock_guard<std::mutex> lk(m_frameMutex);
                m_latestFrame = frame.clone();
            }

            // 预览帧仅在有消费者（主界面可见）时发布，满速跟随摄像头帧率（不节流）保证流畅。
            if (m_previewEnabled.load(std::memory_order_relaxed)) {
                cv::Mat rgb;
                cv::cvtColor(frame, rgb, cv::COLOR_BGR2RGB);
                QImage img(reinterpret_cast<const uchar*>(rgb.data), rgb.cols, rgb.rows,
                           static_cast<int>(rgb.step), QImage::Format_RGB888);
                emit previewFrame(img.copy());
            }
        }
        cap.release();
    }
}

void EngineWorker::detectLoop() {
    QString err;
    if (!m_engine.init(QDir(m_appRoot).filePath(m_modelPath), &err)) {
        emit engineReadyChanged(false, err);
        return;
    }
    emit engineReadyChanged(true, QStringLiteral("引擎就绪"));

    qint64 lastDet = 0;
    while (!m_stop) {
        // 锁定/暂停期间不检测，也不占用 CPU。
        if (m_paused.load()) {
            lastDet = 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // 节流：最多每 kDetectIntervalMs 检测一次，降低 CPU 占用。
        const qint64 now = nowMs();
        const qint64 sinceLast = now - lastDet;
        if (sinceLast < kDetectIntervalMs) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kDetectIntervalMs - sinceLast));
            continue;
        }

        cv::Mat frame;
        {
            std::lock_guard<std::mutex> lk(m_frameMutex);
            if (!m_latestFrame.empty())
                frame = m_latestFrame.clone();
        }
        if (frame.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        lastDet = nowMs();

        QVector<FaceEngine::Result> results;
        m_engine.process(frame.data, frame.cols, frame.rows, &results);

        // 用"人脸框宽度占画面宽度的比例"判定人脸是否够近(足以视为当前用户在场)。
        // 太窄的小人脸=坐得很远/是后排经过的人，不参与在场判定，防止远处人员触发"人在位"。
        const int frameW = frame.cols;
        const int minW   = frameW * m_minFaceWidthPct.load() / 100;  // 该百分比对应的最小像素宽

        QVector<DetectionOut> dets;
        dets.reserve(results.size());
        bool anyNear = false;  // 是否存在够近的人脸
        for (const auto& r : results) {
            DetectionOut o;
            o.rect  = r.rect;
            o.score = r.score;
            o.isNear  = r.rect.width() >= minW;
            if (o.isNear)
                anyNear = true;
            dets.push_back(o);
        }

        emit detections(frame.cols, frame.rows, dets);
        // present = 至少一个"够近"的人脸；仅远处人脸视为用户不在位。
        emit stateChanged(anyNear, nowMs());
    }
}
