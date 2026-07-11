#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace DualIC4Varjo {

class KeyInputService {
public:
    explicit KeyInputService(std::filesystem::path outputPath);
    ~KeyInputService();

    KeyInputService(const KeyInputService&) = delete;
    KeyInputService& operator=(const KeyInputService&) = delete;

    bool start();
    void stop() noexcept;

    std::filesystem::path outputPath() const;
    std::string lastError() const;
    std::uint64_t eventCount() const noexcept;

private:
    void workerMain() noexcept;
    void setError(std::string message) noexcept;

    std::filesystem::path outputPath_;
    std::ofstream output_;
    std::thread worker_;
    std::atomic<bool> stopRequested_{true};
    std::atomic<std::uint64_t> eventCount_{0};
    mutable std::mutex stateMutex_;
    std::string lastError_;
};

} // namespace DualIC4Varjo
