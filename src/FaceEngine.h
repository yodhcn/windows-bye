#pragma once

#include <QRect>
#include <QString>
#include <QVector>

#include <memory>

/// 基于 InspireFace SDK 的人脸区域检测（仅检测，不做身份识别）。
/// - 加载 models/app.pack 资源包；
/// - 会话关闭识别（enable_recognition=false），仅做 SCRFD 人脸检测；
/// - 不提取特征、不入库、不比对身份。
class FaceEngine {
public:
    struct Result {
        QRect rect;
        float score = 0.f;
    };

    bool init(const QString& modelPackPath, QString* error);
    void shutdown();
    bool ready() const { return m_ready; }
    int process(const unsigned char* bgr, int w, int h, QVector<Result>* results);

    FaceEngine();
    ~FaceEngine();
    FaceEngine(FaceEngine&&) noexcept;             ///< 支持作为成员移动构造
    FaceEngine& operator=(FaceEngine&&) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;   // InspireFace session（PIMPL，避免向外部暴露 SDK 头）
    bool m_ready = false;
};
