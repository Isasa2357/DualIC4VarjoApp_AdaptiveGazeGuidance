#include "StereoDisplayTextureRing.hpp"

#include <D3D12Helper/D3D12Core/D3D12Barrier.hpp>
#include <D3D12Helper/D3D12Framework/D3D12Helpers.hpp>

#include <algorithm>
#include <array>
#include <stdexcept>

namespace DualIC4Varjo {

namespace {

constexpr D3D12_RESOURCE_STATES kDisplayState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

void AddTransition(
    std::vector<D3D12_RESOURCE_BARRIER>& barriers,
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after)
{
    if (before != after) {
        barriers.push_back(D3D12CoreLib::MakeTransitionBarrier(resource, before, after));
    }
}

} // namespace

StereoDisplayTextureRing::StereoDisplayTextureRing(
    std::shared_ptr<D3D12CoreLib::D3D12Core> core,
    VarjoXR::Backends::D3D12::D3D12Backend& backend,
    std::size_t slotCount)
    : core_(std::move(core))
    , backend_(backend)
    , requestedSlotCount_(std::max<std::size_t>(3, slotCount))
{
    if (!core_) {
        throw std::invalid_argument("StereoDisplayTextureRing requires D3D12Core");
    }
}

StereoDisplayTextureRing::~StereoDisplayTextureRing()
{
    try {
        waitIdle();
    } catch (...) {
    }
}

bool StereoDisplayTextureRing::FormatKey::operator==(const FormatKey& other) const noexcept
{
    return leftWidth == other.leftWidth && leftHeight == other.leftHeight && leftFormat == other.leftFormat &&
           rightWidth == other.rightWidth && rightHeight == other.rightHeight && rightFormat == other.rightFormat;
}

StereoDisplayTextureRing::FormatKey StereoDisplayTextureRing::makeFormatKey(
    const IC4Ext::D3D12CameraFrame& left,
    const IC4Ext::D3D12CameraFrame& right)
{
    if (!left.texture || !right.texture) {
        throw std::runtime_error("Synchronized frame set contains a null D3D12 texture");
    }
    const auto leftDesc = left.texture->GetDesc();
    const auto rightDesc = right.texture->GetDesc();
    if (leftDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        rightDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        throw std::runtime_error("Camera output must be Texture2D");
    }

    FormatKey key;
    key.leftWidth = static_cast<UINT>(leftDesc.Width);
    key.leftHeight = leftDesc.Height;
    key.leftFormat = left.dxgiFormat != DXGI_FORMAT_UNKNOWN ? left.dxgiFormat : leftDesc.Format;
    key.rightWidth = static_cast<UINT>(rightDesc.Width);
    key.rightHeight = rightDesc.Height;
    key.rightFormat = right.dxgiFormat != DXGI_FORMAT_UNKNOWN ? right.dxgiFormat : rightDesc.Format;
    return key;
}

void StereoDisplayTextureRing::rebuild(const FormatKey& format)
{
    waitIdle();
    slots_.clear();
    slots_.resize(requestedSlotCount_);

    for (auto& slot : slots_) {
        slot.commandContext = core_->CreateDirectContext();
        slot.leftResource = D3D12CoreLib::CreateTexture2D(
            *core_, format.leftWidth, format.leftHeight, format.leftFormat, kDisplayState);
        slot.rightResource = D3D12CoreLib::CreateTexture2D(
            *core_, format.rightWidth, format.rightHeight, format.rightFormat, kDisplayState);

        slot.leftTexture = backend_.wrapResource(slot.leftResource.Get(), format.leftFormat);
        slot.rightTexture = backend_.wrapResource(slot.rightResource.Get(), format.rightFormat);
    }

    format_ = format;
    initialized_ = true;
    nextSlot_ = 0;
}

void StereoDisplayTextureRing::waitSlot(Slot& slot)
{
    if (slot.lastRenderFence != 0) {
        core_->DirectQueue().WaitForFenceValue(slot.lastRenderFence);
        slot.lastRenderFence = 0;
    }
    slot.leftSourceKeepAlive.Reset();
    slot.rightSourceKeepAlive.Reset();
}

StereoDisplayTextureRing::UploadResult StereoDisplayTextureRing::upload(
    const IC4Ext::D3D12CameraFrame& left,
    const IC4Ext::D3D12CameraFrame& right)
{
    if (left.ready.isValid() && !left.ready.wait()) {
        throw std::runtime_error("Timed out waiting for the left camera GPU frame");
    }
    if (right.ready.isValid() && !right.ready.wait()) {
        throw std::runtime_error("Timed out waiting for the right camera GPU frame");
    }

    const FormatKey key = makeFormatKey(left, right);
    if (!initialized_ || key != format_) {
        rebuild(key);
    }

    const std::size_t slotIndex = nextSlot_;
    nextSlot_ = (nextSlot_ + 1) % slots_.size();
    Slot& slot = slots_[slotIndex];
    waitSlot(slot);

    slot.leftSourceKeepAlive = left.texture;
    slot.rightSourceKeepAlive = right.texture;

    auto& context = slot.commandContext;
    context.Reset();
    auto* commandList = context.GetCommandList();

    std::vector<D3D12_RESOURCE_BARRIER> beforeCopy;
    beforeCopy.reserve(4);
    AddTransition(beforeCopy, left.texture.Get(), left.resourceState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    AddTransition(beforeCopy, right.texture.Get(), right.resourceState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    AddTransition(beforeCopy, slot.leftResource.Get(), slot.leftResource.GetState(), D3D12_RESOURCE_STATE_COPY_DEST);
    AddTransition(beforeCopy, slot.rightResource.Get(), slot.rightResource.GetState(), D3D12_RESOURCE_STATE_COPY_DEST);
    context.ResourceBarrier(static_cast<UINT>(beforeCopy.size()), beforeCopy.data());

    commandList->CopyResource(slot.leftResource.Get(), left.texture.Get());
    commandList->CopyResource(slot.rightResource.Get(), right.texture.Get());

    std::vector<D3D12_RESOURCE_BARRIER> afterCopy;
    afterCopy.reserve(4);
    AddTransition(afterCopy, left.texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, left.resourceState);
    AddTransition(afterCopy, right.texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, right.resourceState);
    AddTransition(afterCopy, slot.leftResource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kDisplayState);
    AddTransition(afterCopy, slot.rightResource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kDisplayState);
    context.ResourceBarrier(static_cast<UINT>(afterCopy.size()), afterCopy.data());

    context.Close();
    ID3D12CommandList* lists[] = {context.GetCommandList()};
    core_->DirectQueue().ExecuteCommandLists(1, lists);
    // Keep the camera resources alive even if rendering fails before markRendered().
    slot.lastRenderFence = core_->DirectQueue().Signal();
    slot.leftResource.SetState(kDisplayState);
    slot.rightResource.SetState(kDisplayState);

    return UploadResult{slot.leftTexture, slot.rightTexture, slotIndex};
}

void StereoDisplayTextureRing::markRendered(std::size_t slotIndex)
{
    if (slotIndex >= slots_.size()) {
        throw std::out_of_range("Display texture slot index is invalid");
    }
    slots_[slotIndex].lastRenderFence = core_->DirectQueue().Signal();
}

void StereoDisplayTextureRing::waitIdle()
{
    if (!core_) return;
    // Wait before releasing source keep-alive references. This also covers an
    // exception between copy submission and markRendered().
    core_->DirectQueue().WaitIdle();
    for (auto& slot : slots_) {
        slot.lastRenderFence = 0;
        slot.leftSourceKeepAlive.Reset();
        slot.rightSourceKeepAlive.Reset();
    }
}

} // namespace DualIC4Varjo
