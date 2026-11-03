// Copyright (C) 2024 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "vk_emulated_physical_device_memory.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>

#include "gfxstream/common/logging.h"
#include "gfxstream/system/System.h"

namespace gfxstream {
namespace host {
namespace vk {
namespace {

static constexpr const uint32_t kInvalidMemoryTypeIndex = std::numeric_limits<uint32_t>::max();

// MAS port of google/gfxstream e2ddfc73 (#209): the same function as upstream.
// The host memory type to allocate from instead of the given one with the
// VulkanAllocateUncachedHostVisibleAsCached feature.
//
// Host visible memory that is neither cached nor device local is typically write-combined system
// memory, e.g. on NVIDIA. The guest writes into it through its mapping, which we measured to be
// much slower than cached memory under WHPX, so use a cached coherent type on the same heap
// instead, if there is one.
uint32_t FindReplacementCachedHostMemoryTypeIndex(
    const VkPhysicalDeviceMemoryProperties& hostMemoryProperties, uint32_t hostMemoryTypeIndex) {
    const VkMemoryType& memoryType = hostMemoryProperties.memoryTypes[hostMemoryTypeIndex];
    if (!(memoryType.propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ||
        (memoryType.propertyFlags &
         (VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))) {
        return hostMemoryTypeIndex;
    }

    constexpr VkMemoryPropertyFlags kCachedCoherent = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                                      VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    for (uint32_t i = 0; i < hostMemoryProperties.memoryTypeCount; i++) {
        const VkMemoryType& candidate = hostMemoryProperties.memoryTypes[i];
        if (candidate.heapIndex == memoryType.heapIndex &&
            (candidate.propertyFlags & kCachedCoherent) == kCachedCoherent &&
            !(candidate.propertyFlags &
              (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD |
               VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD))) {
            return i;
        }
    }
    return hostMemoryTypeIndex;
}

}  // namespace

EmulatedPhysicalDeviceMemoryProperties::EmulatedPhysicalDeviceMemoryProperties(
    const VkPhysicalDeviceMemoryProperties& hostMemoryProperties,
    const uint32_t hostColorBufferMemoryTypeIndex, const gfxstream::host::FeatureSet& features) {
    // Start with the original host memory properties:
    mHostMemoryProperties = hostMemoryProperties;
    mGuestMemoryProperties = hostMemoryProperties;
    std::fill_n(mGuestToHostMemoryTypeIndexMap, VK_MAX_MEMORY_TYPES, kInvalidMemoryTypeIndex);
    std::fill_n(mHostToGuestMemoryTypeIndexMap, VK_MAX_MEMORY_TYPES, kInvalidMemoryTypeIndex);
    for (uint32_t i = 0; i < mHostMemoryProperties.memoryTypeCount; i++) {
        mGuestToHostMemoryTypeIndexMap[i] = i;
        mHostToGuestMemoryTypeIndexMap[i] = i;
    }
    mGuestColorBufferMemoryTypeIndex = hostColorBufferMemoryTypeIndex;

    // Limit max safe memory heap size if the VulkanMaxSafeHeapSize feature is set to a non-zero
    // value.
    const uint64_t maxSafeHeapSizeLimit = features.VulkanMaxSafeHeapSize.getValue().value_or(0);
    if (maxSafeHeapSizeLimit > 0) {
        for (uint32_t i = 0; i < mHostMemoryProperties.memoryHeapCount; i++) {
            if (mGuestMemoryProperties.memoryHeaps[i].size > maxSafeHeapSizeLimit) {
                mGuestMemoryProperties.memoryHeaps[i].size = maxSafeHeapSizeLimit;
            }
        }
    }

    // Strip VK_AMD_device_coherent_memory flags that gfxstream does not translate.
    for (uint32_t i = 0; i < mGuestMemoryProperties.memoryTypeCount; i++) {
        mGuestMemoryProperties.memoryTypes[i].propertyFlags &=
            ~(VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD |
              VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD);
    }

    // MAS (Endfield, 10-15), off by default, ANDROID_EMU_VK_HIDE_REBAR=1 turns it on: offer the
    // guest the host's device-local + host-visible types (NVIDIA ReBAR / BAR VRAM) as device-local
    // only. A game that maps such memory writes its per-frame uploads straight into VRAM through
    // the address space mapping, i.e. write-combined over PCIe and then through WHPX; Endfield's
    // render thread spent most of its time in that copy loop. Without the host-visible bit the
    // guest picks a plain host-visible type for those buffers, which ANDROID_EMU_VK_WC_AS_CACHED
    // backs with cached system memory. The type index and heap stay the same, so images and
    // buffers that need device-local memory still get the same host type (memoryTypeBits are
    // unchanged); only CPU mapping of VRAM goes away. GPU reads of the moved buffers cross PCIe
    // instead of hitting VRAM.
    if (gfxstream::base::getEnvironmentVariable("ANDROID_EMU_VK_HIDE_REBAR") == "1") {
        constexpr VkMemoryPropertyFlags kRebar =
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        constexpr VkMemoryPropertyFlags kHostAccess = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                                      VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        for (uint32_t i = 0; i < mGuestMemoryProperties.memoryTypeCount; i++) {
            const VkMemoryPropertyFlags hostFlags =
                mHostMemoryProperties.memoryTypes[i].propertyFlags;
            if ((hostFlags & kRebar) == kRebar) {
                mGuestMemoryProperties.memoryTypes[i].propertyFlags &= ~kHostAccess;
                GFXSTREAM_INFO(
                    "MAS: memory type %u (host flags 0x%x, heap %u) offered to the guest as "
                    "device-local only (ANDROID_EMU_VK_HIDE_REBAR)",
                    i, hostFlags, mHostMemoryProperties.memoryTypes[i].heapIndex);
            }
        }
    }

    // If enabled, hide non device memory types from the guest.
    // (useful to work around a bug where KVM can't map TTM memory).
    if (features.VulkanAllocateDeviceMemoryOnly.enabled()) {
        for (uint32_t i = 0; i < mGuestMemoryProperties.memoryTypeCount; i++) {
            auto guestMemoryProperties = mGuestMemoryProperties.memoryTypes[i].propertyFlags;
            if (!(guestMemoryProperties & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                mGuestMemoryProperties.memoryTypes[i].propertyFlags = 0;
            }
        }
    }

    // Coherent memory in the guest requires one of these features:
    if (!features.GlDirectMem.enabled() && !features.VirtioGpuNext.enabled()) {
        for (uint32_t i = 0; i < mGuestMemoryProperties.memoryTypeCount; i++) {
            mGuestMemoryProperties.memoryTypes[i].propertyFlags =
                mGuestMemoryProperties.memoryTypes[i].propertyFlags &
                ~(VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        }
    }

    // Let cached memory pretend as coherent on the guest side.
    if (features.VulkanDisableCoherentMemoryAndEmulate.enabled()) {
        for (uint32_t i = 0; i < mGuestMemoryProperties.memoryTypeCount; i++) {
            if (mGuestMemoryProperties.memoryTypes[i].propertyFlags &
                VK_MEMORY_PROPERTY_HOST_CACHED_BIT) {
                mGuestMemoryProperties.memoryTypes[i].propertyFlags |=
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            } else {
                mGuestMemoryProperties.memoryTypes[i].propertyFlags &=
                    ~(VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            }
        }
    }

    if (features.VulkanEnsureCachedCoherentMemoryAvailable.enabled()) {
        /* Some app layers (i.e. Angle) require *some* coherent-cached memory to be
         *  available. To ensure compatiblity these guest layers, when coherent-cached
         *  memory type is unavailable, append the cached bit to the first coherent
         *  memory type available. Note that in this scenario, there is no potential
         *  functional downside to marking one of the host-coherent as cached, aside
         *  from the guest layer believing there will be some performance benefit to
         *  using this particular memory.
         */
        bool hasCoherentCached = false;
        uint32_t firstCoherent = VK_MAX_MEMORY_TYPES;
        for (uint32_t i = 0; i < mGuestMemoryProperties.memoryTypeCount; i++) {
            const VkMemoryPropertyFlags flags = mGuestMemoryProperties.memoryTypes[i].propertyFlags;
            const bool coherent = flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            const bool cached = flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
            if (coherent) {
                if (firstCoherent == VK_MAX_MEMORY_TYPES) {
                    firstCoherent = i;
                }
                if (cached) {
                    hasCoherentCached = true;
                }
            }
        }

        if (!hasCoherentCached) {
            if (firstCoherent == VK_MAX_MEMORY_TYPES) {
                GFXSTREAM_FATAL(
                    "Unexpected memoryTypes error -- no available host-coherent memory.");
            }
            mGuestMemoryProperties.memoryTypes[firstCoherent].propertyFlags |=
                VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        }
    }

    // MAS port of google/gfxstream e2ddfc73 (#209). Upstream sets the host memory type index of
    // each guest memory type here; this tree keeps mGuestToHostMemoryTypeIndexMap instead.
    // Only replace the memory type when gfxstream allocates host visible memory itself and maps
    // it with vkMapMemory(). With these features, host visible memory is exported or is host
    // memory that gfxstream imports, and it keeps its memory type.
    const bool hostVisibleMemoryIsExportedOrImported =
        features.ExternalBlob.enabled() || features.SystemBlob.enabled() ||
        features.VulkanAllocateHostVisibleAsUdmabuf.enabled() ||
        features.VulkanAllocateHostMemory.enabled();
    bool allocateUncachedHostVisibleAsCached = false;
    if (features.VulkanAllocateUncachedHostVisibleAsCached.enabled()) {
        if (hostVisibleMemoryIsExportedOrImported) {
            // This is built again for every vkEnumeratePhysicalDevices(), so warn only once.
            static std::once_flag sWarnOnce;
            std::call_once(sWarnOnce, [] {
                GFXSTREAM_WARNING(
                    "VulkanAllocateUncachedHostVisibleAsCached has no effect with ExternalBlob, "
                    "SystemBlob, VulkanAllocateHostVisibleAsUdmabuf or VulkanAllocateHostMemory.");
            });
        } else {
            allocateUncachedHostVisibleAsCached = true;
        }
    }
    if (allocateUncachedHostVisibleAsCached) {
        for (uint32_t i = 0; i < mGuestMemoryProperties.memoryTypeCount; i++) {
            // A replaced guest memory type keeps the flags of the original host memory type, and
            // transformToGuestMemoryRequirements() only offers it for resources that allow the
            // replacement.
            mGuestToHostMemoryTypeIndexMap[i] =
                FindReplacementCachedHostMemoryTypeIndex(mHostMemoryProperties, i);
        }
    }

    // If enabled, reserve an additional memory type for AHB backed buffers and images
    // so that the host can control its memory properties. This ensures that the guest
    // only sees `VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT` and will not try to map the
    // memory.
    if (features.VulkanUseDedicatedAhbMemoryType.enabled()) {
        if (mGuestMemoryProperties.memoryTypeCount == VK_MAX_MEMORY_TYPES) {
            GFXSTREAM_FATAL(
                "Unable to create emulated AHB memory type because VK_MAX_MEMORY_TYPES "
                "already in use.");
        }

        uint32_t ahbMemoryTypeIndex = mGuestMemoryProperties.memoryTypeCount;
        ++mGuestMemoryProperties.memoryTypeCount;

        VkMemoryType& ahbMemoryType = mGuestMemoryProperties.memoryTypes[ahbMemoryTypeIndex];
        ahbMemoryType.propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        ahbMemoryType.heapIndex =
            mHostMemoryProperties.memoryTypes[hostColorBufferMemoryTypeIndex].heapIndex;

        mGuestToHostMemoryTypeIndexMap[ahbMemoryTypeIndex] = hostColorBufferMemoryTypeIndex;
        mGuestAhbMemoryTypeIndex = ahbMemoryTypeIndex;

        mGuestColorBufferMemoryTypeIndex = ahbMemoryTypeIndex;
    }
}

std::optional<EmulatedPhysicalDeviceMemoryProperties::HostMemoryInfo>
EmulatedPhysicalDeviceMemoryProperties::getHostMemoryInfoFromHostMemoryTypeIndex(
    uint32_t hostMemoryTypeIndex) const {
    if (hostMemoryTypeIndex >= mHostMemoryProperties.memoryTypeCount) {
        return std::nullopt;
    }

    return HostMemoryInfo{
        .index = hostMemoryTypeIndex,
        .memoryType = mHostMemoryProperties.memoryTypes[hostMemoryTypeIndex],
    };
}

std::optional<EmulatedPhysicalDeviceMemoryProperties::HostMemoryInfo>
EmulatedPhysicalDeviceMemoryProperties::getHostMemoryInfoFromGuestMemoryTypeIndex(
    uint32_t guestMemoryTypeIndex) const {
    if (guestMemoryTypeIndex >= mGuestMemoryProperties.memoryTypeCount) {
        return std::nullopt;
    }

    uint32_t hostMemoryTypeIndex = mGuestToHostMemoryTypeIndexMap[guestMemoryTypeIndex];
    if (hostMemoryTypeIndex == kInvalidMemoryTypeIndex) {
        return std::nullopt;
    }

    return getHostMemoryInfoFromHostMemoryTypeIndex(hostMemoryTypeIndex);
}

void EmulatedPhysicalDeviceMemoryProperties::transformToGuestMemoryRequirements(
    VkMemoryRequirements* memoryRequirements) const {
    uint32_t guestMemoryTypeBits = 0;

    // MAS port of google/gfxstream e2ddfc73 (#209): go through the guest memory types as upstream,
    // since VulkanAllocateUncachedHostVisibleAsCached backs two guest memory types with one host
    // memory type, which mHostToGuestMemoryTypeIndexMap cannot express. The AHB memory type is
    // skipped, as it was never in mHostToGuestMemoryTypeIndexMap.
    const uint32_t hostMemoryTypeBits = memoryRequirements->memoryTypeBits;
    for (uint32_t guestMemoryTypeIndex = 0;
         guestMemoryTypeIndex < mGuestMemoryProperties.memoryTypeCount; guestMemoryTypeIndex++) {
        const uint32_t hostMemoryTypeIndex = mGuestToHostMemoryTypeIndexMap[guestMemoryTypeIndex];
        if (hostMemoryTypeIndex == kInvalidMemoryTypeIndex) {
            continue;
        }
        if (mGuestAhbMemoryTypeIndex && guestMemoryTypeIndex == *mGuestAhbMemoryTypeIndex) {
            continue;
        }

        if (!(hostMemoryTypeBits & (1u << hostMemoryTypeIndex))) {
            // MAS local diagnostic: the resource allows the original host memory type of a
            // replaced guest memory type, but not the replacement, so the guest loses that type
            // for it. Never seen on NVIDIA (11-01); say so if a driver does it.
            if (hostMemoryTypeIndex != guestMemoryTypeIndex &&
                guestMemoryTypeIndex < mHostMemoryProperties.memoryTypeCount &&
                (hostMemoryTypeBits & (1u << guestMemoryTypeIndex))) {
                static std::atomic<uint32_t> sHiddenCount{0};
                const uint32_t hiddenCount = ++sHiddenCount;
                if (hiddenCount <= 20 || (hiddenCount & (hiddenCount - 1)) == 0) {
                    GFXSTREAM_INFO(
                        "MAS: guest memory type %u hidden from a resource that allows host "
                        "memory type %u but not %u (host bits 0x%x, %u so far)",
                        guestMemoryTypeIndex, guestMemoryTypeIndex, hostMemoryTypeIndex,
                        hostMemoryTypeBits, hiddenCount);
                }
            }
            continue;
        }

        guestMemoryTypeBits |= (1u << guestMemoryTypeIndex);
    }

    memoryRequirements->memoryTypeBits = guestMemoryTypeBits;
}

void EmulatedPhysicalDeviceMemoryProperties::clampMemoryBudgetToGuestHeapSizes(
    VkPhysicalDeviceMemoryBudgetPropertiesEXT* budgetProps) const {
    if (budgetProps == nullptr) {
        return;
    }
    for (uint32_t i = 0; i < mGuestMemoryProperties.memoryHeapCount; i++) {
        const VkDeviceSize heapSize = mGuestMemoryProperties.memoryHeaps[i].size;
        if (budgetProps->heapBudget[i] > heapSize) {
            budgetProps->heapBudget[i] = heapSize;
        }
        if (budgetProps->heapUsage[i] > heapSize) {
            budgetProps->heapUsage[i] = heapSize;
        }
    }
}

}  // namespace vk
}  // namespace host
}  // namespace gfxstream