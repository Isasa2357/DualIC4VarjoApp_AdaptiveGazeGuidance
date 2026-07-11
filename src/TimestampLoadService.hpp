#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

class VarjoSession;
class VarjoTimestampMapping;

namespace DualIC4Varjo {

class TimestampLoadService {
public:
    TimestampLoadService(int argc, char** argv);
    ~TimestampLoadService();

    TimestampLoadService(const TimestampLoadService&) = delete;
    TimestampLoadService& operator=(const TimestampLoadService&) = delete;

    bool start();
    void stop() noexcept;

    std::string lastError() const;
    std::filesystem::path outputPath() const;
    std::uint64_t sampleCount() const noexcept;
    std::uint64_t failedSampleCount() const noexcept;

private:
    void workerMain() noexcept;
    void setError(std::string message) noexcept;

    std::filesystem::path outputPath_;
    std::unique_ptr<VarjoSession> session_;
    std::unique_ptr<VarjoTimestampMapping> mapping_;
    std::ofstream output_;
    std::thread worker_;
    std::atomic<bool> stopRequested_{true};
    std::atomic<std::uint64_t> sampleCount_{0};
    std::atomic<std::uint64_t> failedSampleCount_{0};
    mutable std::mutex stateMutex_;
    std::string lastError_;
};

} // namespace DualIC4Varjo
