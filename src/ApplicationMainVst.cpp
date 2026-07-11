#include "VstLoadServiceHook.hpp"

#define main DualIC4VarjoBaseMain
#define markRendered(...)                                                     \
    markRendered(__VA_ARGS__),                                                \
        DualIC4Varjo::VstLoadServiceHook::ensureStarted(                      \
            session->shared())
#include "ApplicationMainPlaneControl.cpp"
#undef markRendered
#undef main

#include <exception>
#include <iostream>

int main(int argc, char** argv)
{
    DualIC4Varjo::VstLoadServiceHook::configure(argc, argv);

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

    DualIC4Varjo::VstLoadServiceHook::stop();
    std::cout
        << "[VST] leftReceived="
        << DualIC4Varjo::VstLoadServiceHook::leftReceivedCount()
        << " rightReceived="
        << DualIC4Varjo::VstLoadServiceHook::rightReceivedCount()
        << " leftProcessed="
        << DualIC4Varjo::VstLoadServiceHook::leftProcessedCount()
        << " rightProcessed="
        << DualIC4Varjo::VstLoadServiceHook::rightProcessedCount()
        << " dropped="
        << DualIC4Varjo::VstLoadServiceHook::droppedCount()
        << " writeFailures="
        << DualIC4Varjo::VstLoadServiceHook::writeFailureCount()
        << " output="
        << DualIC4Varjo::VstLoadServiceHook::outputDirectory().string()
        << '\n';

    const std::string error =
        DualIC4Varjo::VstLoadServiceHook::lastError();
    if (!error.empty()) {
        std::cerr << "[VST] last error: " << error << '\n';
    }
    return result;
}
