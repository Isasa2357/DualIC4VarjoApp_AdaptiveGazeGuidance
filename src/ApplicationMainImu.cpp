#include "ImuLoadServiceHook.hpp"

#define main DualIC4VarjoBaseMain
#define markRendered(...)                                                     \
    markRendered(__VA_ARGS__),                                                \
        DualIC4Varjo::ImuLoadServiceHook::submit(                             \
            d3dBackend.frameInfoSnapshot(),                                   \
            session->shared())
#include "ApplicationMainPlaneControl.cpp"
#undef markRendered
#undef main

#include <exception>
#include <iostream>

int main(int argc, char** argv)
{
    DualIC4Varjo::ImuLoadServiceHook::configure(argc, argv);

    int result = 1;
    try {
        result = DualIC4VarjoBaseMain(argc, argv);
    } catch (const std::exception& exception) {
        std::cerr
            << "Unhandled application exception: "
            << exception.what()
            << '\n';
        result = 1;
    } catch (...) {
        std::cerr << "Unhandled unknown application exception.\n";
        result = 1;
    }

    DualIC4Varjo::ImuLoadServiceHook::stop();
    std::cout
        << "[IMU] received="
        << DualIC4Varjo::ImuLoadServiceHook::receivedCount()
        << " processed="
        << DualIC4Varjo::ImuLoadServiceHook::processedCount()
        << " written="
        << DualIC4Varjo::ImuLoadServiceHook::writtenCount()
        << " dropped="
        << DualIC4Varjo::ImuLoadServiceHook::droppedCount()
        << " CSV="
        << DualIC4Varjo::ImuLoadServiceHook::outputPath().string()
        << '\n';

    const std::string error =
        DualIC4Varjo::ImuLoadServiceHook::lastError();
    if (!error.empty()) {
        std::cerr << "[IMU] last error: " << error << '\n';
    }
    return result;
}
