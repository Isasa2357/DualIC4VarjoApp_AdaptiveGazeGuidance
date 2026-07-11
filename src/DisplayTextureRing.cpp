#include "DisplayTextureRing.hpp"

#include <D3D12Helper/D3D12Core/D3D12Barrier.hpp>
#include <D3D12Helper/D3D12Framework/D3D12Helpers.hpp>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace DualIC4Varjo {
namespace {

constexpr D3D12_RESOURCE_STATES kDisplayState =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

void AddTransition(
    std::vector<D3D12_RESOURCE_BARRIER>& barriers,
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after)
{
    if (before != after) {
        barriers.push_back(
            D3D12CoreLib::MakeTransitionBarrier(resource, before, after));
    }
}

} // namespace

DisplayTextureRing::DisplayTextureRing(
    std::shared_ptr<D3D12CoreLib::D3D12Core> core,
    VarjoXR::Backends::D3D12::D3D12Backend& backend,
    std::size_t slotCount)
    : core_(std::move(core))
    , backend_(backend)
    , requestedSlotCount_(std::max<std::size_t>(3, slotCount))
{
    if (!core_) {
        throw std::invalid_argument("DisplayTextureRing requires D3D12Core");
    }
}

DisplayTextureRing::~DisplayTextureRing()
{
    try {
        waitIdle();
    } catch (...) {
    }
}

bool DisplayTextureRing::FormatKey::operator==(
    const FormatKey& other) const noexcept
{
    return width == other.width &&
           height == other.height &&
           format == other.format;
}

DisplayTextureRing::FormatKey DisplayTextureRing::makeFormatKey(
    const IC4Ext::D3D12CameraFrame& frame)
{
    if (!frame.texture) {
        throw std::runtime_error("Display frame contains a null D3D12 texture");
    }
    const auto desc = frame.texture->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        throw std::runtime_error("Camera output must be Texture2D");
    }

    FormatKey key;
    key.width = static_cast<UINT>(desc.Width);
    key.height = desc.Height;
    key.format = frame.dxgiFormat != DXGI_FORMAT_UNKNOWN
        ? frame.dxgiFormat
        : desc.Format;
    return key;
}

void DisplayTextureRing::rebuild(const FormatKey& format)
{
    waitIdle();
    slots_.clear();
    slots_.resize(requestedSlotCount_);

    for (auto& slot : slots_) {
        slot.commandContext = core_->CreateDirectContext();
        slot.resource = D3D12CoreLib::CreateTexture2D(
            *core_,
            format.width,
            format.height,
            format.format,
            kDisplayState);
        slot.texture = backend_.wrapResource(
            slot.resource.Get(),
            format.format);
    }

    format_ = format;
    initialized_ = true;
    nextSlot_ = 0;
}

void DisplayTextureRing::waitSlot(Slot& slot)
{
    if (slot.lastRenderFence != 0) {
        core_->DirectQueue().WaitForFenceValue(slot.lastRenderFence);
        slot.lastRenderFence = 0;
    }
    slot.sourceKeepAlive.Reset();
}

DisplayTextureRing::UploadResult DisplayTextureRing::upload(
    const IC4Ext::D3D12CameraFrame& frame)
{
    if (frame.ready.isValid() && !frame.ready.wait()) {
        throw std::runtime_error(
            "Timed out waiting for the camera GPU frame");
    }

    const FormatKey key = makeFormatKey(frame);
    if (!initialized_ || key != format_) {
        rebuild(key);
    }

    const std::size_t slotIndex = nextSlot_;
    nextSlot_ = (nextSlot_ + 1) % slots_.size();
    Slot& slot = slots_[slotIndex];
    waitSlot(slot);
    slot.sourceKeepAlive = frame.texture;

    auto& context = slot.commandContext;
    context.Reset();
    auto* commandList = context.GetCommandList();

    std::vector<D3D12_RESOURCE_BARRIER> beforeCopy;
    beforeCopy.reserve(2);
    AddTransition(
        beforeCopy,
        frame.texture.Get(),
        frame.resourceState,
        D3D12_RESOURCE_STATE_COPY_SOURCE);
    AddTransition(
        beforeCopy,
        slot.resource.Get(),
        slot.resource.GetState(),
        D3D12_RESOURCE_STATE_COPY_DEST);
    context.ResourceBarrier(
        static_cast<UINT>(beforeCopy.size()),
        beforeCopy.data());

    commandList->CopyResource(slot.resource.Get(), frame.texture.Get());

    std::vector<D3D12_RESOURCE_BARRIER> afterCopy;
    afterCopy.reserve(2);
    AddTransition(
        afterCopy,
        frame.texture.Get(),
        D3D12_RESOURCE_STATE_COPY_SOURCE,
        frame.resourceState);
    AddTransition(
        afterCopy,
        slot.resource.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        kDisplayState);
    context.ResourceBarrier(
        static_cast<UINT>(afterCopy.size()),
        afterCopy.data());

    context.Close();
    ID3D12CommandList* lists[] = {context.GetCommandList()};
    core_->DirectQueue().ExecuteCommandLists(1, lists);
    slot.lastRenderFence = core_->DirectQueue().Signal();
    slot.resource.SetState(kDisplayState);

    return UploadResult{slot.texture, slotIndex};
}

void DisplayTextureRing::markRendered(std::size_t slotIndex)
{
    if (slotIndex >= slots_.size()) {
        throw std::out_of_range("Display texture slot index is invalid");
    }
    slots_[slotIndex].lastRenderFence = core_->DirectQueue().Signal();
}

void DisplayTextureRing::waitIdle()
{
    if (!core_) return;
    core_->DirectQueue().WaitIdle();
    for (auto& slot : slots_) {
        slot.lastRenderFence = 0;
        slot.sourceKeepAlive.Reset();
    }
}

} // namespace DualIC4Varjo
