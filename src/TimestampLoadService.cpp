#include "TimestampLoadService.hpp"
#include "ExperimentOutput.hpp"

#include <VarjoToolkit/Core/VarjoSession.hpp>
#include <VarjoToolkit/Utilities/VarjoTimestampMapping.hpp>

#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

namespace DualIC4Varjo {
namespace {

constexpr auto kTimestampSamplePeriod = std::chrono::milliseconds(100);

std::string FindArgumentValue(
    int argc,
    char** argv,
    const std::string& option,
    const std::string& fallback)
{
    for (int index = 1; index + 1 < argc; ++index) {
        if (argv[index] && option == argv[index]) {
            return argv[index + 1] ? argv[index + 1] : fallback;
        }
    }
    return fallback;
}

std::string SanitizeFilename(std::string value)
{
    for (char& character : value) {
        switch (character) {
        case '<': case '>': case ':': case '"': case '/':
        case '\\': case '|': case '?': case '*':
            character = '_';
            break;
        default:
            break;
        }
    }
    return value.empty() ? std::string("timestamp_test") : value;
}

std::filesystem::path ResolveOutputPath(int argc, char** argv)
{
    if (const auto active = ActiveExperimentOutputLayout()) {
        return active->directory /
               (active->resolvedProjectName + "_varjo_timestamp_mapping.csv");
    }

    const std::filesystem::path baseDirectory =
        FindArgumentValue(argc, argv, "--dir", "logs");
    const std::string project = SanitizeFilename(
        FindArgumentValue(argc, argv, "--project", "timestamp_test"));
    return baseDirectory /
           "service_load" /
           (project + "_varjo_timestamp_mapping.csv");
}

} // namespace

TimestampLoadService::TimestampLoadService(int argc, char** argv)
    : outputPath_(ResolveOutputPath(argc, argv))
{
}

TimestampLoadService::~TimestampLoadService()
{
    stop();
}

bool TimestampLoadService::start()
{
    stop();

    try {
        std::error_code error;
        std::filesystem::create_directories(
            outputPath_.parent_path(),
            error);
        if (error) {
            setError(
                "failed to create timestamp output directory: " +
                error.message());
            return false;
        }

        output_.open(outputPath_, std::ios::out | std::ios::trunc);
        if (!output_.is_open()) {
            setError(
                "failed to open timestamp CSV: " +
                outputPath_.string());
            return false;
        }

        output_
            << "row_index,"
            << "sample_begin_system_unix_us,"
            << "sample_end_system_unix_us,"
            << "sample_call_duration_us,"
            << "sample_valid,"
            << "varjo_timestamp_ns,"
            << "varjo_timestamp_unix_ns,"
            << "varjo_timestamp_unix_us,"
            << "sample_system_timestamp_unix_us,"
            << "delta_varjo_unix_minus_system_us\n";

        session_ = std::make_unique<VarjoSession>();
        if (!session_->valid() && !session_->initialize()) {
            setError(
                "timestamp Varjo session initialization failed: " +
                session_->lastError());
            output_.close();
            session_.reset();
            return false;
        }

        mapping_ = std::make_unique<VarjoTimestampMapping>(
            session_->shared());
        sampleCount_.store(0, std::memory_order_release);
        failedSampleCount_.store(0, std::memory_order_release);
        stopRequested_.store(false, std::memory_order_release);
        worker_ = std::thread(&TimestampLoadService::workerMain, this);

        std::cout
            << "[TIMESTAMP] service started at 10 Hz\n"
            << "[TIMESTAMP] CSV: "
            << outputPath_.string()
            << '\n';
        return true;
    } catch (const std::exception& exception) {
        setError(exception.what());
        stop();
        return false;
    }
}

void TimestampLoadService::stop() noexcept
{
    stopRequested_.store(true, std::memory_order_release);
    if (worker_.joinable()) {
        try {
            worker_.join();
        } catch (...) {
        }
    }

    if (output_.is_open()) {
        output_.flush();
        output_.close();
    }
    mapping_.reset();
    session_.reset();
}

std::string TimestampLoadService::lastError() const
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    return lastError_;
}

std::filesystem::path TimestampLoadService::outputPath() const
{
    return outputPath_;
}

std::uint64_t TimestampLoadService::sampleCount() const noexcept
{
    return sampleCount_.load(std::memory_order_acquire);
}

std::uint64_t TimestampLoadService::failedSampleCount() const noexcept
{
    return failedSampleCount_.load(std::memory_order_acquire);
}

void TimestampLoadService::workerMain() noexcept
{
    using clock = std::chrono::steady_clock;
    using system_clock = std::chrono::system_clock;

    auto nextSample = clock::now();
    try {
        while (!stopRequested_.load(std::memory_order_acquire)) {
            nextSample += kTimestampSamplePeriod;

            const auto beginSystem = system_clock::now();
            const auto beginSteady = clock::now();
            const auto sample = mapping_->sampleCurrentMapping();
            const auto endSteady = clock::now();
            const auto endSystem = system_clock::now();

            const auto beginUnixUs =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    beginSystem.time_since_epoch()).count();
            const auto endUnixUs =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    endSystem.time_since_epoch()).count();
            const auto durationUs =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    endSteady - beginSteady).count();

            const std::uint64_t rowIndex =
                sampleCount_.fetch_add(1, std::memory_order_acq_rel);
            if (!sample.valid) {
                failedSampleCount_.fetch_add(
                    1,
                    std::memory_order_acq_rel);
            }

            output_
                << rowIndex << ','
                << beginUnixUs << ','
                << endUnixUs << ','
                << durationUs << ','
                << (sample.valid ? 1 : 0) << ','
                << sample.varjoTimestamp << ','
                << sample.varjoTimestampUnixNs << ','
                << sample.varjoTimestampUnixUs << ','
                << sample.systemTimestampUnixUs << ','
                << sample.deltaVarjoUnixMinusSystemUs
                << '\n';

            if (!output_) {
                throw std::runtime_error(
                    "timestamp CSV write failed");
            }

            std::this_thread::sleep_until(nextSample);
            const auto now = clock::now();
            if (now > nextSample + kTimestampSamplePeriod) {
                nextSample = now;
            }
        }
    } catch (const std::exception& exception) {
        setError(exception.what());
        stopRequested_.store(true, std::memory_order_release);
    } catch (...) {
        setError("unknown timestamp worker failure");
        stopRequested_.store(true, std::memory_order_release);
    }
}

void TimestampLoadService::setError(std::string message) noexcept
{
    try {
        std::lock_guard<std::mutex> lock(stateMutex_);
        lastError_ = std::move(message);
    } catch (...) {
    }
}

} // namespace DualIC4Varjo
