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
    std::int64_t hostReceivedSteadyUs = 0;
    std::int64_t hostReceivedUnixUs = 0;
    std::string hostReceivedLocalIso8601;
    int width = 0;
    int height = 0;
    int dxgiFormat = 0;
    IC4Ext::FrameChunkMetadata chunk;
};

struct RenderedFrameMetadataRow {
    std::uint64_t renderRowIndex = 0;
    std::int64_t renderSubmitUnixUs = 0;
    std::string renderSubmitLocalIso8601;
    std::int64_t renderSubmitSteadyUs = 0;
    bool newFrameFromQueue = false;
    bool submitOk = false;

    std::uint64_t syncGroupId = 0;
    std::int64_t syncEmittedSteadyUs = 0;
    std::int64_t syncEmittedUnixUs = 0;
    std::string syncTimestampSource;
    std::int64_t syncTimestampDiffNs = 0;
    double syncTimestampDiffMs = 0.0;
    std::int64_t hostReceivedDiffUs = 0;
    double hostReceivedDiffMs = 0.0;

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
