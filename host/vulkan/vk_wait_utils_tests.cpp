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

#include "vk_wait_utils.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace gfxstream {
namespace host {
namespace vk {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;

constexpr uint64_t kMsNs = 1000ULL * 1000ULL;
constexpr uint64_t kSecondNs = 1000ULL * kMsNs;

// A clock that only moves when a fake wait says so.
struct FakeClock {
    using duration = std::chrono::nanoseconds;
    using rep = duration::rep;
    using period = duration::period;
    using time_point = std::chrono::time_point<FakeClock>;
    static constexpr bool is_steady = true;

    static time_point now() { return time_point(duration(sNowNs)); }

    static inline int64_t sNowNs = 0;
};

// Records the step timeouts it is called with. Each call advances FakeClock by
// |advanceNs(stepTimeoutNs)| and returns |result(callIndex)|. Gives up with
// VK_ERROR_DEVICE_LOST after kMaxCalls so a broken loop fails instead of hanging.
class FakeWait {
   public:
    static constexpr size_t kMaxCalls = 100;

    template <typename AdvanceFn, typename ResultFn>
    FakeWait(AdvanceFn advanceNs, ResultFn result) : mAdvanceNs(advanceNs), mResult(result) {}

    VkResult operator()(uint64_t stepTimeoutNs) {
        mSteps.push_back(stepTimeoutNs);
        if (mSteps.size() > kMaxCalls) {
            return VK_ERROR_DEVICE_LOST;
        }
        FakeClock::sNowNs += static_cast<int64_t>(mAdvanceNs(stepTimeoutNs));
        return mResult(mSteps.size() - 1);
    }

    const std::vector<uint64_t>& steps() const { return mSteps; }

   private:
    std::function<uint64_t(uint64_t)> mAdvanceNs;
    std::function<VkResult(size_t)> mResult;
    std::vector<uint64_t> mSteps;
};

// A driver that waits out the whole step and never signals.
FakeWait NeverSignaled() {
    return FakeWait([](uint64_t stepTimeoutNs) { return stepTimeoutNs; },
                    [](size_t) { return VK_TIMEOUT; });
}

template <typename IsProcessDeadFn>
VkResult Wait(uint64_t timeoutNs, IsProcessDeadFn isProcessDead, FakeWait& wait) {
    return WaitWithProcessDeathCheck<FakeClock>(
        timeoutNs, isProcessDead, [&wait](uint64_t stepTimeoutNs) { return wait(stepTimeoutNs); });
}

auto Alive() {
    return [] { return false; };
}

class VkWaitUtilsTest : public ::testing::Test {
   protected:
    void SetUp() override { FakeClock::sNowNs = 0; }
};

TEST_F(VkWaitUtilsTest, ReturnsTheDriverResultOnceSignaled) {
    FakeWait wait([](uint64_t) { return 5 * kMsNs; },
                  [](size_t) { return VK_SUCCESS; });
    EXPECT_EQ(VK_SUCCESS, Wait(UINT64_MAX, Alive(), wait));
    EXPECT_THAT(wait.steps(), ElementsAre(kSecondNs));
}

TEST_F(VkWaitUtilsTest, PassesErrorsThrough) {
    FakeWait wait([](uint64_t) { return 0; },
                  [](size_t) { return VK_ERROR_DEVICE_LOST; });
    EXPECT_EQ(VK_ERROR_DEVICE_LOST, Wait(3 * kSecondNs, Alive(), wait));
    EXPECT_THAT(wait.steps(), ElementsAre(kSecondNs));
}

TEST_F(VkWaitUtilsTest, ZeroTimeoutPollsOnce) {
    FakeWait wait = NeverSignaled();
    EXPECT_EQ(VK_TIMEOUT, Wait(0, Alive(), wait));
    EXPECT_THAT(wait.steps(), ElementsAre(0u));
}

TEST_F(VkWaitUtilsTest, TimeoutUnderOneStepWaitsOnce) {
    FakeWait wait = NeverSignaled();
    EXPECT_EQ(VK_TIMEOUT, Wait(300 * kMsNs, Alive(), wait));
    EXPECT_THAT(wait.steps(), ElementsAre(300 * kMsNs));
}

TEST_F(VkWaitUtilsTest, LongTimeoutIsSplitIntoSteps) {
    FakeWait wait = NeverSignaled();
    EXPECT_EQ(VK_TIMEOUT, Wait(2500 * kMsNs, Alive(), wait));
    EXPECT_THAT(wait.steps(), ElementsAre(kSecondNs, kSecondNs, 500 * kMsNs));
    EXPECT_EQ(static_cast<int64_t>(2500 * kMsNs), FakeClock::sNowNs);
}

TEST_F(VkWaitUtilsTest, ChargesOnlyTheTimeThatPassed) {
    // A driver that returns VK_TIMEOUT after half of each step.
    FakeWait wait([](uint64_t stepTimeoutNs) { return stepTimeoutNs / 2; },
                  [](size_t) { return VK_TIMEOUT; });
    EXPECT_EQ(VK_TIMEOUT, Wait(2500 * kMsNs, Alive(), wait));
    // 2.5 s left -> 2.0 -> 1.5 -> 1.0, which is the last step.
    EXPECT_THAT(wait.steps(), ElementsAre(kSecondNs, kSecondNs, kSecondNs, kSecondNs));
}

TEST_F(VkWaitUtilsTest, SignalAfterSomeStepsIsReturned) {
    FakeWait wait([](uint64_t stepTimeoutNs) { return stepTimeoutNs; },
                  [](size_t call) { return call < 2 ? VK_TIMEOUT : VK_SUCCESS; });
    EXPECT_EQ(VK_SUCCESS, Wait(10 * kSecondNs, Alive(), wait));
    EXPECT_THAT(wait.steps(), ElementsAre(kSecondNs, kSecondNs, kSecondNs));
}

TEST_F(VkWaitUtilsTest, InfiniteWaitKeepsWaitingWhileTheProcessLives) {
    FakeWait wait([](uint64_t stepTimeoutNs) { return stepTimeoutNs; },
                  [](size_t call) { return call < 30 ? VK_TIMEOUT : VK_SUCCESS; });
    EXPECT_EQ(VK_SUCCESS, Wait(UINT64_MAX, Alive(), wait));
    EXPECT_EQ(31u, wait.steps().size());
    for (uint64_t step : wait.steps()) {
        EXPECT_EQ(kSecondNs, step);
    }
}

TEST_F(VkWaitUtilsTest, InfiniteWaitStopsWhenTheProcessDies) {
    FakeWait wait = NeverSignaled();
    // The process is killed while the third step is waiting.
    auto isProcessDead = [&wait] { return wait.steps().size() >= 3; };
    EXPECT_EQ(VK_TIMEOUT, Wait(UINT64_MAX, isProcessDead, wait));
    EXPECT_THAT(wait.steps(), ElementsAre(kSecondNs, kSecondNs, kSecondNs));
}

TEST_F(VkWaitUtilsTest, FiniteWaitStopsWhenTheProcessDies) {
    FakeWait wait = NeverSignaled();
    auto isProcessDead = [&wait] { return wait.steps().size() >= 2; };
    EXPECT_EQ(VK_TIMEOUT, Wait(60 * kSecondNs, isProcessDead, wait));
    EXPECT_THAT(wait.steps(), ElementsAre(kSecondNs, kSecondNs));
}

TEST_F(VkWaitUtilsTest, DeadProcessDoesNotWaitAtAll) {
    FakeWait wait = NeverSignaled();
    EXPECT_EQ(VK_TIMEOUT, Wait(UINT64_MAX, [] { return true; }, wait));
    EXPECT_THAT(wait.steps(), IsEmpty());
}

TEST_F(VkWaitUtilsTest, RemainingTimeout) {
    EXPECT_EQ(7u, RemainingTimeoutNs(10, 3));
    EXPECT_EQ(0u, RemainingTimeoutNs(10, 10));
    EXPECT_EQ(0u, RemainingTimeoutNs(10, 20));
    EXPECT_EQ(UINT64_MAX, RemainingTimeoutNs(UINT64_MAX, 0));
    EXPECT_EQ(UINT64_MAX, RemainingTimeoutNs(UINT64_MAX, 5 * kSecondNs));
}

}  // namespace
}  // namespace vk
}  // namespace host
}  // namespace gfxstream
