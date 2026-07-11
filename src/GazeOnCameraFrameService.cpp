#include "GazeOnCameraFrameService.hpp"

#include "StereoCalibrationSupport.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace DualIC4Varjo {
namespace {

constexpr std::size_t kFrameHistoryCapacity = 1024;
constexpr std::size_t kGazeQueueCapacity = 20000;

struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct Vec4 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double w = 1.0;
};

struct ProjectedQuad {
    std::array<varjo_Vector2Df, 4> corners{};
    bool valid = false;
    bool fallback = false;
    std::int64_t frameNumber = 0;
};

struct MappedGaze {
    float displayX = std::numeric_limits<float>::quiet_NaN();
    float displayY = std::numeric_limits<float>::quiet_NaN();
    float cameraX = std::numeric_limits<float>::quiet_NaN();
    float cameraY = std::numeric_limits<float>::quiet_NaN();
    bool inside = false;
};

struct FrameRecord {
    VarjoFrameInfoSnapshot frameInfo;
    GazeCameraPlaneSnapshot plane;
};

struct CalibrationState {
    bool enabled = false;
    CalibrationImageSize sourceSize{};
    CalibrationImageSize outputSize{};
    CalibrationHomography leftInverse{};
    CalibrationHomography rightInverse{};
};

Vec3 add(const Vec3& a, const Vec3& b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3 multiply(const Vec3& value, double scalar)
{
    return {value.x * scalar, value.y * scalar, value.z * scalar};
}

double dot(const Vec3& a, const Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 cross(const Vec3& a, const Vec3& b)
{
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

Vec3 normalize(const Vec3& value, const Vec3& fallback)
{
    const double length = std::sqrt(dot(value, value));
    if (!(length > 1.0e-9)) return fallback;
    return multiply(value, 1.0 / length);
}

Vec4 multiplyMatrix(const double matrix[16], const Vec4& value)
{
    return {
        matrix[0] * value.x + matrix[4] * value.y +
            matrix[8] * value.z + matrix[12] * value.w,
        matrix[1] * value.x + matrix[5] * value.y +
            matrix[9] * value.z + matrix[13] * value.w,
        matrix[2] * value.x + matrix[6] * value.y +
            matrix[10] * value.z + matrix[14] * value.w,
        matrix[3] * value.x + matrix[7] * value.y +
            matrix[11] * value.z + matrix[15] * value.w,
    };
}

Vec3 eyeOriginWorld(const double view[16])
{
    const double tx = view[12];
    const double ty = view[13];
    const double tz = view[14];
    return {
        -(view[0] * tx + view[1] * ty + view[2] * tz),
        -(view[4] * tx + view[5] * ty + view[6] * tz),
        -(view[8] * tx + view[9] * ty + view[10] * tz),
    };
}

Vec3 viewVectorToWorld(const double view[16], const Vec3& value)
{
    return {
        view[0] * value.x + view[1] * value.y + view[2] * value.z,
        view[4] * value.x + view[5] * value.y + view[6] * value.z,
        view[8] * value.x + view[9] * value.y + view[10] * value.z,
    };
}

bool reconstructHeadBasis(
    const VarjoFrameInfoSnapshot& frame,
    Vec3& center,
    Vec3& right,
    Vec3& up,
    Vec3& forward)
{
    if (!frame.valid || frame.views.size() < 2) return false;
    const Vec3 leftEye = eyeOriginWorld(frame.views[0].viewMatrix);
    const Vec3 rightEye = eyeOriginWorld(frame.views[1].viewMatrix);
    center = multiply(add(leftEye, rightEye), 0.5);
    right = normalize(add(
        viewVectorToWorld(frame.views[0].viewMatrix, {1.0, 0.0, 0.0}),
        viewVectorToWorld(frame.views[1].viewMatrix, {1.0, 0.0, 0.0})),
        {1.0, 0.0, 0.0});
    up = normalize(add(
        viewVectorToWorld(frame.views[0].viewMatrix, {0.0, 1.0, 0.0}),
        viewVectorToWorld(frame.views[1].viewMatrix, {0.0, 1.0, 0.0})),
        {0.0, 1.0, 0.0});
    forward = normalize(add(
        viewVectorToWorld(frame.views[0].viewMatrix, {0.0, 0.0, -1.0}),
        viewVectorToWorld(frame.views[1].viewMatrix, {0.0, 0.0, -1.0})),
        {0.0, 0.0, -1.0});
    right = normalize(right, normalize(cross(forward, up), {1.0, 0.0, 0.0}));
    up = normalize(cross(right, forward), up);
    return true;
}

Vec3 gazePointHead(const varjo_Ray& ray, double distance)
{
    return {
        ray.origin[0] + ray.forward[0] * distance,
        ray.origin[1] + ray.forward[1] * distance,
        -(ray.origin[2] + ray.forward[2] * distance),
    };
}

Vec3 headPointToWorld(
    const VarjoFrameInfoSnapshot& frame,
    const Vec3& point)
{
    Vec3 center{}, right{}, up{}, forward{};
    if (!reconstructHeadBasis(frame, center, right, up, forward)) return point;
    const Vec3 positiveZ = multiply(forward, -1.0);
    return add(center, add(
        multiply(right, point.x),
        add(multiply(up, point.y), multiply(positiveZ, point.z))));
}

varjo_Vector2Df projectRayToDisplayUv(
    const varjo_Ray& ray,
    double focusDistance,
    const VarjoFrameInfoSnapshot& frame,
    std::size_t viewIndex)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    if (frame.views.size() <= viewIndex) return {nan, nan};
    double distance = focusDistance;
    if (!(distance > 0.01 && distance <= 2.0)) distance = 1.0;
    const Vec3 world = headPointToWorld(frame, gazePointHead(ray, distance));
    const Vec4 eye = multiplyMatrix(
        frame.views[viewIndex].viewMatrix,
        {world.x, world.y, world.z, 1.0});
    const Vec4 clip = multiplyMatrix(
        frame.views[viewIndex].projectionMatrix,
        eye);
    if (!std::isfinite(clip.w) || std::fabs(clip.w) <= 1.0e-12) {
        return {nan, nan};
    }
    const double ndcX = clip.x / clip.w;
    const double ndcY = clip.y / clip.w;
    if (!std::isfinite(ndcX) || !std::isfinite(ndcY)) return {nan, nan};
    return {
        static_cast<float>(ndcX * 0.5 + 0.5),
        static_cast<float>(0.5 - ndcY * 0.5),
    };
}

varjo_Vector2Df projectWorldPointToDisplayUv(
    const Vec3& world,
    const varjo_ViewInfo& view)
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const Vec4 eye = multiplyMatrix(
        view.viewMatrix,
        {world.x, world.y, world.z, 1.0});
    const Vec4 clip = multiplyMatrix(view.projectionMatrix, eye);
    if (!std::isfinite(clip.w) || std::fabs(clip.w) <= 1.0e-12) {
        return {nan, nan};
    }
    const double ndcX = clip.x / clip.w;
    const double ndcY = clip.y / clip.w;
    if (!std::isfinite(ndcX) || !std::isfinite(ndcY)) return {nan, nan};
    return {
        static_cast<float>(ndcX * 0.5 + 0.5),
        static_cast<float>(0.5 - ndcY * 0.5),
    };
}

ProjectedQuad buildProjectedQuad(
    const FrameRecord& record,
    bool left)
{
    ProjectedQuad result;
    result.frameNumber = record.frameInfo.frameNumber;
    if (record.plane.placementMode != VarjoXR::PlacementMode::HeadRelative ||
        record.frameInfo.views.size() < 2 ||
        !(record.plane.width > 0.0f) || !(record.plane.height > 0.0f)) {
        return result;
    }

    Vec3 center{}, right{}, up{}, forward{};
    if (!reconstructHeadBasis(record.frameInfo, center, right, up, forward)) {
        return result;
    }
    Vec3 planeCenter = add(center, multiply(forward, -record.plane.z));
    planeCenter = add(planeCenter, multiply(right, record.plane.x));
    planeCenter = add(planeCenter, multiply(up, record.plane.y));
    const Vec3 halfRight = multiply(right, record.plane.width * 0.5);
    const Vec3 halfUp = multiply(up, record.plane.height * 0.5);
    const std::array<Vec3, 4> corners{
        add(add(planeCenter, multiply(halfRight, -1.0)), halfUp),
        add(add(planeCenter, halfRight), halfUp),
        add(add(planeCenter, multiply(halfRight, -1.0)), multiply(halfUp, -1.0)),
        add(add(planeCenter, halfRight), multiply(halfUp, -1.0)),
    };
    const auto& view = record.frameInfo.views[left ? 0u : 1u];
    for (std::size_t index = 0; index < corners.size(); ++index) {
        result.corners[index] = projectWorldPointToDisplayUv(corners[index], view);
        if (!std::isfinite(result.corners[index].x) ||
            !std::isfinite(result.corners[index].y)) {
            return result;
        }
    }
    result.valid = true;
    return result;
}

bool applyInverseHomography(
    const CalibrationHomography& homography,
    double x,
    double y,
    double& outputX,
    double& outputY)
{
    const auto& h = homography.rows;
    const double denominator = h[6] * x + h[7] * y + h[8];
    if (!std::isfinite(denominator) || std::fabs(denominator) <= 1.0e-12) {
        return false;
    }
    outputX = (h[0] * x + h[1] * y + h[2]) / denominator;
    outputY = (h[3] * x + h[4] * y + h[5]) / denominator;
    return std::isfinite(outputX) && std::isfinite(outputY);
}

MappedGaze mapRayToCamera(
    const varjo_Ray& ray,
    double focusDistance,
    const FrameRecord& record,
    const CalibrationState& calibration,
    bool left)
{
    MappedGaze result;
    const std::size_t viewIndex = left ? 0u : 1u;
    const auto display = projectRayToDisplayUv(
        ray, focusDistance, record.frameInfo, viewIndex);
    result.displayX = display.x;
    result.displayY = display.y;

    if (record.plane.placementMode != VarjoXR::PlacementMode::HeadRelative ||
        !(record.plane.width > 0.0f) || !(record.plane.height > 0.0f)) {
        return result;
    }

    const Vec3 origin{
        ray.origin[0],
        ray.origin[1],
        -ray.origin[2],
    };
    const Vec3 direction{
        ray.forward[0],
        ray.forward[1],
        -ray.forward[2],
    };
    if (std::fabs(direction.z) <= 1.0e-12) return result;
    const double t = (static_cast<double>(record.plane.z) - origin.z) /
        direction.z;
    if (!(t > 0.0) || !std::isfinite(t)) return result;
    const double hitX = origin.x + direction.x * t;
    const double hitY = origin.y + direction.y * t;
    const double outputU =
        (hitX - (record.plane.x - record.plane.width * 0.5)) /
        record.plane.width;
    const double outputV =
        ((record.plane.y + record.plane.height * 0.5) - hitY) /
        record.plane.height;

    const std::uint32_t rawWidth = left
        ? record.plane.leftFrameWidth
        : record.plane.rightFrameWidth;
    const std::uint32_t rawHeight = left
        ? record.plane.leftFrameHeight
        : record.plane.rightFrameHeight;
    if (rawWidth <= 1 || rawHeight <= 1) return result;

    const std::uint32_t outputWidth = calibration.enabled
        ? calibration.outputSize.width
        : rawWidth;
    const std::uint32_t outputHeight = calibration.enabled
        ? calibration.outputSize.height
        : rawHeight;
    if (outputWidth <= 1 || outputHeight <= 1) return result;

    double rawX = outputU * static_cast<double>(outputWidth - 1u);
    double rawY = outputV * static_cast<double>(outputHeight - 1u);
    if (calibration.enabled) {
        const CalibrationHomography& inverse = left
            ? calibration.leftInverse
            : calibration.rightInverse;
        double mappedX = 0.0;
        double mappedY = 0.0;
        if (!applyInverseHomography(inverse, rawX, rawY, mappedX, mappedY)) {
            return result;
        }
        rawX = mappedX;
        rawY = mappedY;
    }

    result.cameraX = static_cast<float>(
        rawX / static_cast<double>(rawWidth - 1u));
    result.cameraY = static_cast<float>(
        rawY / static_cast<double>(rawHeight - 1u));
    result.inside =
        std::isfinite(result.cameraX) && std::isfinite(result.cameraY) &&
        outputU >= 0.0 && outputU <= 1.0 &&
        outputV >= 0.0 && outputV <= 1.0 &&
        rawX >= 0.0 && rawX <= static_cast<double>(rawWidth - 1u) &&
        rawY >= 0.0 && rawY <= static_cast<double>(rawHeight - 1u);
    return result;
}

std::string csvFloat(float value)
{
    if (!std::isfinite(value)) return "nan";
    std::ostringstream stream;
    stream << std::setprecision(9) << value;
    return stream.str();
}

const char* csvBool(bool value)
{
    return value ? "1" : "0";
}

std::int64_t systemUnixUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::int64_t captureUnixUs(
    varjo_Session* session,
    varjo_Nanoseconds captureTime)
{
    if (!session || captureTime <= 0) return 0;
    return static_cast<std::int64_t>(
        varjo_ConvertToUnixTime(session, captureTime) / 1000);
}

const char* kCsvHeader =
    "row_unix_us,"
    "gaze_captureTime,gaze_capture_unix_us_estimated,gaze_frameNumber,"
    "gaze_left_display_x01,gaze_left_display_y01,gaze_left_camera_x01,gaze_left_camera_y01,gaze_left_inside_frame,"
    "gaze_right_display_x01,gaze_right_display_y01,gaze_right_camera_x01,gaze_right_camera_y01,gaze_right_inside_frame,"
    "renderingGaze_captureTime,renderingGaze_capture_unix_us_estimated,renderingGaze_frameNumber,"
    "renderingGaze_left_display_x01,renderingGaze_left_display_y01,renderingGaze_left_camera_x01,renderingGaze_left_camera_y01,renderingGaze_left_inside_frame,"
    "renderingGaze_right_display_x01,renderingGaze_right_display_y01,renderingGaze_right_camera_x01,renderingGaze_right_camera_y01,renderingGaze_right_inside_frame,"
    "canvas_width,canvas_height,"
    "left_frame_width,left_frame_height,left_horizontal_offset,left_vertical_offset,"
    "right_frame_width,right_frame_height,right_horizontal_offset,right_vertical_offset,"
    "mapping_mode,projection_frame_number,"
    "left_projected_quad_valid,left_projected_quad_fallback,"
    "left_quad_tl_x01,left_quad_tl_y01,left_quad_tr_x01,left_quad_tr_y01,left_quad_bl_x01,left_quad_bl_y01,left_quad_br_x01,left_quad_br_y01,"
    "right_projected_quad_valid,right_projected_quad_fallback,"
    "right_quad_tl_x01,right_quad_tl_y01,right_quad_tr_x01,right_quad_tr_y01,right_quad_bl_x01,right_quad_bl_y01,right_quad_br_x01,right_quad_br_y01,"
    "spatial_plane_distance_m,spatial_plane_width_m,spatial_plane_height_m\n";

std::mutex gHookMutex;
std::unique_ptr<GazeOnCameraFrameService> gHookService;
std::atomic<GazeOnCameraFrameService*> gHookRaw{nullptr};
std::atomic<std::uint64_t> gFinalReceived{0};
std::atomic<std::uint64_t> gFinalWritten{0};
std::atomic<std::uint64_t> gFinalDropped{0};
std::atomic<std::uint64_t> gFinalFrames{0};
std::atomic<std::uint64_t> gFinalEvicted{0};
std::filesystem::path gFinalPath;
std::string gFinalError;

} // namespace

struct GazeOnCameraFrameService::Impl {
    explicit Impl(
        std::filesystem::path path,
        std::optional<std::filesystem::path> calibrationPath)
        : outputPath(std::move(path))
    {
        if (calibrationPath) {
            const auto document = LoadStereoCalibrationJson(*calibrationPath);
            const auto& profile = document.profile(document.defaultProfile);
            calibration.enabled = true;
            calibration.sourceSize = document.sourceSize;
            calibration.outputSize = document.rectifiedOutputSize;
            calibration.leftInverse = profile.leftInverse;
            calibration.rightInverse = profile.rightInverse;
        }
    }

    std::filesystem::path outputPath;
    CalibrationState calibration;
    std::ofstream output;
    std::thread worker;
    std::atomic<bool> stopRequested{true};
    std::condition_variable condition;
    mutable std::mutex mutex;
    std::deque<FrameRecord> frames;
    std::deque<VarjoEyeTrackingData> gazeQueue;
    std::shared_ptr<varjo_Session> session;
    std::string lastError;
    std::atomic<std::uint64_t> received{0};
    std::atomic<std::uint64_t> written{0};
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<std::uint64_t> submittedFrames{0};
    std::atomic<std::uint64_t> evictedFrames{0};

    std::optional<FrameRecord> findFrame(varjo_Nanoseconds target)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (frames.empty()) return std::nullopt;
        auto iterator = std::upper_bound(
            frames.begin(), frames.end(), target,
            [](varjo_Nanoseconds value, const FrameRecord& frame) {
                return value < frame.frameInfo.displayTime;
            });
        if (iterator == frames.begin()) return *iterator;
        --iterator;
        return *iterator;
    }

    void writeMapped(
        const varjo_Gaze& gaze,
        const FrameRecord& frame)
    {
        const auto left = mapRayToCamera(
            gaze.gaze, gaze.focusDistance, frame, calibration, true);
        const auto right = mapRayToCamera(
            gaze.gaze, gaze.focusDistance, frame, calibration, false);
        output
            << gaze.captureTime << ','
            << captureUnixUs(session.get(), gaze.captureTime) << ','
            << gaze.frameNumber << ','
            << csvFloat(left.displayX) << ','
            << csvFloat(left.displayY) << ','
            << csvFloat(left.cameraX) << ','
            << csvFloat(left.cameraY) << ','
            << csvBool(left.inside) << ','
            << csvFloat(right.displayX) << ','
            << csvFloat(right.displayY) << ','
            << csvFloat(right.cameraX) << ','
            << csvFloat(right.cameraY) << ','
            << csvBool(right.inside) << ',';
    }

    void writeRow(const VarjoEyeTrackingData& data)
    {
        const auto paired = findFrame(data.gaze.captureTime);
        if (!paired) return;
        const auto renderingFrame = data.renderingGaze
            ? findFrame(data.renderingGaze->captureTime)
            : std::optional<FrameRecord>{};
        const ProjectedQuad leftQuad = buildProjectedQuad(*paired, true);
        const ProjectedQuad rightQuad = buildProjectedQuad(*paired, false);

        output << systemUnixUs() << ',';
        writeMapped(data.gaze, *paired);
        if (data.renderingGaze && renderingFrame) {
            writeMapped(*data.renderingGaze, *renderingFrame);
        } else {
            for (int index = 0; index < 13; ++index) output << "nullopt,";
        }

        const std::uint32_t canvasWidth = calibration.enabled
            ? calibration.outputSize.width
            : paired->plane.leftFrameWidth;
        const std::uint32_t canvasHeight = calibration.enabled
            ? calibration.outputSize.height
            : paired->plane.leftFrameHeight;
        output
            << canvasWidth << ',' << canvasHeight << ','
            << paired->plane.leftFrameWidth << ','
            << paired->plane.leftFrameHeight << ",0,0,"
            << paired->plane.rightFrameWidth << ','
            << paired->plane.rightFrameHeight << ",0,0,"
            << (calibration.enabled
                ? "headlocked_plane_calibrated_homography"
                : "headlocked_plane_identity") << ','
            << paired->frameInfo.frameNumber << ','
            << csvBool(leftQuad.valid) << ','
            << csvBool(leftQuad.fallback) << ',';
        for (const auto& corner : leftQuad.corners) {
            output << csvFloat(corner.x) << ',' << csvFloat(corner.y) << ',';
        }
        output
            << csvBool(rightQuad.valid) << ','
            << csvBool(rightQuad.fallback) << ',';
        for (const auto& corner : rightQuad.corners) {
            output << csvFloat(corner.x) << ',' << csvFloat(corner.y) << ',';
        }
        output
            << std::max(0.0f, -paired->plane.z) << ','
            << paired->plane.width << ','
            << paired->plane.height << '\n';
        if (!output) throw std::runtime_error("gaze-on-camera-frame CSV write failed");
        written.fetch_add(1, std::memory_order_acq_rel);
    }

    void workerMain() noexcept
    {
        try {
            for (;;) {
                VarjoEyeTrackingData data{};
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    condition.wait(lock, [&] {
                        return stopRequested.load(std::memory_order_acquire) ||
                            !gazeQueue.empty();
                    });
                    if (gazeQueue.empty()) {
                        if (stopRequested.load(std::memory_order_acquire)) break;
                        continue;
                    }
                    data = std::move(gazeQueue.front());
                    gazeQueue.pop_front();
                }
                writeRow(data);
            }
            output.flush();
        } catch (const std::exception& exception) {
            std::lock_guard<std::mutex> lock(mutex);
            lastError = exception.what();
            stopRequested.store(true, std::memory_order_release);
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex);
            lastError = "unknown gaze-on-camera-frame worker failure";
            stopRequested.store(true, std::memory_order_release);
        }
    }
};

GazeOnCameraFrameService::GazeOnCameraFrameService(
    std::filesystem::path outputPath,
    std::optional<std::filesystem::path> calibrationJson)
    : impl_(std::make_unique<Impl>(
        std::move(outputPath), std::move(calibrationJson)))
{
}

GazeOnCameraFrameService::~GazeOnCameraFrameService()
{
    stop();
}

bool GazeOnCameraFrameService::start()
{
    stop();
    try {
        std::error_code error;
        std::filesystem::create_directories(
            impl_->outputPath.parent_path(), error);
        if (error) throw std::runtime_error(error.message());
        impl_->output.open(impl_->outputPath, std::ios::out | std::ios::trunc);
        if (!impl_->output.is_open()) {
            throw std::runtime_error(
                "failed to open gaze-on-camera-frame CSV: " +
                impl_->outputPath.string());
        }
        impl_->output << kCsvHeader;
        impl_->stopRequested.store(false, std::memory_order_release);
        impl_->worker = std::thread(&Impl::workerMain, impl_.get());
        std::cout
            << "[GAZE_CAMERA] asynchronous projection service started\n"
            << "[GAZE_CAMERA] CSV: " << impl_->outputPath.string() << '\n';
        return true;
    } catch (const std::exception& exception) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->lastError = exception.what();
        return false;
    }
}

void GazeOnCameraFrameService::stop() noexcept
{
    if (!impl_) return;
    impl_->stopRequested.store(true, std::memory_order_release);
    impl_->condition.notify_all();
    if (impl_->worker.joinable()) {
        try { impl_->worker.join(); } catch (...) {}
    }
    if (impl_->output.is_open()) {
        impl_->output.flush();
        impl_->output.close();
    }
}

bool GazeOnCameraFrameService::submitFrameInfo(
    VarjoFrameInfoSnapshot snapshot,
    GazeCameraPlaneSnapshot plane,
    std::shared_ptr<varjo_Session> session)
{
    if (!snapshot.valid) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->session = std::move(session);
    impl_->frames.push_back({std::move(snapshot), plane});
    impl_->submittedFrames.fetch_add(1, std::memory_order_acq_rel);
    while (impl_->frames.size() > kFrameHistoryCapacity) {
        impl_->frames.pop_front();
        impl_->evictedFrames.fetch_add(1, std::memory_order_acq_rel);
    }
    return true;
}

bool GazeOnCameraFrameService::submitGazeData(
    std::vector<VarjoEyeTrackingData> data)
{
    if (data.empty()) return true;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->received.fetch_add(data.size(), std::memory_order_acq_rel);
    for (auto& sample : data) {
        if (impl_->gazeQueue.size() >= kGazeQueueCapacity) {
            impl_->gazeQueue.pop_front();
            impl_->dropped.fetch_add(1, std::memory_order_acq_rel);
        }
        impl_->gazeQueue.push_back(std::move(sample));
    }
    impl_->condition.notify_one();
    return true;
}

std::filesystem::path GazeOnCameraFrameService::outputPath() const
{
    return impl_->outputPath;
}

std::string GazeOnCameraFrameService::lastError() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->lastError;
}

std::uint64_t GazeOnCameraFrameService::receivedGazeCount() const noexcept
{
    return impl_->received.load(std::memory_order_acquire);
}

std::uint64_t GazeOnCameraFrameService::writtenRowCount() const noexcept
{
    return impl_->written.load(std::memory_order_acquire);
}

std::uint64_t GazeOnCameraFrameService::droppedGazeCount() const noexcept
{
    return impl_->dropped.load(std::memory_order_acquire);
}

std::uint64_t GazeOnCameraFrameService::submittedFrameCount() const noexcept
{
    return impl_->submittedFrames.load(std::memory_order_acquire);
}

std::uint64_t GazeOnCameraFrameService::evictedFrameCount() const noexcept
{
    return impl_->evictedFrames.load(std::memory_order_acquire);
}

bool GazeOnCameraFrameHook::configure(
    const std::filesystem::path& outputPath,
    const std::optional<std::filesystem::path>& calibrationJson)
{
    stop();
    try {
        auto service = std::make_unique<GazeOnCameraFrameService>(
            outputPath, calibrationJson);
        if (!service->start()) {
            gFinalError = service->lastError();
            return false;
        }
        std::lock_guard<std::mutex> lock(gHookMutex);
        gFinalPath = outputPath;
        gFinalError.clear();
        gHookService = std::move(service);
        gHookRaw.store(gHookService.get(), std::memory_order_release);
        return true;
    } catch (const std::exception& exception) {
        std::lock_guard<std::mutex> lock(gHookMutex);
        gFinalError = exception.what();
        return false;
    }
}

void GazeOnCameraFrameHook::submitFrameInfo(
    const VarjoFrameInfoSnapshot& snapshot,
    const GazeCameraPlaneSnapshot& plane,
    const std::shared_ptr<varjo_Session>& session) noexcept
{
    auto* service = gHookRaw.load(std::memory_order_acquire);
    if (service) service->submitFrameInfo(snapshot, plane, session);
}

void GazeOnCameraFrameHook::submitGazeData(
    std::vector<VarjoEyeTrackingData> data) noexcept
{
    auto* service = gHookRaw.load(std::memory_order_acquire);
    if (service) service->submitGazeData(std::move(data));
}

void GazeOnCameraFrameHook::stop() noexcept
{
    gHookRaw.store(nullptr, std::memory_order_release);
    std::unique_ptr<GazeOnCameraFrameService> service;
    {
        std::lock_guard<std::mutex> lock(gHookMutex);
        service = std::move(gHookService);
    }
    if (!service) return;
    service->stop();
    gFinalReceived.store(service->receivedGazeCount(), std::memory_order_release);
    gFinalWritten.store(service->writtenRowCount(), std::memory_order_release);
    gFinalDropped.store(service->droppedGazeCount(), std::memory_order_release);
    gFinalFrames.store(service->submittedFrameCount(), std::memory_order_release);
    gFinalEvicted.store(service->evictedFrameCount(), std::memory_order_release);
    gFinalPath = service->outputPath();
    gFinalError = service->lastError();
}

std::filesystem::path GazeOnCameraFrameHook::outputPath()
{
    std::lock_guard<std::mutex> lock(gHookMutex);
    return gHookService ? gHookService->outputPath() : gFinalPath;
}

std::string GazeOnCameraFrameHook::lastError()
{
    std::lock_guard<std::mutex> lock(gHookMutex);
    return gHookService ? gHookService->lastError() : gFinalError;
}

std::uint64_t GazeOnCameraFrameHook::receivedGazeCount() noexcept
{
    auto* service = gHookRaw.load(std::memory_order_acquire);
    return service ? service->receivedGazeCount() :
        gFinalReceived.load(std::memory_order_acquire);
}

std::uint64_t GazeOnCameraFrameHook::writtenRowCount() noexcept
{
    auto* service = gHookRaw.load(std::memory_order_acquire);
    return service ? service->writtenRowCount() :
        gFinalWritten.load(std::memory_order_acquire);
}

std::uint64_t GazeOnCameraFrameHook::droppedGazeCount() noexcept
{
    auto* service = gHookRaw.load(std::memory_order_acquire);
    return service ? service->droppedGazeCount() :
        gFinalDropped.load(std::memory_order_acquire);
}

std::uint64_t GazeOnCameraFrameHook::submittedFrameCount() noexcept
{
    auto* service = gHookRaw.load(std::memory_order_acquire);
    return service ? service->submittedFrameCount() :
        gFinalFrames.load(std::memory_order_acquire);
}

std::uint64_t GazeOnCameraFrameHook::evictedFrameCount() noexcept
{
    auto* service = gHookRaw.load(std::memory_order_acquire);
    return service ? service->evictedFrameCount() :
        gFinalEvicted.load(std::memory_order_acquire);
}

} // namespace DualIC4Varjo
