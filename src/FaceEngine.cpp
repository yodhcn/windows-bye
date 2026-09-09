#include "FaceEngine.h"

// InspireFace C++ SDK（仅检测，不做身份识别）。
#include <inspireface/inspireface.hpp>

#include <exception>

namespace {
// 检测用输入分辨率级别：320 是默认、性价比最高；越小越快（越省 CPU），越大越能检出更远/更小的人脸。
constexpr int kDetectPixelLevel = 320;
constexpr int kMaxFaces = 5;
}  // namespace

struct FaceEngine::Impl {
    std::shared_ptr<inspire::Session> session;
};

FaceEngine::FaceEngine() = default;

FaceEngine::~FaceEngine() {
    // 仅释放本会话，不触碰进程级 Launch 全局（避免析构顺序问题）。
    if (m_impl) {
        if (m_impl->session)
            m_impl->session.reset();
        m_impl.reset();
    }
    m_ready = false;
}

FaceEngine::FaceEngine(FaceEngine&&) noexcept = default;
FaceEngine& FaceEngine::operator=(FaceEngine&&) noexcept = default;

bool FaceEngine::init(const QString& modelPackPath, QString* error) {
    if (m_ready)
        return true;
    try {
        // 全局资源加载：InspireFace 需先把模型包载入，再创建会话。
        auto launch = INSPIREFACE_CONTEXT;
        if (!launch->isMLoad()) {
            const int32_t loadRet = launch->Load(modelPackPath.toStdString());
            if (loadRet != 0) {
                if (error)
                    *error = QStringLiteral("InspireFace 加载模型包失败（错误码 %1）").arg(loadRet);
                m_ready = false;
                return false;
            }
        }

        // 检测-only 会话：关闭识别/活体/属性等一切非检测能力，降低 CPU 占用。
        inspire::CustomPipelineParameter param;
        param.enable_recognition = false;
        param.enable_liveness = false;
        param.enable_ir_liveness = false;
        param.enable_mask_detect = false;
        param.enable_face_attribute = false;
        param.enable_face_quality = false;
        param.enable_interaction_liveness = false;
        param.enable_face_pose = false;
        param.enable_face_emotion = false;

        if (!m_impl)
            m_impl.reset(new Impl);
        m_impl->session = std::shared_ptr<inspire::Session>(
            inspire::Session::CreatePtr(inspire::DETECT_MODE_ALWAYS_DETECT,
                                        kMaxFaces, param, kDetectPixelLevel));
        if (!m_impl->session) {
            if (error)
                *error = QStringLiteral("无法创建 InspireFace 检测会话");
            m_ready = false;
            return false;
        }
        m_ready = true;
    } catch (const std::exception& e) {
        if (error)
            *error = QStringLiteral("初始化 InspireFace 失败：%1")
                         .arg(QString::fromUtf8(e.what()));
        m_ready = false;
        return false;
    } catch (...) {
        if (error)
            *error = QStringLiteral("初始化 InspireFace 失败（未知异常）");
        m_ready = false;
        return false;
    }
    return true;
}

void FaceEngine::shutdown() {
    if (m_impl) {
        if (m_impl->session)
            m_impl->session.reset();
        m_impl.reset();
    }
    if (INSPIREFACE_CONTEXT && INSPIREFACE_CONTEXT->isMLoad())
        INSPIREFACE_CONTEXT->Unload();
    m_ready = false;
}

int FaceEngine::process(const unsigned char* bgr, int w, int h, QVector<Result>* results) {
    results->clear();
    if (!m_ready || !m_impl || !m_impl->session || !bgr || w <= 0 || h <= 0)
        return 0;
    try {
        // 直接以摄像头 BGR 帧构建处理流程；ROTATION_0 保证返回框与原始帧同一像素空间。
        inspirecv::FrameProcess process =
            inspirecv::FrameProcess::Create(bgr, h, w, inspirecv::BGR, inspirecv::ROTATION_0);

        std::vector<inspire::FaceTrackWrap> faces;
        const int32_t ret = m_impl->session->FaceDetectAndTrack(process, faces);
        if (ret != 0)
            return 0;

        results->reserve(static_cast<qsizetype>(faces.size()));
        for (const auto& f : faces) {
            const auto& r = f.rect;  // x,y,width,height（原始帧像素坐标）
            if (r.width <= 0 || r.height <= 0)
                continue;
            Result out;
            out.rect  = QRect(r.x, r.y, r.width, r.height);
            out.score = 0.f;
            results->push_back(out);
        }
    } catch (...) {
        return 0;
    }
    return results->size();
}
