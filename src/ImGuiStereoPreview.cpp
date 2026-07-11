#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "ImGuiStereoPreview.hpp"

#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam);

namespace DualIC4Varjo {
namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT kBackBufferCount = 2;
constexpr UINT kSrvDescriptorCount = 64;
constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

std::string HResultText(HRESULT hr)
{
    std::ostringstream stream;
    stream << "HRESULT=0x"
           << std::hex
           << std::uppercase
           << static_cast<unsigned long>(hr);
    return stream.str();
}

void ThrowIfFailed(HRESULT hr, const char* operation)
{
    if (SUCCEEDED(hr)) return;
    throw std::runtime_error(
        std::string(operation ? operation : "D3D12 operation") +
        " failed: " +
        HResultText(hr));
}

std::wstring Utf8ToWide(const std::string& text)
{
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0);
    if (length <= 0) {
        return std::wstring(text.begin(), text.end());
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        result.data(),
        length);
    return result;
}

const IC4Ext::D3D12CameraFrame* FindFrame(
    const IC4Ext::D3D12SyncedFrameSet& frameSet,
    std::uint32_t cameraIndex)
{
    for (const auto& indexed : frameSet.frames) {
        if (indexed.cameraIndex == cameraIndex) {
            return &indexed.frame;
        }
    }
    return nullptr;
}

struct DescriptorHandle {
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
};

class DescriptorAllocator {
public:
    void initialize(ID3D12Device* device, ID3D12DescriptorHeap* heap)
    {
        if (!device || !heap) {
            throw std::invalid_argument(
                "DescriptorAllocator requires a device and heap");
        }

        heap_ = heap;
        const auto desc = heap->GetDesc();
        increment_ = device->GetDescriptorHandleIncrementSize(desc.Type);
        startCpu_ = heap->GetCPUDescriptorHandleForHeapStart();
        startGpu_ = heap->GetGPUDescriptorHandleForHeapStart();

        freeIndices_.clear();
        freeIndices_.reserve(desc.NumDescriptors);
        for (UINT index = desc.NumDescriptors; index > 0; --index) {
            freeIndices_.push_back(index - 1);
        }
    }

    DescriptorHandle allocate()
    {
        if (freeIndices_.empty()) {
            throw std::runtime_error(
                "ImGui SRV descriptor heap is exhausted");
        }

        const UINT index = freeIndices_.back();
        freeIndices_.pop_back();

        DescriptorHandle result;
        result.cpu.ptr =
            startCpu_.ptr +
            static_cast<SIZE_T>(index) * increment_;
        result.gpu.ptr =
            startGpu_.ptr +
            static_cast<UINT64>(index) * increment_;
        return result;
    }

    void free(DescriptorHandle handle)
    {
        if (handle.cpu.ptr == 0 || increment_ == 0) return;
        const SIZE_T delta = handle.cpu.ptr - startCpu_.ptr;
        freeIndices_.push_back(
            static_cast<UINT>(delta / increment_));
    }

private:
    ID3D12DescriptorHeap* heap_ = nullptr;
    UINT increment_ = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE startCpu_{};
    D3D12_GPU_DESCRIPTOR_HANDLE startGpu_{};
    std::vector<UINT> freeIndices_;
};

struct PreviewFrame {
    ComPtr<ID3D12Resource> left;
    ComPtr<ID3D12Resource> right;
    DXGI_FORMAT leftFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT rightFormat = DXGI_FORMAT_UNKNOWN;
    UINT leftWidth = 0;
    UINT leftHeight = 0;
    UINT rightWidth = 0;
    UINT rightHeight = 0;
    std::uint64_t syncGroupId = 0;
    std::uint64_t leftFrameNumber = 0;
    std::uint64_t rightFrameNumber = 0;

    bool valid() const noexcept
    {
        return left &&
               right &&
               leftWidth > 0 &&
               leftHeight > 0 &&
               rightWidth > 0 &&
               rightHeight > 0;
    }
};

struct DesktopFrameContext {
    ComPtr<ID3D12CommandAllocator> commandAllocator;
    UINT64 fenceValue = 0;
    DescriptorHandle leftSrv;
    DescriptorHandle rightSrv;
    ComPtr<ID3D12Resource> leftKeepAlive;
    ComPtr<ID3D12Resource> rightKeepAlive;
};

} // namespace

struct ImGuiStereoPreview::Impl {
    Impl(
        std::shared_ptr<D3D12CoreLib::D3D12Core> coreValue,
        std::shared_ptr<IC4Ext::D3D12SyncedFrameQueue> inputQueueValue,
        ImGuiStereoPreviewConfig configValue)
        : core(std::move(coreValue))
        , inputQueue(std::move(inputQueueValue))
        , config(std::move(configValue))
    {
        if (!core) {
            throw std::invalid_argument(
                "ImGuiStereoPreview requires D3D12Core");
        }
        if (!inputQueue) {
            throw std::invalid_argument(
                "ImGuiStereoPreview requires an input queue");
        }
        if (config.windowWidth <= 0 || config.windowHeight <= 0) {
            throw std::invalid_argument(
                "ImGuiStereoPreview window size must be positive");
        }
    }

    ~Impl()
    {
        stopAndJoin();
    }

    bool start()
    {
        std::unique_lock<std::mutex> lock(startupMutex);
        if (worker.joinable()) return startupSucceeded;

        stopRequested.store(false, std::memory_order_release);
        startupComplete = false;
        startupSucceeded = false;
        running.store(true, std::memory_order_release);

        try {
            worker = std::thread([this] { threadMain(); });
        } catch (...) {
            running.store(false, std::memory_order_release);
            throw;
        }

        startupCv.wait(lock, [this] { return startupComplete; });
        const bool succeeded = startupSucceeded;
        lock.unlock();

        if (!succeeded) {
            join();
        }
        return succeeded;
    }

    void requestStop() noexcept
    {
        stopRequested.store(true, std::memory_order_release);
        const HWND localWindow = hwnd.load(std::memory_order_acquire);
        if (localWindow) {
            PostMessageW(localWindow, WM_CLOSE, 0, 0);
        }
    }

    void join() noexcept
    {
        if (worker.joinable() &&
            worker.get_id() != std::this_thread::get_id()) {
            try {
                worker.join();
            } catch (...) {
            }
        }
    }

    void stopAndJoin() noexcept
    {
        requestStop();
        join();
    }

    std::string getLastError() const
    {
        std::lock_guard<std::mutex> lock(errorMutex);
        return errorText;
    }

    void rethrowWorkerExceptionIfAny() const
    {
        std::exception_ptr value;
        {
            std::lock_guard<std::mutex> lock(errorMutex);
            value = workerException;
        }
        if (value) std::rethrow_exception(value);
    }

    void setStartupResult(bool succeeded)
    {
        {
            std::lock_guard<std::mutex> lock(startupMutex);
            startupSucceeded = succeeded;
            startupComplete = true;
        }
        startupCv.notify_all();
    }

    void setWorkerError(std::exception_ptr error) noexcept
    {
        try {
            std::lock_guard<std::mutex> lock(errorMutex);
            workerException = error;
            try {
                if (error) std::rethrow_exception(error);
            } catch (const std::exception& exception) {
                errorText = exception.what();
            } catch (...) {
                errorText = "Unknown ImGui preview failure";
            }
        } catch (...) {
        }
    }

    static void AllocateImGuiDescriptor(
        ImGui_ImplDX12_InitInfo* info,
        D3D12_CPU_DESCRIPTOR_HANDLE* outCpu,
        D3D12_GPU_DESCRIPTOR_HANDLE* outGpu)
    {
        auto* self = static_cast<Impl*>(info ? info->UserData : nullptr);
        if (!self || !outCpu || !outGpu) return;
        const auto handle = self->descriptorAllocator.allocate();
        *outCpu = handle.cpu;
        *outGpu = handle.gpu;
    }

    static void FreeImGuiDescriptor(
        ImGui_ImplDX12_InitInfo* info,
        D3D12_CPU_DESCRIPTOR_HANDLE cpu,
        D3D12_GPU_DESCRIPTOR_HANDLE gpu)
    {
        auto* self = static_cast<Impl*>(info ? info->UserData : nullptr);
        if (!self) return;
        self->descriptorAllocator.free({cpu, gpu});
    }

    static LRESULT CALLBACK WindowProcedure(
        HWND window,
        UINT message,
        WPARAM wParam,
        LPARAM lParam)
    {
        Impl* self = reinterpret_cast<Impl*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));

        if (message == WM_NCCREATE) {
            const auto* create =
                reinterpret_cast<const CREATESTRUCTW*>(lParam);
            self = static_cast<Impl*>(create->lpCreateParams);
            SetWindowLongPtrW(
                window,
                GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(self));
        }

        if (ImGui::GetCurrentContext() &&
            ImGui_ImplWin32_WndProcHandler(
                window,
                message,
                wParam,
                lParam)) {
            return 1;
        }

        switch (message) {
        case WM_SIZE:
            if (self && wParam != SIZE_MINIMIZED) {
                self->pendingWidth.store(
                    static_cast<UINT>(LOWORD(lParam)),
                    std::memory_order_release);
                self->pendingHeight.store(
                    static_cast<UINT>(HIWORD(lParam)),
                    std::memory_order_release);
            }
            return 0;

        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0u) == SC_KEYMENU) {
                return 0;
            }
            break;

        case WM_CLOSE:
            if (self) {
                self->stopRequested.store(
                    true,
                    std::memory_order_release);
            }
            DestroyWindow(window);
            return 0;

        case WM_DESTROY:
            if (self) {
                self->hwnd.store(nullptr, std::memory_order_release);
            }
            PostQuitMessage(0);
            return 0;

        default:
            break;
        }

        return DefWindowProcW(window, message, wParam, lParam);
    }

    void threadMain() noexcept
    {
        bool startupWasReported = false;
        try {
            initialize();
            setStartupResult(true);
            startupWasReported = true;

            runMessageLoop();
            waitForGpuIdle();
            cleanup();
        } catch (...) {
            setWorkerError(std::current_exception());
            if (!startupWasReported) {
                setStartupResult(false);
                startupWasReported = true;
            }
            try {
                waitForGpuIdle();
            } catch (...) {
            }
            cleanup();
        }

        if (!startupWasReported) {
            setStartupResult(false);
        }
        running.store(false, std::memory_order_release);
    }

    void initialize()
    {
        device = core->GetDevice();
        if (!device) {
            throw std::runtime_error(
                "D3D12Core returned a null device");
        }

        ImGui_ImplWin32_EnableDpiAwareness();

        windowClass = {};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_CLASSDC;
        windowClass.lpfnWndProc = &Impl::WindowProcedure;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName =
            L"DualIC4VarjoImGuiStereoPreview";

        if (!RegisterClassExW(&windowClass)) {
            const DWORD error = GetLastError();
            if (error != ERROR_CLASS_ALREADY_EXISTS) {
                throw std::runtime_error(
                    "RegisterClassExW failed");
            }
        }
        windowClassRegistered = true;

        RECT windowRect{
            0,
            0,
            config.windowWidth,
            config.windowHeight};
        AdjustWindowRect(
            &windowRect,
            WS_OVERLAPPEDWINDOW,
            FALSE);

        const std::wstring title = Utf8ToWide(config.windowTitle);
        const HWND createdWindow = CreateWindowW(
            windowClass.lpszClassName,
            title.empty() ? L"Dual IC4 Stereo Preview" : title.c_str(),
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            windowRect.right - windowRect.left,
            windowRect.bottom - windowRect.top,
            nullptr,
            nullptr,
            windowClass.hInstance,
            this);
        if (!createdWindow) {
            throw std::runtime_error("CreateWindowW failed");
        }
        hwnd.store(createdWindow, std::memory_order_release);

        createDeviceObjects(createdWindow);
        initializeImGui(createdWindow);

        ShowWindow(createdWindow, SW_SHOWDEFAULT);
        UpdateWindow(createdWindow);

        std::cout << "[THREAD] ImGui preview thread id="
                  << GetCurrentThreadId()
                  << '\n';
    }

    void createDeviceObjects(HWND window)
    {
        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        ThrowIfFailed(
            device->CreateCommandQueue(
                &queueDesc,
                IID_PPV_ARGS(&commandQueue)),
            "CreateCommandQueue for ImGui preview");

        UINT factoryFlags = 0;
        ComPtr<IDXGIFactory6> factory;
        ThrowIfFailed(
            CreateDXGIFactory2(
                factoryFlags,
                IID_PPV_ARGS(&factory)),
            "CreateDXGIFactory2");

        BOOL allowTearing = FALSE;
        if (SUCCEEDED(factory->CheckFeatureSupport(
                DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                &allowTearing,
                sizeof(allowTearing)))) {
            tearingSupported = allowTearing == TRUE;
        }

        DXGI_SWAP_CHAIN_DESC1 swapDesc{};
        swapDesc.Width = 0;
        swapDesc.Height = 0;
        swapDesc.Format = kBackBufferFormat;
        swapDesc.SampleDesc.Count = 1;
        swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDesc.BufferCount = kBackBufferCount;
        swapDesc.Scaling = DXGI_SCALING_STRETCH;
        swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        swapDesc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        if (!config.vsync && tearingSupported) {
            swapDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        }
        swapChainFlags = swapDesc.Flags;

        ComPtr<IDXGISwapChain1> swapChain1;
        ThrowIfFailed(
            factory->CreateSwapChainForHwnd(
                commandQueue.Get(),
                window,
                &swapDesc,
                nullptr,
                nullptr,
                &swapChain1),
            "CreateSwapChainForHwnd");
        ThrowIfFailed(
            factory->MakeWindowAssociation(
                window,
                DXGI_MWA_NO_ALT_ENTER),
            "MakeWindowAssociation");
        ThrowIfFailed(
            swapChain1.As(&swapChain),
            "Query IDXGISwapChain3");

        ThrowIfFailed(
            swapChain->SetMaximumFrameLatency(kBackBufferCount),
            "SetMaximumFrameLatency");
        frameLatencyWaitableObject =
            swapChain->GetFrameLatencyWaitableObject();

        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.NumDescriptors = kBackBufferCount;
        ThrowIfFailed(
            device->CreateDescriptorHeap(
                &rtvHeapDesc,
                IID_PPV_ARGS(&rtvHeap)),
            "Create ImGui RTV descriptor heap");
        rtvIncrement = device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
        srvHeapDesc.Type =
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvHeapDesc.NumDescriptors = kSrvDescriptorCount;
        srvHeapDesc.Flags =
            D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(
            device->CreateDescriptorHeap(
                &srvHeapDesc,
                IID_PPV_ARGS(&srvHeap)),
            "Create ImGui SRV descriptor heap");
        descriptorAllocator.initialize(device, srvHeap.Get());

        for (auto& frame : frameContexts) {
            ThrowIfFailed(
                device->CreateCommandAllocator(
                    D3D12_COMMAND_LIST_TYPE_DIRECT,
                    IID_PPV_ARGS(&frame.commandAllocator)),
                "Create ImGui command allocator");
            frame.leftSrv = descriptorAllocator.allocate();
            frame.rightSrv = descriptorAllocator.allocate();
        }

        ThrowIfFailed(
            device->CreateCommandList(
                0,
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                frameContexts[0].commandAllocator.Get(),
                nullptr,
                IID_PPV_ARGS(&commandList)),
            "Create ImGui command list");
        ThrowIfFailed(
            commandList->Close(),
            "Close initial ImGui command list");

        ThrowIfFailed(
            device->CreateFence(
                0,
                D3D12_FENCE_FLAG_NONE,
                IID_PPV_ARGS(&fence)),
            "Create ImGui fence");
        fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!fenceEvent) {
            throw std::runtime_error(
                "CreateEventW failed for ImGui fence");
        }

        createRenderTargets();
    }

    void initializeImGui(HWND window)
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        imguiContextCreated = true;

        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

        ImGui::StyleColorsDark();
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowPadding = ImVec2(0.0f, 0.0f);
        style.ItemSpacing = ImVec2(0.0f, 0.0f);
        style.WindowRounding = 0.0f;

        if (!ImGui_ImplWin32_Init(window)) {
            throw std::runtime_error(
                "ImGui_ImplWin32_Init failed");
        }
        imguiWin32Initialized = true;

        ImGui_ImplDX12_InitInfo initInfo{};
        initInfo.Device = device;
        initInfo.CommandQueue = commandQueue.Get();
        initInfo.NumFramesInFlight =
            static_cast<int>(kBackBufferCount);
        initInfo.RTVFormat = kBackBufferFormat;
        initInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
        initInfo.UserData = this;
        initInfo.SrvDescriptorHeap = srvHeap.Get();
        initInfo.SrvDescriptorAllocFn =
            &Impl::AllocateImGuiDescriptor;
        initInfo.SrvDescriptorFreeFn =
            &Impl::FreeImGuiDescriptor;

        if (!ImGui_ImplDX12_Init(&initInfo)) {
            throw std::runtime_error(
                "ImGui_ImplDX12_Init failed");
        }
        imguiDx12Initialized = true;
    }

    void createRenderTargets()
    {
        auto rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT index = 0; index < kBackBufferCount; ++index) {
            ThrowIfFailed(
                swapChain->GetBuffer(
                    index,
                    IID_PPV_ARGS(&renderTargets[index])),
                "Get ImGui swap-chain buffer");
            renderTargetViews[index] = rtv;
            device->CreateRenderTargetView(
                renderTargets[index].Get(),
                nullptr,
                rtv);
            rtv.ptr += rtvIncrement;
        }
    }

    void releaseRenderTargets()
    {
        for (auto& target : renderTargets) {
            target.Reset();
        }
    }

    void waitForFenceValue(UINT64 value)
    {
        if (value == 0 || fence->GetCompletedValue() >= value) {
            return;
        }
        ThrowIfFailed(
            fence->SetEventOnCompletion(value, fenceEvent),
            "SetEventOnCompletion for ImGui preview");
        const DWORD waitResult =
            WaitForSingleObject(fenceEvent, INFINITE);
        if (waitResult != WAIT_OBJECT_0) {
            throw std::runtime_error(
                "WaitForSingleObject failed for ImGui preview");
        }
    }

    void waitForGpuIdle()
    {
        if (!commandQueue || !fence) return;
        const UINT64 value = ++lastFenceValue;
        ThrowIfFailed(
            commandQueue->Signal(fence.Get(), value),
            "Signal ImGui preview fence");
        waitForFenceValue(value);
        for (auto& frame : frameContexts) {
            frame.fenceValue = 0;
            frame.leftKeepAlive.Reset();
            frame.rightKeepAlive.Reset();
        }
    }

    void resizeSwapChain(UINT width, UINT height)
    {
        if (width == 0 || height == 0 || !swapChain) return;
        waitForGpuIdle();
        releaseRenderTargets();

        ThrowIfFailed(
            swapChain->ResizeBuffers(
                kBackBufferCount,
                width,
                height,
                kBackBufferFormat,
                swapChainFlags),
            "Resize ImGui swap chain");
        createRenderTargets();
    }

    void updateCurrentFrame()
    {
        auto latest = inputQueue->tryPopLatest();
        if (!latest) return;

        const auto* left = FindFrame(*latest, 0);
        const auto* right = FindFrame(*latest, 1);
        if (!left || !right || !left->texture || !right->texture) {
            throw std::runtime_error(
                "ImGui preview received an incomplete stereo frame");
        }

        if (left->ready.isValid() && !left->ready.wait()) {
            throw std::runtime_error(
                "Left preview frame did not become GPU-ready");
        }
        if (right->ready.isValid() && !right->ready.wait()) {
            throw std::runtime_error(
                "Right preview frame did not become GPU-ready");
        }

        const auto leftDesc = left->texture->GetDesc();
        const auto rightDesc = right->texture->GetDesc();
        if (leftDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            rightDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
            throw std::runtime_error(
                "ImGui preview requires Texture2D camera frames");
        }
        if ((left->resourceState &
             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) == 0 ||
            (right->resourceState &
             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) == 0) {
            throw std::runtime_error(
                "ImGui preview camera frames are not pixel-shader readable");
        }

        PreviewFrame next;
        next.left = left->texture;
        next.right = right->texture;
        next.leftFormat =
            left->dxgiFormat != DXGI_FORMAT_UNKNOWN
                ? left->dxgiFormat
                : leftDesc.Format;
        next.rightFormat =
            right->dxgiFormat != DXGI_FORMAT_UNKNOWN
                ? right->dxgiFormat
                : rightDesc.Format;
        next.leftWidth = static_cast<UINT>(leftDesc.Width);
        next.leftHeight = leftDesc.Height;
        next.rightWidth = static_cast<UINT>(rightDesc.Width);
        next.rightHeight = rightDesc.Height;
        next.syncGroupId = latest->syncGroupId;
        next.leftFrameNumber = left->timing.frameNumber;
        next.rightFrameNumber = right->timing.frameNumber;
        currentFrame = std::move(next);
    }

    void writeFrameDescriptors(DesktopFrameContext& frame)
    {
        frame.leftKeepAlive = currentFrame.left;
        frame.rightKeepAlive = currentFrame.right;

        D3D12_SHADER_RESOURCE_VIEW_DESC leftSrvDesc{};
        leftSrvDesc.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        leftSrvDesc.Format = currentFrame.leftFormat;
        leftSrvDesc.ViewDimension =
            D3D12_SRV_DIMENSION_TEXTURE2D;
        leftSrvDesc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(
            currentFrame.left.Get(),
            &leftSrvDesc,
            frame.leftSrv.cpu);

        D3D12_SHADER_RESOURCE_VIEW_DESC rightSrvDesc{};
        rightSrvDesc.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        rightSrvDesc.Format = currentFrame.rightFormat;
        rightSrvDesc.ViewDimension =
            D3D12_SRV_DIMENSION_TEXTURE2D;
        rightSrvDesc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(
            currentFrame.right.Get(),
            &rightSrvDesc,
            frame.rightSrv.cpu);
    }

    void buildUserInterface(const DesktopFrameContext& frame)
    {
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);

        constexpr ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBringToFrontOnFocus;

        ImGui::Begin("Stereo Preview", nullptr, flags);

        if (!currentFrame.valid()) {
            ImGui::SetCursorPos(ImVec2(16.0f, 16.0f));
            ImGui::TextUnformatted(
                "Waiting for synchronized camera frames...");
            ImGui::End();
            return;
        }

        constexpr float overlayHeight = 24.0f;
        const ImVec2 available = ImGui::GetContentRegionAvail();
        const float totalSourceWidth =
            static_cast<float>(
                currentFrame.leftWidth + currentFrame.rightWidth);
        const float maxSourceHeight =
            static_cast<float>(
                std::max(
                    currentFrame.leftHeight,
                    currentFrame.rightHeight));
        const float drawableHeight =
            std::max(1.0f, available.y - overlayHeight);
        const float scale = std::max(
            0.0001f,
            std::min(
                available.x / totalSourceWidth,
                drawableHeight / maxSourceHeight));

        const float leftWidth =
            static_cast<float>(currentFrame.leftWidth) * scale;
        const float leftHeight =
            static_cast<float>(currentFrame.leftHeight) * scale;
        const float rightWidth =
            static_cast<float>(currentFrame.rightWidth) * scale;
        const float rightHeight =
            static_cast<float>(currentFrame.rightHeight) * scale;
        const float combinedWidth = leftWidth + rightWidth;
        const float combinedHeight =
            std::max(leftHeight, rightHeight);

        ImGui::SetCursorPos(ImVec2(8.0f, 4.0f));
        ImGui::Text(
            "Sync %llu | L %llu | R %llu | %.1f FPS",
            static_cast<unsigned long long>(
                currentFrame.syncGroupId),
            static_cast<unsigned long long>(
                currentFrame.leftFrameNumber),
            static_cast<unsigned long long>(
                currentFrame.rightFrameNumber),
            ImGui::GetIO().Framerate);

        const float offsetX =
            std::max(0.0f, (available.x - combinedWidth) * 0.5f);
        const float offsetY =
            overlayHeight +
            std::max(
                0.0f,
                (drawableHeight - combinedHeight) * 0.5f);
        ImGui::SetCursorPos(ImVec2(offsetX, offsetY));

        const ImTextureRef leftTexture(
            static_cast<ImTextureID>(frame.leftSrv.gpu.ptr));
        const ImTextureRef rightTexture(
            static_cast<ImTextureID>(frame.rightSrv.gpu.ptr));

        ImGui::Image(
            leftTexture,
            ImVec2(leftWidth, leftHeight));
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::Image(
            rightTexture,
            ImVec2(rightWidth, rightHeight));

        ImGui::End();
    }

    void renderDesktopFrame()
    {
        if (frameLatencyWaitableObject) {
            const DWORD waitResult = WaitForSingleObject(
                frameLatencyWaitableObject,
                1000);
            if (waitResult != WAIT_OBJECT_0 &&
                waitResult != WAIT_TIMEOUT) {
                throw std::runtime_error(
                    "Swap-chain frame-latency wait failed");
            }
        }

        const UINT backBufferIndex =
            swapChain->GetCurrentBackBufferIndex();
        DesktopFrameContext& frame =
            frameContexts[backBufferIndex];

        waitForFenceValue(frame.fenceValue);
        frame.fenceValue = 0;
        frame.leftKeepAlive.Reset();
        frame.rightKeepAlive.Reset();

        updateCurrentFrame();
        if (currentFrame.valid()) {
            writeFrameDescriptors(frame);
        }

        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        buildUserInterface(frame);
        ImGui::Render();

        ThrowIfFailed(
            frame.commandAllocator->Reset(),
            "Reset ImGui command allocator");
        ThrowIfFailed(
            commandList->Reset(
                frame.commandAllocator.Get(),
                nullptr),
            "Reset ImGui command list");

        D3D12_RESOURCE_BARRIER toRenderTarget{};
        toRenderTarget.Type =
            D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toRenderTarget.Transition.pResource =
            renderTargets[backBufferIndex].Get();
        toRenderTarget.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        toRenderTarget.Transition.StateBefore =
            D3D12_RESOURCE_STATE_PRESENT;
        toRenderTarget.Transition.StateAfter =
            D3D12_RESOURCE_STATE_RENDER_TARGET;
        commandList->ResourceBarrier(1, &toRenderTarget);

        constexpr float clearColor[4]{
            0.0f,
            0.0f,
            0.0f,
            1.0f};
        commandList->ClearRenderTargetView(
            renderTargetViews[backBufferIndex],
            clearColor,
            0,
            nullptr);
        commandList->OMSetRenderTargets(
            1,
            &renderTargetViews[backBufferIndex],
            FALSE,
            nullptr);

        ID3D12DescriptorHeap* heaps[]{srvHeap.Get()};
        commandList->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(
            ImGui::GetDrawData(),
            commandList.Get());

        D3D12_RESOURCE_BARRIER toPresent =
            toRenderTarget;
        toPresent.Transition.StateBefore =
            D3D12_RESOURCE_STATE_RENDER_TARGET;
        toPresent.Transition.StateAfter =
            D3D12_RESOURCE_STATE_PRESENT;
        commandList->ResourceBarrier(1, &toPresent);

        ThrowIfFailed(
            commandList->Close(),
            "Close ImGui command list");
        ID3D12CommandList* lists[]{commandList.Get()};
        commandQueue->ExecuteCommandLists(1, lists);

        UINT presentFlags = 0;
        if (!config.vsync && tearingSupported) {
            presentFlags |= DXGI_PRESENT_ALLOW_TEARING;
        }
        const HRESULT presentResult = swapChain->Present(
            config.vsync ? 1 : 0,
            presentFlags);
        if (presentResult == DXGI_STATUS_OCCLUDED) {
            swapChainOccluded = true;
        } else {
            ThrowIfFailed(
                presentResult,
                "Present ImGui stereo preview");
            swapChainOccluded = false;
        }

        const UINT64 fenceValue = ++lastFenceValue;
        ThrowIfFailed(
            commandQueue->Signal(
                fence.Get(),
                fenceValue),
            "Signal ImGui frame fence");
        frame.fenceValue = fenceValue;
    }

    void runMessageLoop()
    {
        while (!stopRequested.load(std::memory_order_acquire)) {
            MSG message{};
            while (PeekMessageW(
                    &message,
                    nullptr,
                    0,
                    0,
                    PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
                if (message.message == WM_QUIT) {
                    stopRequested.store(
                        true,
                        std::memory_order_release);
                }
            }
            if (stopRequested.load(std::memory_order_acquire)) {
                break;
            }

            const UINT resizeWidth =
                pendingWidth.exchange(
                    0,
                    std::memory_order_acq_rel);
            const UINT resizeHeight =
                pendingHeight.exchange(
                    0,
                    std::memory_order_acq_rel);
            if (resizeWidth > 0 && resizeHeight > 0) {
                resizeSwapChain(resizeWidth, resizeHeight);
            }

            const HWND localWindow =
                hwnd.load(std::memory_order_acquire);
            if (!localWindow || IsIconic(localWindow)) {
                Sleep(10);
                continue;
            }

            if (swapChainOccluded) {
                const HRESULT testResult =
                    swapChain->Present(0, DXGI_PRESENT_TEST);
                if (testResult == DXGI_STATUS_OCCLUDED) {
                    Sleep(10);
                    continue;
                }
                ThrowIfFailed(
                    testResult,
                    "Test ImGui swap-chain visibility");
                swapChainOccluded = false;
            }

            renderDesktopFrame();
        }
    }

    void cleanup() noexcept
    {
        if (imguiDx12Initialized) {
            ImGui_ImplDX12_Shutdown();
            imguiDx12Initialized = false;
        }
        if (imguiWin32Initialized) {
            ImGui_ImplWin32_Shutdown();
            imguiWin32Initialized = false;
        }
        if (imguiContextCreated) {
            ImGui::DestroyContext();
            imguiContextCreated = false;
        }

        releaseRenderTargets();

        if (frameLatencyWaitableObject) {
            CloseHandle(frameLatencyWaitableObject);
            frameLatencyWaitableObject = nullptr;
        }
        if (fenceEvent) {
            CloseHandle(fenceEvent);
            fenceEvent = nullptr;
        }

        commandList.Reset();
        for (auto& frame : frameContexts) {
            frame.leftKeepAlive.Reset();
            frame.rightKeepAlive.Reset();
            frame.commandAllocator.Reset();
        }
        fence.Reset();
        swapChain.Reset();
        srvHeap.Reset();
        rtvHeap.Reset();
        commandQueue.Reset();

        const HWND localWindow =
            hwnd.exchange(nullptr, std::memory_order_acq_rel);
        if (localWindow && IsWindow(localWindow)) {
            DestroyWindow(localWindow);
        }

        if (windowClassRegistered) {
            UnregisterClassW(
                windowClass.lpszClassName,
                windowClass.hInstance);
            windowClassRegistered = false;
        }

        device = nullptr;
    }

    std::shared_ptr<D3D12CoreLib::D3D12Core> core;
    std::shared_ptr<IC4Ext::D3D12SyncedFrameQueue> inputQueue;
    ImGuiStereoPreviewConfig config;

    ID3D12Device* device = nullptr;
    ComPtr<ID3D12CommandQueue> commandQueue;
    ComPtr<IDXGISwapChain3> swapChain;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12DescriptorHeap> srvHeap;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ComPtr<ID3D12Fence> fence;
    std::array<ComPtr<ID3D12Resource>, kBackBufferCount>
        renderTargets;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kBackBufferCount>
        renderTargetViews{};
    std::array<DesktopFrameContext, kBackBufferCount>
        frameContexts;
    DescriptorAllocator descriptorAllocator;
    PreviewFrame currentFrame;

    HANDLE fenceEvent = nullptr;
    HANDLE frameLatencyWaitableObject = nullptr;
    UINT rtvIncrement = 0;
    UINT swapChainFlags = 0;
    UINT64 lastFenceValue = 0;
    bool tearingSupported = false;
    bool swapChainOccluded = false;

    WNDCLASSEXW windowClass{};
    bool windowClassRegistered = false;
    std::atomic<HWND> hwnd{nullptr};
    std::atomic<UINT> pendingWidth{0};
    std::atomic<UINT> pendingHeight{0};

    bool imguiContextCreated = false;
    bool imguiWin32Initialized = false;
    bool imguiDx12Initialized = false;

    std::atomic<bool> stopRequested{false};
    std::atomic<bool> running{false};
    std::thread worker;

    mutable std::mutex startupMutex;
    std::condition_variable startupCv;
    bool startupComplete = false;
    bool startupSucceeded = false;

    mutable std::mutex errorMutex;
    std::string errorText;
    std::exception_ptr workerException;
};

ImGuiStereoPreview::ImGuiStereoPreview(
    std::shared_ptr<D3D12CoreLib::D3D12Core> core,
    std::shared_ptr<IC4Ext::D3D12SyncedFrameQueue> inputQueue,
    ImGuiStereoPreviewConfig config)
    : impl_(std::make_unique<Impl>(
          std::move(core),
          std::move(inputQueue),
          std::move(config)))
{
}

ImGuiStereoPreview::~ImGuiStereoPreview() = default;

bool ImGuiStereoPreview::start()
{
    return impl_->start();
}

void ImGuiStereoPreview::requestStop() noexcept
{
    impl_->requestStop();
}

void ImGuiStereoPreview::join() noexcept
{
    impl_->join();
}

void ImGuiStereoPreview::stopAndJoin() noexcept
{
    impl_->stopAndJoin();
}

bool ImGuiStereoPreview::running() const noexcept
{
    return impl_->running.load(std::memory_order_acquire);
}

std::string ImGuiStereoPreview::lastError() const
{
    return impl_->getLastError();
}

void ImGuiStereoPreview::rethrowWorkerExceptionIfAny() const
{
    impl_->rethrowWorkerExceptionIfAny();
}

} // namespace DualIC4Varjo
