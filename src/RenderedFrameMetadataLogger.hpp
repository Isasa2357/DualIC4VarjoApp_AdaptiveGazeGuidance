#pragma once

#include <IC4Ext/IC4Ext.hpp>
#include <ThreadKit/Queues/BlockingQueue.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace DualIC4Varjo {

struct CameraFrameMetadataRow {
    std::uint64_t frameNumber = 0;
    std::uint64_t deviceTimestampNs = 0;
    std::int64_t hostReceivedUnixUs = 0;
    int width = 0;
    int height = 0;
};

struct RenderedFrameMetadataRow {
    std::uint64_t renderRowIndex = 0;
    std::int64_t renderSubmitUnixUs = 0;
    std::string renderSubmitLocalIso8601;
    bool newFrameFromQueue = false;
    bool submitOk = false;

    bool planeMoved = false;
    std::string planePlacementMode;
    float planeX = 0.0f;
    float planeY = 0.0f;
    float planeZ = 0.0f;

    std::uint64_t syncGroupId = 0;
    std::int64_t syncEmittedUnixUs = 0;
    std::string syncTimestampSource;
    std::int64_t syncTimestampDiffNs = 0;
    std::int64_t hostReceivedDiffUs = 0;

    CameraFrameMetadataRow left;
    CameraFrameMetadataRow right;

    std::size_t displaySlotIndex = 0;
    IC4Ext::FrameSyncStats syncStats;
    ThreadKit::Queues::QueueStats syncedQueueStats;
    IC4Ext::CameraThreadStats leftCameraStats;
    IC4Ext::CameraThreadStats rightCameraStats;
};

class RenderedFrameMetadataLogger {
public:
    RenderedFrameMetadataLogger() = default;
    ~RenderedFrameMetadataLogger();

    RenderedFrameMetadataLogger(const RenderedFrameMetadataLogger&) = delete;
    RenderedFrameMetadataLogger& operator=(const RenderedFrameMetadataLogger&) = delete;

    bool start(const std::filesystem::path& path);
    bool enqueue(RenderedFrameMetadataRow row);
    void stop();

    std::string lastError() const;
    const std::filesystem::path& path() const noexcept { return path_; }

private:
    void workerLoop();
    void writeHeader();
    void writeRow(const RenderedFrameMetadataRow& row);
    void writeRecord(const std::string& record);
    void setError(std::string message);

    std::filesystem::path path_;
    std::ofstream output_;
    ThreadKit::Queues::BlockingQueue<RenderedFrameMetadataRow> queue_;
    std::thread worker_;
    bool started_ = false;
    mutable std::mutex errorMutex_;
    std::string lastError_;
};

} // namespace DualIC4Varjo
