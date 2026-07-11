#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "CheckerboardStereoCalibration.hpp"

#include <Windows.h>

#include <opencv2/calib3d.hpp>
#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace DualIC4Varjo {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kMaximumObservationCount = 30;
constexpr std::uint32_t kDetectionMaximumDimension = 960;
constexpr double kFundamentalRansacThresholdPixels = 1.5;
constexpr const char* kDefaultProfile = "affine_vertical";
constexpr const char* kRightOrder = "same";

struct Observation {
    std::vector<cv::Point2f> left;
    std::vector<cv::Point2f> right;
};

struct ProfileEstimate {
    std::string method;
    cv::Mat leftForward;
    cv::Mat rightForward;
    CalibrationHomography leftInverse;
    CalibrationHomography rightInverse;
    std::size_t usedPoints = 0;
    std::size_t inlierPoints = 0;
    double meanVerticalError = 0.0;
    double medianVerticalError = 0.0;
};

struct DetectionImage {
    cv::Mat gray;
    double scale = 1.0;
};

const IC4Ext::D3D12IndexedCameraFrame* FindCamera(
    const IC4Ext::D3D12SyncedFrameSet& set,
    std::uint32_t cameraIndex)
{
    for (const auto& item : set.frames) {
        if (item.cameraIndex == cameraIndex) return &item;
    }
    return nullptr;
}

bool KeyPressedEdge(int virtualKey, bool& previous) noexcept
{
    const bool current = (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
    const bool pressed = current && !previous;
    previous = current;
    return pressed;
}

StereoCalibrationDocument MakeEmptyDocument(
    std::uint32_t width,
    std::uint32_t height)
{
    StereoCalibrationDocument document;
    document.format = "vdca.stereo_rectification";
    document.version = 1;
    document.defaultProfile = kDefaultProfile;
    document.sourceSize = {width, height};
    document.calibrationInputSize = {width, height};
    document.rectifiedOutputSize = {width, height};
    document.resizeMode = "none";
    document.rightOrder = kRightOrder;
    document.samplingFilter = "linear";
    document.borderMode = "constant";
    document.borderRgba = {0.0f, 0.0f, 0.0f, 1.0f};
    return document;
}

CalibrationHomography ToHomography(const cv::Mat& matrix)
{
    if (matrix.rows != 3 || matrix.cols != 3) {
        throw std::runtime_error("OpenCV returned a non-3x3 homography");
    }
    cv::Mat converted;
    matrix.convertTo(converted, CV_64F);
    CalibrationHomography result;
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            const double value = converted.at<double>(row, column);
            if (!std::isfinite(value)) {
                throw std::runtime_error("OpenCV returned a non-finite homography value");
            }
            result.rows[static_cast<std::size_t>(row * 3 + column)] = value;
        }
    }
    return result;
}

CalibrationHomography InverseHomography(const cv::Mat& forward)
{
    cv::Mat inverse;
    if (cv::invert(forward, inverse, cv::DECOMP_SVD) == 0.0) {
        throw std::runtime_error("rectification homography inversion failed");
    }
    return ToHomography(inverse);
}

DetectionImage PrepareDetectionImage(
    const cv::Mat& gray,
    std::uint32_t maximumDimension)
{
    if (gray.empty()) throw std::invalid_argument("checkerboard detection image is empty");
    const int largest = std::max(gray.cols, gray.rows);
    if (maximumDimension == 0 || largest <= static_cast<int>(maximumDimension)) {
        return {gray, 1.0};
    }

    const double scale = static_cast<double>(maximumDimension) /
        static_cast<double>(largest);
    DetectionImage result;
    result.scale = scale;
    cv::resize(
        gray,
        result.gray,
        cv::Size(
            std::max(1, static_cast<int>(std::lround(gray.cols * scale))),
            std::max(1, static_cast<int>(std::lround(gray.rows * scale)))),
        0.0,
        0.0,
        cv::INTER_AREA);
    return result;
}

void RestoreFullResolution(std::vector<cv::Point2f>& corners, double scale)
{
    if (scale == 1.0) return;
    const float inverse = static_cast<float>(1.0 / scale);
    for (auto& corner : corners) {
        corner.x *= inverse;
        corner.y *= inverse;
    }
}

void RefineOnFullResolution(
    const cv::Mat& gray,
    std::vector<cv::Point2f>& corners)
{
    if (corners.empty()) return;
    try {
        cv::cornerSubPix(
            gray,
            corners,
            cv::Size(5, 5),
            cv::Size(-1, -1),
            cv::TermCriteria(
                cv::TermCriteria::EPS | cv::TermCriteria::COUNT,
                20,
                1.0e-2));
    } catch (const cv::Exception&) {
        // The reduced-image detector already returned sub-pixel positions.
    }
}

bool DetectCorners(
    const cv::Mat& fullGray,
    cv::Size boardSize,
    std::vector<cv::Point2f>& corners)
{
    const DetectionImage detection = PrepareDetectionImage(
        fullGray,
        kDetectionMaximumDimension);
    const int flags = cv::CALIB_CB_ADAPTIVE_THRESH |
        cv::CALIB_CB_NORMALIZE_IMAGE |
        cv::CALIB_CB_FAST_CHECK;
    const bool found = cv::findChessboardCorners(
        detection.gray,
        boardSize,
        corners,
        flags);
    if (!found) {
        corners.clear();
        return false;
    }
    RestoreFullResolution(corners, detection.scale);
    RefineOnFullResolution(fullGray, corners);
    return true;
}

std::vector<cv::Point2f> ApplyRightOrder(
    const std::vector<cv::Point2f>& corners,
    std::uint32_t columns,
    std::uint32_t rows,
    const std::string& order)
{
    if (corners.size() != static_cast<std::size_t>(columns) * rows) {
        throw std::invalid_argument("unexpected right checkerboard corner count");
    }
    std::vector<cv::Point2f> result(corners.size());
    for (std::uint32_t row = 0; row < rows; ++row) {
        for (std::uint32_t column = 0; column < columns; ++column) {
            std::uint32_t sourceRow = row;
            std::uint32_t sourceColumn = column;
            if (order == "flip_x" || order == "rot180") {
                sourceColumn = columns - 1u - column;
            }
            if (order == "flip_y" || order == "rot180") {
                sourceRow = rows - 1u - row;
            }
            result[static_cast<std::size_t>(row) * columns + column] =
                corners[static_cast<std::size_t>(sourceRow) * columns + sourceColumn];
        }
    }
    return result;
}

bool DetectObservation(
    const cv::Mat& leftBgr,
    const cv::Mat& rightBgr,
    cv::Size boardSize,
    Observation& observation)
{
    cv::Mat leftGray;
    cv::Mat rightGray;
    cv::cvtColor(leftBgr, leftGray, cv::COLOR_BGR2GRAY);
    cv::cvtColor(rightBgr, rightGray, cv::COLOR_BGR2GRAY);

    auto leftFuture = std::async(std::launch::async, [&]() {
        return DetectCorners(leftGray, boardSize, observation.left);
    });
    auto rightFuture = std::async(std::launch::async, [&]() {
        return DetectCorners(rightGray, boardSize, observation.right);
    });
    const bool leftFound = leftFuture.get();
    const bool rightFound = rightFuture.get();
    if (!leftFound || !rightFound) {
        observation = {};
        return false;
    }
    observation.right = ApplyRightOrder(
        observation.right,
        static_cast<std::uint32_t>(boardSize.width),
        static_cast<std::uint32_t>(boardSize.height),
        kRightOrder);
    return true;
}

double MeanCornerMotion(const Observation& previous, const Observation& current)
{
    if (previous.left.size() != current.left.size() ||
        previous.right.size() != current.right.size() ||
        current.left.empty()) {
        return std::numeric_limits<double>::infinity();
    }
    double sum = 0.0;
    std::size_t count = 0;
    for (std::size_t index = 0; index < current.left.size(); ++index) {
        sum += cv::norm(previous.left[index] - current.left[index]);
        sum += cv::norm(previous.right[index] - current.right[index]);
        count += 2;
    }
    return sum / static_cast<double>(count);
}

void Concatenate(
    const std::deque<Observation>& observations,
    std::vector<cv::Point2f>& left,
    std::vector<cv::Point2f>& right)
{
    std::size_t total = 0;
    for (const auto& observation : observations) total += observation.left.size();
    left.clear();
    right.clear();
    left.reserve(total);
    right.reserve(total);
    for (const auto& observation : observations) {
        left.insert(left.end(), observation.left.begin(), observation.left.end());
        right.insert(right.end(), observation.right.begin(), observation.right.end());
    }
}

std::pair<double, double> VerticalError(
    const cv::Mat& leftForward,
    const cv::Mat& rightForward,
    const std::vector<cv::Point2f>& left,
    const std::vector<cv::Point2f>& right)
{
    std::vector<cv::Point2f> transformedLeft;
    std::vector<cv::Point2f> transformedRight;
    cv::perspectiveTransform(left, transformedLeft, leftForward);
    cv::perspectiveTransform(right, transformedRight, rightForward);

    std::vector<double> errors;
    errors.reserve(transformedLeft.size());
    double sum = 0.0;
    for (std::size_t index = 0; index < transformedLeft.size(); ++index) {
        const double error = std::abs(
            static_cast<double>(transformedLeft[index].y) -
            static_cast<double>(transformedRight[index].y));
        errors.push_back(error);
        sum += error;
    }
    if (errors.empty()) throw std::runtime_error("no points for vertical-error evaluation");

    const double mean = sum / static_cast<double>(errors.size());
    const auto middle = errors.begin() +
        static_cast<std::ptrdiff_t>(errors.size() / 2u);
    std::nth_element(errors.begin(), middle, errors.end());
    double median = *middle;
    if ((errors.size() % 2u) == 0u) {
        median = (*std::max_element(errors.begin(), middle) + *middle) * 0.5;
    }
    return {mean, median};
}

cv::Mat ScaleToOutput(cv::Size inputSize, cv::Size outputSize)
{
    const double scaleX = static_cast<double>(outputSize.width) /
        static_cast<double>(inputSize.width);
    const double scaleY = static_cast<double>(outputSize.height) /
        static_cast<double>(inputSize.height);
    return (cv::Mat_<double>(3, 3) <<
        scaleX, 0.0, 0.0,
        0.0, scaleY, 0.0,
        0.0, 0.0, 1.0);
}

cv::Mat FitCommonCanvas(
    const cv::Mat& leftForward,
    const cv::Mat& rightForward,
    cv::Size inputSize,
    cv::Size outputSize)
{
    std::vector<cv::Point2f> imageCorners{
        {0.0f, 0.0f},
        {static_cast<float>(inputSize.width - 1), 0.0f},
        {static_cast<float>(inputSize.width - 1),
         static_cast<float>(inputSize.height - 1)},
        {0.0f, static_cast<float>(inputSize.height - 1)},
    };
    std::vector<cv::Point2f> combined;
    std::vector<cv::Point2f> transformed;
    cv::perspectiveTransform(imageCorners, combined, leftForward);
    cv::perspectiveTransform(imageCorners, transformed, rightForward);
    combined.insert(combined.end(), transformed.begin(), transformed.end());

    double minimumX = std::numeric_limits<double>::infinity();
    double minimumY = std::numeric_limits<double>::infinity();
    double maximumX = -std::numeric_limits<double>::infinity();
    double maximumY = -std::numeric_limits<double>::infinity();
    for (const auto& point : combined) {
        minimumX = std::min(minimumX, static_cast<double>(point.x));
        minimumY = std::min(minimumY, static_cast<double>(point.y));
        maximumX = std::max(maximumX, static_cast<double>(point.x));
        maximumY = std::max(maximumY, static_cast<double>(point.y));
    }
    const double extentX = std::max(1.0, maximumX - minimumX);
    const double extentY = std::max(1.0, maximumY - minimumY);
    const double scale = std::min(
        static_cast<double>(outputSize.width) / extentX,
        static_cast<double>(outputSize.height) / extentY);
    return (cv::Mat_<double>(3, 3) <<
        scale, 0.0, -minimumX * scale,
        0.0, scale, -minimumY * scale,
        0.0, 0.0, 1.0);
}

ProfileEstimate MakeEstimate(
    std::string method,
    cv::Mat leftForward,
    cv::Mat rightForward,
    const std::vector<cv::Point2f>& left,
    const std::vector<cv::Point2f>& right,
    std::size_t inlierPoints)
{
    leftForward.convertTo(leftForward, CV_64F);
    rightForward.convertTo(rightForward, CV_64F);
    const auto [mean, median] = VerticalError(
        leftForward,
        rightForward,
        left,
        right);

    ProfileEstimate result;
    result.method = std::move(method);
    result.leftForward = std::move(leftForward);
    result.rightForward = std::move(rightForward);
    result.leftInverse = InverseHomography(result.leftForward);
    result.rightInverse = InverseHomography(result.rightForward);
    result.usedPoints = left.size();
    result.inlierPoints = inlierPoints;
    result.meanVerticalError = mean;
    result.medianVerticalError = median;
    return result;
}

ProfileEstimate EstimateAffineVertical(
    const std::vector<cv::Point2f>& left,
    const std::vector<cv::Point2f>& right,
    cv::Size inputSize,
    cv::Size outputSize)
{
    cv::Mat design(static_cast<int>(right.size()), 3, CV_64F);
    cv::Mat target(static_cast<int>(right.size()), 1, CV_64F);
    for (int index = 0; index < static_cast<int>(right.size()); ++index) {
        design.at<double>(index, 0) = right[static_cast<std::size_t>(index)].x;
        design.at<double>(index, 1) = right[static_cast<std::size_t>(index)].y;
        design.at<double>(index, 2) = 1.0;
        target.at<double>(index, 0) = left[static_cast<std::size_t>(index)].y;
    }
    cv::Mat coefficients;
    if (!cv::solve(design, target, coefficients, cv::DECOMP_SVD)) {
        throw std::runtime_error("affine_vertical solve failed");
    }

    cv::Mat leftForward = cv::Mat::eye(3, 3, CV_64F);
    cv::Mat rightForward = cv::Mat::eye(3, 3, CV_64F);
    rightForward.at<double>(1, 0) = coefficients.at<double>(0, 0);
    rightForward.at<double>(1, 1) = coefficients.at<double>(1, 0);
    rightForward.at<double>(1, 2) = coefficients.at<double>(2, 0);
    const cv::Mat scale = ScaleToOutput(inputSize, outputSize);
    return MakeEstimate(
        "affine_vertical",
        scale * leftForward,
        scale * rightForward,
        left,
        right,
        left.size());
}

ProfileEstimate EstimateAffineFull(
    const std::vector<cv::Point2f>& left,
    const std::vector<cv::Point2f>& right,
    cv::Size inputSize,
    cv::Size outputSize)
{
    cv::Mat inlierMask;
    cv::Mat affine = cv::estimateAffinePartial2D(
        right,
        left,
        inlierMask,
        cv::RANSAC,
        2.5,
        5000,
        0.999,
        20);
    if (affine.empty()) throw std::runtime_error("estimateAffinePartial2D failed");

    cv::Mat leftForward = cv::Mat::eye(3, 3, CV_64F);
    cv::Mat rightForward = cv::Mat::eye(3, 3, CV_64F);
    cv::Mat affine64;
    affine.convertTo(affine64, CV_64F);
    affine64.copyTo(rightForward(cv::Rect(0, 0, 3, 2)));
    const cv::Mat scale = ScaleToOutput(inputSize, outputSize);
    const std::size_t inliers = inlierMask.empty()
        ? left.size()
        : static_cast<std::size_t>(cv::countNonZero(inlierMask));
    return MakeEstimate(
        "affine_full",
        scale * leftForward,
        scale * rightForward,
        left,
        right,
        inliers);
}

ProfileEstimate EstimateUncalibrated(
    const std::vector<cv::Point2f>& left,
    const std::vector<cv::Point2f>& right,
    cv::Size inputSize,
    cv::Size outputSize)
{
    cv::Mat inlierMask;
    cv::Mat fundamental = cv::findFundamentalMat(
        left,
        right,
        cv::FM_RANSAC,
        kFundamentalRansacThresholdPixels,
        0.999,
        inlierMask);
    if (fundamental.empty() || fundamental.rows != 3 || fundamental.cols != 3) {
        throw std::runtime_error("findFundamentalMat failed");
    }

    std::vector<cv::Point2f> inlierLeft;
    std::vector<cv::Point2f> inlierRight;
    const auto* mask = inlierMask.empty() ? nullptr : inlierMask.ptr<std::uint8_t>();
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (!mask || mask[index] != 0) {
            inlierLeft.push_back(left[index]);
            inlierRight.push_back(right[index]);
        }
    }
    if (inlierLeft.size() < 8) {
        throw std::runtime_error("too few fundamental-matrix inliers");
    }

    cv::Mat leftForward;
    cv::Mat rightForward;
    if (!cv::stereoRectifyUncalibrated(
            inlierLeft,
            inlierRight,
            fundamental,
            inputSize,
            leftForward,
            rightForward)) {
        throw std::runtime_error("stereoRectifyUncalibrated failed");
    }
    leftForward.convertTo(leftForward, CV_64F);
    rightForward.convertTo(rightForward, CV_64F);
    const cv::Mat canvas = FitCommonCanvas(
        leftForward,
        rightForward,
        inputSize,
        outputSize);
    return MakeEstimate(
        "uncalibrated",
        canvas * leftForward,
        canvas * rightForward,
        left,
        right,
        inlierLeft.size());
}

std::map<std::string, ProfileEstimate> UpdateEstimatedProfiles(
    const std::deque<Observation>& observations,
    cv::Size imageSize)
{
    std::vector<cv::Point2f> left;
    std::vector<cv::Point2f> right;
    Concatenate(observations, left, right);
    if (left.size() < 8 || left.size() != right.size()) {
        throw std::runtime_error("insufficient paired checkerboard corners");
    }

    std::map<std::string, ProfileEstimate> profiles;
    try {
        profiles.emplace(
            "affine_vertical",
            EstimateAffineVertical(left, right, imageSize, imageSize));
    } catch (const std::exception& exception) {
        std::cerr << "[CALIB] affine_vertical failed: " << exception.what() << '\n';
    }
    try {
        profiles.emplace(
            "affine_full",
            EstimateAffineFull(left, right, imageSize, imageSize));
    } catch (const std::exception& exception) {
        std::cerr << "[CALIB] affine_full failed: " << exception.what() << '\n';
    }
    try {
        profiles.emplace(
            "uncalibrated",
            EstimateUncalibrated(left, right, imageSize, imageSize));
    } catch (const std::exception& exception) {
        std::cerr << "[CALIB] uncalibrated failed: " << exception.what() << '\n';
    }
    if (profiles.empty()) {
        throw std::runtime_error("all stereo calibration estimators failed");
    }
    return profiles;
}

void DrawHorizontalGuides(cv::Mat& pair, int singleImageWidth)
{
    const int spacing = std::max(40, pair.rows / 10);
    for (int y = spacing; y < pair.rows; y += spacing) {
        cv::line(
            pair,
            cv::Point(0, y),
            cv::Point(pair.cols - 1, y),
            cv::Scalar(0, 255, 255),
            1,
            cv::LINE_AA);
    }
    cv::line(
        pair,
        cv::Point(singleImageWidth, 0),
        cv::Point(singleImageWidth, pair.rows - 1),
        cv::Scalar(255, 255, 0),
        1,
        cv::LINE_AA);
}

cv::Mat BuildDisplay(
    const cv::Mat& left,
    const cv::Mat& right,
    const Observation& detected,
    cv::Size boardSize,
    const std::map<std::string, ProfileEstimate>& profiles,
    std::size_t observationCount,
    std::size_t minimumObservations)
{
    cv::Mat leftAnnotated = left.clone();
    cv::Mat rightAnnotated = right.clone();
    cv::drawChessboardCorners(
        leftAnnotated,
        boardSize,
        detected.left,
        !detected.left.empty());
    cv::drawChessboardCorners(
        rightAnnotated,
        boardSize,
        detected.right,
        !detected.right.empty());

    cv::Mat rawPair;
    cv::hconcat(leftAnnotated, rightAnnotated, rawPair);
    cv::putText(
        rawPair,
        "observations " + std::to_string(observationCount) + "/" +
            std::to_string(minimumObservations) +
            " (max 30) | profile affine_vertical | Q finish | R clear | Esc abort",
        cv::Point(20, 35),
        cv::FONT_HERSHEY_SIMPLEX,
        0.75,
        cv::Scalar(0, 255, 0),
        2,
        cv::LINE_AA);

    cv::Mat display = rawPair;
    const auto selected = profiles.find(kDefaultProfile);
    if (selected != profiles.end()) {
        cv::Mat rectifiedLeft;
        cv::Mat rectifiedRight;
        cv::warpPerspective(
            left,
            rectifiedLeft,
            selected->second.leftForward,
            left.size(),
            cv::INTER_LINEAR,
            cv::BORDER_CONSTANT);
        cv::warpPerspective(
            right,
            rectifiedRight,
            selected->second.rightForward,
            right.size(),
            cv::INTER_LINEAR,
            cv::BORDER_CONSTANT);
        cv::Mat rectifiedPair;
        cv::hconcat(rectifiedLeft, rectifiedRight, rectifiedPair);
        DrawHorizontalGuides(rectifiedPair, rectifiedLeft.cols);
        cv::putText(
            rectifiedPair,
            "affine_vertical | mean y error " +
                std::to_string(selected->second.meanVerticalError) +
                " px | median " +
                std::to_string(selected->second.medianVerticalError) + " px",
            cv::Point(20, 35),
            cv::FONT_HERSHEY_SIMPLEX,
            0.75,
            cv::Scalar(0, 255, 255),
            2,
            cv::LINE_AA);
        cv::vconcat(rawPair, rectifiedPair, display);
    }

    constexpr int maximumWidth = 1800;
    constexpr int maximumHeight = 1000;
    const double scale = std::min(
        1.0,
        std::min(
            static_cast<double>(maximumWidth) / static_cast<double>(display.cols),
            static_cast<double>(maximumHeight) / static_cast<double>(display.rows)));
    if (scale < 1.0) {
        cv::Mat resized;
        cv::resize(display, resized, cv::Size(), scale, scale, cv::INTER_AREA);
        return resized;
    }
    return display;
}

} // namespace

CheckerboardCalibrationResult RunCheckerboardStereoCalibration(
    IC4Ext::D3D12SyncedFrameQueue& inputQueue,
    const IC4Ext::D3D12BackendContext& backend,
    const CheckerboardCalibrationOptions& options,
    const std::optional<StereoCalibrationDocument>& initialDocument)
{
    CheckerboardCalibrationResult result;
    try {
        if (options.innerCornersColumns <= 1 || options.innerCornersRows <= 1) {
            throw std::invalid_argument(
                "checkerboard inner-corner dimensions must be greater than one");
        }
        if (options.minimumSamplePairs < 3) {
            throw std::invalid_argument(
                "checkerboard minimum observation count must be at least three");
        }

        IC4Ext::D3D12FrameReadback leftReadback;
        IC4Ext::D3D12FrameReadback rightReadback;
        if (!leftReadback.initialize(backend) || !rightReadback.initialize(backend)) {
            throw std::runtime_error("failed to initialize D3D12 frame readback");
        }

        const cv::Size boardSize(
            options.innerCornersColumns,
            options.innerCornersRows);
        std::deque<Observation> observations;
        std::map<std::string, ProfileEstimate> profiles;
        cv::Size imageSize;
        auto lastAccepted = Clock::now() -
            std::chrono::milliseconds(std::max(0, options.captureIntervalMs));
        bool qPrevious = false;
        bool escapePrevious = false;
        bool resetPrevious = false;

        constexpr const char* windowName =
            "Reference Stereo Calibration (affine_vertical)";
        cv::namedWindow(windowName, cv::WINDOW_NORMAL);

        for (;;) {
            auto set = inputQueue.waitPopLatestFor(std::chrono::milliseconds(100));
            if (!set) {
                const int key = cv::waitKey(1) & 0xff;
                if (key == 27 || KeyPressedEdge(VK_ESCAPE, escapePrevious)) {
                    result.aborted = true;
                    break;
                }
                continue;
            }

            const auto* leftFrame = FindCamera(*set, 0);
            const auto* rightFrame = FindCamera(*set, 1);
            if (!leftFrame || !rightFrame) continue;

            IC4Ext::CpuFrame leftCpu;
            IC4Ext::CpuFrame rightCpu;
            if (!leftReadback.readback(
                    leftFrame->frame,
                    IC4Ext::CpuFrameFormat::BGR8,
                    leftCpu) ||
                !rightReadback.readback(
                    rightFrame->frame,
                    IC4Ext::CpuFrameFormat::BGR8,
                    rightCpu)) {
                std::cerr << "[CALIB] frame readback failed; skipping pair\n";
                continue;
            }
            if (leftCpu.width != rightCpu.width ||
                leftCpu.height != rightCpu.height ||
                leftCpu.empty() ||
                rightCpu.empty()) {
                throw std::runtime_error(
                    "checkerboard calibration requires equal non-empty images");
            }

            imageSize = cv::Size(
                static_cast<int>(leftCpu.width),
                static_cast<int>(leftCpu.height));
            cv::Mat leftBgr(
                imageSize,
                CV_8UC3,
                leftCpu.data.data(),
                leftCpu.rowPitch);
            cv::Mat rightBgr(
                imageSize,
                CV_8UC3,
                rightCpu.data.data(),
                rightCpu.rowPitch);

            Observation detected;
            const bool found = DetectObservation(
                leftBgr,
                rightBgr,
                boardSize,
                detected);
            const auto now = Clock::now();
            const bool intervalReady =
                now - lastAccepted >= std::chrono::milliseconds(
                    std::max(0, options.captureIntervalMs));
            const bool poseChanged = observations.empty() ||
                MeanCornerMotion(observations.back(), detected) >=
                    options.minimumCornerMotionPixels;

            if (found && intervalReady && poseChanged) {
                observations.push_back(detected);
                while (observations.size() > kMaximumObservationCount) {
                    observations.pop_front();
                }
                lastAccepted = now;
                std::cout
                    << "[CALIB] accepted observation " << observations.size()
                    << "/" << options.minimumSamplePairs
                    << " (motion threshold "
                    << options.minimumCornerMotionPixels << " px)\n";

                if (observations.size() >= options.minimumSamplePairs) {
                    profiles = UpdateEstimatedProfiles(observations, imageSize);
                    for (const auto& [name, profile] : profiles) {
                        std::cout
                            << "[CALIB] profile=" << name
                            << " meanY=" << profile.meanVerticalError
                            << " px medianY=" << profile.medianVerticalError
                            << " px inliers=" << profile.inlierPoints
                            << "/" << profile.usedPoints << '\n';
                    }
                }
            }

            cv::imshow(
                windowName,
                BuildDisplay(
                    leftBgr,
                    rightBgr,
                    detected,
                    boardSize,
                    profiles,
                    observations.size(),
                    options.minimumSamplePairs));

            const int key = cv::waitKey(1) & 0xff;
            const bool reset = key == 'r' || key == 'R' ||
                KeyPressedEdge('R', resetPrevious);
            if (reset) {
                observations.clear();
                profiles.clear();
                lastAccepted = Clock::now() -
                    std::chrono::milliseconds(std::max(0, options.captureIntervalMs));
                std::cout << "[CALIB] observations cleared\n";
            }

            const bool quit = key == 'q' || key == 'Q' ||
                KeyPressedEdge('Q', qPrevious);
            if (quit) {
                if (observations.size() < options.minimumSamplePairs ||
                    profiles.find(kDefaultProfile) == profiles.end()) {
                    std::cout
                        << "[CALIB] Q ignored: valid " << kDefaultProfile
                        << " result requires " << options.minimumSamplePairs
                        << " observations; currently " << observations.size() << '\n';
                } else {
                    break;
                }
            }

            if (key == 27 ||
                KeyPressedEdge(VK_ESCAPE, escapePrevious) ||
                cv::getWindowProperty(windowName, cv::WND_PROP_VISIBLE) < 1.0) {
                result.aborted = true;
                break;
            }
        }

        cv::destroyWindow(windowName);
        result.capturedSamplePairs = observations.size();
        if (result.aborted) return result;
        if (imageSize.width <= 0 || imageSize.height <= 0) {
            throw std::runtime_error("calibration did not receive an image");
        }
        if (profiles.find(kDefaultProfile) == profiles.end()) {
            profiles = UpdateEstimatedProfiles(observations, imageSize);
        }
        const auto selected = profiles.find(kDefaultProfile);
        if (selected == profiles.end()) {
            throw std::runtime_error("affine_vertical calibration was not estimated");
        }

        StereoCalibrationDocument document = initialDocument
            ? *initialDocument
            : MakeEmptyDocument(
                static_cast<std::uint32_t>(imageSize.width),
                static_cast<std::uint32_t>(imageSize.height));
        document.format = "vdca.stereo_rectification";
        document.version = 1;
        document.defaultProfile = kDefaultProfile;
        document.sourceSize = {
            static_cast<std::uint32_t>(imageSize.width),
            static_cast<std::uint32_t>(imageSize.height)};
        document.calibrationInputSize = document.sourceSize;
        document.rectifiedOutputSize = document.sourceSize;
        document.resizeMode = "none";
        document.rightOrder = kRightOrder;
        document.samplingFilter = "linear";
        document.borderMode = "constant";
        document.borderRgba = {0.0f, 0.0f, 0.0f, 1.0f};
        document.profiles.clear();

        for (const auto& [name, estimate] : profiles) {
            CalibrationProfile profile;
            profile.method = estimate.method;
            profile.leftInverse = estimate.leftInverse;
            profile.rightInverse = estimate.rightInverse;
            document.profiles.emplace(name, std::move(profile));
        }

        result.ok = true;
        result.averageEpipolarErrorPixels = selected->second.meanVerticalError;
        result.document = std::move(document);
        std::cout
            << "[CALIB] selected profile=" << kDefaultProfile
            << " meanY=" << selected->second.meanVerticalError
            << " px medianY=" << selected->second.medianVerticalError
            << " px observations=" << observations.size() << '\n';
        return result;
    } catch (const std::exception& exception) {
        cv::destroyAllWindows();
        result.error = exception.what();
        return result;
    } catch (...) {
        cv::destroyAllWindows();
        result.error = "unknown checkerboard stereo calibration failure";
        return result;
    }
}

} // namespace DualIC4Varjo
