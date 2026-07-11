#pragma once

// Include this before defining the markRendered macro in the VST entry point.
// The include guard prevents the macro from rewriting the class declaration.
#include "StereoDisplayTextureRing.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include <Varjo.h>

namespace DualIC4Varjo {

class VstLoadServiceHook {
public:
    static void configure(int argc, char** argv);

    static void ensureStarted(
        const std::shared_ptr<varjo_Session>& session) noexcept;

    static void stop() noexcept;

    static std::filesystem::path outputDirectory();
    static std::string lastError();
    static std::uint64_t leftReceivedCount() noexcept;
    static std::uint64_t rightReceivedCount() noexcept;
    static std::uint64_t leftProcessedCount() noexcept;
    static std::uint64_t rightProcessedCount() noexcept;
    static std::uint64_t droppedCount() noexcept;
    static std::uint64_t writeFailureCount() noexcept;
};

} // namespace DualIC4Varjo
