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
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace DualIC4Varjo {
namespace {

using Clock = std::chrono::steady_clock;

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
    document.defaultProfile = "checkerboard_uncalibrated";
    document.sourceSize = {width, height};
    document.calibrationInputSize = {width, height};
    document.rectifiedOutputSize = {width, height};
    document.resizeMode = "none";
    document.rightOrder = "same";
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
                throw std::runtime_error(
                    "OpenCV returned a non-finite homography value");
            }
            result.rows[static_cast<std::size_t>(row * 3 + column)] = value;
        }
    }
    return result;
}

cv::Mat ToCvMatrix(const CalibrationHomography& homography)
{
    cv::Mat result(3, 3, CV_64F);
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            result.at<double>(row, column) =
                homography.rows[static_cast<std::size_t>(row * 3 + column)];
        }
    }
    return result;
}

struct SolveResult {
    bool ok = false;
    cv::Mat leftForward;
    cv::Mat rightForward;
    CalibrationHomography leftInverse;
    CalibrationHomography rightInverse;
    double averageVerticalError = 0.0;
    std::string error;
};

SolveResult SolveRectification(
    const std::vector<std::vector<cv::Point2f>>& leftSamples,
    const std::vector<std::vector<cv::Point2f>>& rightSamples,
    cv::Size imageSize)
{
    SolveResult result;
    if (leftSamples.size() != rightSamples.size() || leftSamples.size() < 3) {
        result.error = "at least three paired checkerboard samples are required";
        return result;
    }

    std::vector<cv::Point2f> leftPoints;
    std::vector<cv::Point2f> rightPoints;
    for (std::size_t index = 0; index < leftSamples.size(); ++index) {
        if (leftSamples[index].size() != rightSamples[index].size()) continue;
        leftPoints.insert(
            leftPoints.end(),
            leftSamples[index].begin(),
            leftSamples[index].end());
        rightPoints.insert(
            rightPoints.end(),
            rightSamples[index].begin(),
            rightSamples[index].end());
    }
    if (leftPoints.size() < 16 || leftPoints.size() != rightPoints.size()) {
        result.error = "insufficient paired checkerboard corners";
        return result;
    }

    cv::Mat inlierMask;
    const cv::Mat fundamental = cv::findFundamentalMat(
        leftPoints,
        rightPoints,
        cv::FM_RANSAC,
        1.5,
        0.995,
        inlierMask);
    if (fundamental.empty() || fundamental.rows != 3 || fundamental.cols != 3) {
        result.error = "findFundamentalMat failed";
        return result;
    }

    std::vector<cv::Point2f> leftInliers;
    std::vector<cv::Point2f> rightInliers;
    leftInliers.reserve(leftPoints.size());
    rightInliers.reserve(rightPoints.size());
    for (std::size_t index = 0; index < leftPoints.size(); ++index) {
        const bool inlier = inlierMask.empty() ||
            inlierMask.at<std::uint8_t>(static_cast<int>(index), 0) != 0;
        if (!inlier) continue;
        leftInliers.push_back(leftPoints[index]);
        rightInliers.push_back(rightPoints[index]);
    }
    if (leftInliers.size() < 16) {
        result.error = "too few RANSAC inliers for stereo rectification";
        return result;
    }

    cv::Mat leftForward;
    cv::Mat rightForward;
    const bool rectified = cv::stereoRectifyUncalibrated(
        leftInliers,
        rightInliers,
        fundamental,
        imageSize,
        leftForward,
        rightForward,
        5.0);
    if (!rectified || leftForward.empty() || rightForward.empty()) {
        result.error = "stereoRectifyUncalibrated failed";
        return result;
    }

    cv::Mat leftInverse;
    cv::Mat rightInverse;
    if (cv::invert(leftForward, leftInverse, cv::DECOMP_SVD) == 0.0 ||
        cv::invert(rightForward, rightInverse, cv::DECOMP_SVD) == 0.0) {
        result.error = "rectification homography inversion failed";
        return result;
    }

    std::vector<cv::Point2f> rectifiedLeft;
    std::vector<cv::Point2f> rectifiedRight;
    cv::perspectiveTransform(leftInliers, rectifiedLeft, leftForward);
    cv::perspectiveTransform(rightInliers, rectifiedRight, rightForward);
    double verticalError = 0.0;
    for (std::size_t index = 0; index < rectifiedLeft.size(); ++index) {
        verticalError += std::abs(
            static_cast<double>(rectifiedLeft[index].y) -
            static_cast<double>(rectifiedRight[index].y));
    }

    result.ok = true;
    result.leftForward = std::move(leftForward);
    result.rightForward = std::move(rightForward);
    result.leftInverse = ToHomography(leftInverse);
    result.rightInverse = ToHomography(rightInverse);
    result.averageVerticalError = verticalError /
        static_cast<double>(std::max<std::size_t>(1, rectifiedLeft.size()));
    return result;
}

bool DetectCheckerboard(
    const cv::Mat& bgr,
    cv::Size boardSize,
    std::vector<cv::Point2f>& corners)
{
    cv::Mat gray;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    const bool found = cv::findChessboardCorners(
        gray,
        boardSize,
        corners,
        cv::CALIB_CB_ADAPTIVE_THRESH |
            cv::CALIB_CB_NORMALIZE_IMAGE |
            cv::CALIB_CB_FAST_CHECK);
    if (!found) {
        corners.clear();
        return false;
    }

    cv::cornerSubPix(
        gray,
        corners,
        cv::Size(11, 11),
        cv::Size(-1, -1),
        cv::TermCriteria(
            cv::TermCriteria::EPS | cv::TermCriteria::COUNT,
            30,
            0.01));
    return true;
}

double AverageCornerMotion(
    const std::vector<cv::Point2f>& current,
    const std::vector<cv::Point2f>& previous)
{
    if (current.empty() || current.size() != previous.size()) {
        return std::numeric_limits<double>::infinity();
    }
    double sum = 0.0;
    for (std::size_t index = 0; index < current.size(); ++index) {
        sum += cv::norm(current[index] - previous[index]);
    }
    return sum / static_cast<double>(current.size());
}

cv::Mat BuildDisplay(
    const cv::Mat& left,
    const cv::Mat& right,
    const std::vector<cv::Point2f>& leftCorners,
    const std::vector<cv::Point2f>& rightCorners,
    cv::Size boardSize,
    const std::optional<SolveResult>& provisional,
    std::size_t sampleCount,
    std::size_t minimumSamples)
{
    cv::Mat leftAnnotated = left.clone();
    cv::Mat rightAnnotated = right.clone();
    cv::drawChessboardCorners(
        leftAnnotated, boardSize, leftCorners, !leftCorners.empty());
    cv::drawChessboardCorners(
        rightAnnotated, boardSize, rightCorners, !rightCorners.empty());

    cv::Mat rawPair;
    cv::hconcat(leftAnnotated, rightAnnotated, rawPair);
    const std::string status =
        "samples " + std::to_string(sampleCount) + "/" +
        std::to_string(minimumSamples) +
        " | auto capture | Q finish | R clear | Esc abort";
    cv::putText(
        rawPair,
        status,
        cv::Point(20, 35),
        cv::FONT_HERSHEY_SIMPLEX,
        0.8,
        cv::Scalar(0, 255, 0),
        2,
        cv::LINE_AA);

    cv::Mat display = rawPair;
    if (provisional && provisional->ok) {
        cv::Mat rectifiedLeft;
        cv::Mat rectifiedRight;
        cv::warpPerspective(
            left,
            rectifiedLeft,
            provisional->leftForward,
            left.size(),
            cv::INTER_LINEAR,
            cv::BORDER_CONSTANT);
        cv::warpPerspective(
            right,
            rectifiedRight,
            provisional->rightForward,
            right.size(),
            cv::INTER_LINEAR,
            cv::BORDER_CONSTANT);
        cv::Mat rectifiedPair;
        cv::hconcat(rectifiedLeft, rectifiedRight, rectifiedPair);
        cv::putText(
            rectifiedPair,
            "provisional rectification | avg vertical error " +
                std::to_string(provisional->averageVerticalError) + " px",
            cv::Point(20, 35),
            cv::FONT_HERSHEY_SIMPLEX,
            0.8,
            cv::Scalar(0, 255, 255),
            2,
            cv::LINE_AA);
        cv::vconcat(rawPair, rectifiedPair, display);
    }

    const int maxWidth = 1800;
    const int maxHeight = 1000;
    const double scale = std::min(
        1.0,
        std::min(
            static_cast<double>(maxWidth) / static_cast<double>(display.cols),
            static_cast<double>(maxHeight) / static_cast<double>(display.rows)));
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
        if (options.innerCornersColumns <= 1 ||
            options.innerCornersRows <= 1) {
            throw std::invalid_argument(
                "checkerboard inner-corner dimensions must be greater than one");
        }
        if (options.minimumSamplePairs < 3) {
            throw std::invalid_argument(
                "checkerboard minimum sample count must be at least three");
        }

        IC4Ext::D3D12FrameReadback leftReadback;
        IC4Ext::D3D12FrameReadback rightReadback;
        if (!leftReadback.initialize(backend) ||
            !rightReadback.initialize(backend)) {
            throw std::runtime_error("failed to initialize D3D12 frame readback");
        }

        const cv::Size boardSize(
            options.innerCornersColumns,
            options.innerCornersRows);
        std::vector<std::vector<cv::Point2f>> leftSamples;
        std::vector<std::vector<cv::Point2f>> rightSamples;
        std::vector<cv::Point2f> lastCapturedLeft;
        std::vector<cv::Point2f> lastCapturedRight;
        auto lastCapture = Clock::now() -
            std::chrono::milliseconds(options.captureIntervalMs);
        std::optional<SolveResult> provisional;
        cv::Size imageSize;
        bool qPrevious = false;
        bool escapePrevious = false;
        bool resetPrevious = false;

        constexpr const char* kWindowName =
            "OpenCV Checkerboard Stereo Calibration";
        cv::namedWindow(kWindowName, cv::WINDOW_NORMAL);

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
                leftCpu.empty() || rightCpu.empty()) {
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

            std::vector<cv::Point2f> leftCorners;
            std::vector<cv::Point2f> rightCorners;
            const bool leftFound = DetectCheckerboard(
                leftBgr, boardSize, leftCorners);
            const bool rightFound = DetectCheckerboard(
                rightBgr, boardSize, rightCorners);

            const auto now = Clock::now();
            const bool intervalReady =
                now - lastCapture >=
                    std::chrono::milliseconds(options.captureIntervalMs);
            const double leftMotion = AverageCornerMotion(
                leftCorners, lastCapturedLeft);
            const double rightMotion = AverageCornerMotion(
                rightCorners, lastCapturedRight);
            const bool poseChanged = leftSamples.empty() ||
                std::max(leftMotion, rightMotion) >=
                    options.minimumCornerMotionPixels;

            if (leftFound && rightFound && intervalReady && poseChanged) {
                leftSamples.push_back(leftCorners);
                rightSamples.push_back(rightCorners);
                lastCapturedLeft = leftCorners;
                lastCapturedRight = rightCorners;
                lastCapture = now;
                std::cout
                    << "[CALIB] checkerboard sample "
                    << leftSamples.size() << " captured\n";

                if (leftSamples.size() >= 3) {
                    SolveResult solved = SolveRectification(
                        leftSamples,
                        rightSamples,
                        imageSize);
                    if (solved.ok) provisional = std::move(solved);
                }
            }

            cv::imshow(
                kWindowName,
                BuildDisplay(
                    leftBgr,
                    rightBgr,
                    leftCorners,
                    rightCorners,
                    boardSize,
                    provisional,
                    leftSamples.size(),
                    options.minimumSamplePairs));

            const int key = cv::waitKey(1) & 0xff;
            const bool reset = key == 'r' || key == 'R' ||
                KeyPressedEdge('R', resetPrevious);
            if (reset) {
                leftSamples.clear();
                rightSamples.clear();
                lastCapturedLeft.clear();
                lastCapturedRight.clear();
                provisional.reset();
                std::cout << "[CALIB] captured samples cleared\n";
            }

            const bool quit = key == 'q' || key == 'Q' ||
                KeyPressedEdge('Q', qPrevious);
            if (quit) {
                if (leftSamples.size() < options.minimumSamplePairs) {
                    std::cout
                        << "[CALIB] Q ignored: "
                        << options.minimumSamplePairs
                        << " samples required, currently "
                        << leftSamples.size() << '\n';
                } else {
                    break;
                }
            }

            if (key == 27 || KeyPressedEdge(VK_ESCAPE, escapePrevious) ||
                cv::getWindowProperty(kWindowName, cv::WND_PROP_VISIBLE) < 1.0) {
                result.aborted = true;
                break;
            }
        }

        cv::destroyWindow(kWindowName);
        result.capturedSamplePairs = leftSamples.size();
        if (result.aborted) return result;

        SolveResult final = SolveRectification(
            leftSamples,
            rightSamples,
            imageSize);
        if (!final.ok) {
            throw std::runtime_error(final.error);
        }

        StereoCalibrationDocument document = initialDocument
            ? *initialDocument
            : MakeEmptyDocument(
                static_cast<std::uint32_t>(imageSize.width),
                static_cast<std::uint32_t>(imageSize.height));
        document.format = "vdca.stereo_rectification";
        document.version = 1;
        document.defaultProfile = "checkerboard_uncalibrated";
        document.sourceSize = {
            static_cast<std::uint32_t>(imageSize.width),
            static_cast<std::uint32_t>(imageSize.height)};
        document.calibrationInputSize = document.sourceSize;
        document.rectifiedOutputSize = document.sourceSize;
        document.resizeMode = "none";
        document.rightOrder = "same";
        document.samplingFilter = "linear";
        document.borderMode = "constant";
        document.borderRgba = {0.0f, 0.0f, 0.0f, 1.0f};

        CalibrationProfile profile;
        profile.method = "opencv_checkerboard_stereoRectifyUncalibrated";
        profile.leftInverse = final.leftInverse;
        profile.rightInverse = final.rightInverse;
        document.profiles[document.defaultProfile] = std::move(profile);

        result.ok = true;
        result.averageEpipolarErrorPixels = final.averageVerticalError;
        result.document = std::move(document);
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
