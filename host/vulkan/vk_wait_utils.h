// Copyright 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <vulkan/vulkan.h>

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace gfxstream {
namespace host {
namespace vk {

// How often a long fence or semaphore wait checks whether its guest process is still alive.
inline constexpr uint64_t kProcessDeathCheckIntervalNs = 1000ULL * 1000ULL * 1000ULL;  // 1 s

// The part of |timeoutNs| left after |elapsedNs|. UINT64_MAX means no deadline and stays so.
inline uint64_t RemainingTimeoutNs(uint64_t timeoutNs, uint64_t elapsedNs) {
    if (timeoutNs == UINT64_MAX) {
        return UINT64_MAX;
    }
    return timeoutNs > elapsedNs ? timeoutNs - elapsedNs : 0;
}

// MAS (ModAVD#40), the loop of upstream gfxstream c1cecd29 without its process resource
// refactoring: waits with |waitFn(stepTimeoutNs)| in steps of at most
// kProcessDeathCheckIntervalNs and returns VK_TIMEOUT once |isProcessDead()| is true, so a
// render thread of a killed guest process does not stay blocked in the driver forever.
// UINT64_MAX waits without a deadline; any other timeout is charged with the time that actually
// passed. Tests replace |Clock|.
template <typename Clock = std::chrono::steady_clock, typename IsProcessDeadFn, typename WaitFn>
VkResult WaitWithProcessDeathCheck(uint64_t timeoutNs, IsProcessDeadFn&& isProcessDead,
                                   WaitFn&& waitFn) {
    uint64_t remainingTimeoutNs = timeoutNs;
    auto lastTime = Clock::now();
    while (true) {
        if (isProcessDead()) {
            return VK_TIMEOUT;
        }
        const uint64_t stepTimeoutNs = std::min(remainingTimeoutNs, kProcessDeathCheckIntervalNs);
        const VkResult result = waitFn(stepTimeoutNs);
        if (result != VK_TIMEOUT) {
            return result;
        }
        if (stepTimeoutNs == remainingTimeoutNs) {
            return VK_TIMEOUT;
        }
        if (timeoutNs != UINT64_MAX) {
            const auto now = Clock::now();
            const uint64_t elapsedNs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(now - lastTime).count());
            lastTime = now;
            remainingTimeoutNs = RemainingTimeoutNs(remainingTimeoutNs, elapsedNs);
            if (remainingTimeoutNs == 0) {
                return VK_TIMEOUT;
            }
        }
    }
}

}  // namespace vk
}  // namespace host
}  // namespace gfxstream
