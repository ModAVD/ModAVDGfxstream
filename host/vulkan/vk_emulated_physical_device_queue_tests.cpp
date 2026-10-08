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

#include "vk_emulated_physical_device_queue.h"

#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <vulkan/vulkan.h>

#include "gfxstream/host/features.h"

namespace gfxstream {
namespace host {
namespace vk {
namespace {

using ::testing::AllOf;
using ::testing::ElementsAreArray;
using ::testing::Eq;
using ::testing::ExplainMatchResult;
using ::testing::Field;
using ::testing::Matcher;
using ::testing::Optional;

MATCHER_P(EqsVkExtent3D, expected, "") {
    return ExplainMatchResult(AllOf(Field("width", &VkExtent3D::width, Eq(expected.width)),
                                    Field("height", &VkExtent3D::height, Eq(expected.height)),
                                    Field("depth", &VkExtent3D::depth, Eq(expected.depth))),
                              arg, result_listener);
}

MATCHER_P(EqsVkQueueFamilyProperties, expected, "") {
    return ExplainMatchResult(
        AllOf(Field("queueFlags", &VkQueueFamilyProperties::queueFlags, Eq(expected.queueFlags)),
              Field("queueCount", &VkQueueFamilyProperties::queueCount, Eq(expected.queueCount)),
              Field("timestampValidBits", &VkQueueFamilyProperties::timestampValidBits,
                    Eq(expected.timestampValidBits)),
              Field("minImageTransferGranularity",
                    &VkQueueFamilyProperties::minImageTransferGranularity,
                    EqsVkExtent3D(expected.minImageTransferGranularity))),
        arg, result_listener);
}

// Use VulkanVirtualQueue feature to multiplex physical queues.
TEST(VkGuestQueueUtilsTest, Passthrough) {
    const std::vector<VkQueueFamilyProperties> hostQueueFamilyProperties = {
        {.queueFlags = VK_QUEUE_GRAPHICS_BIT,
         .queueCount = 1,
         .timestampValidBits = 16,
         .minImageTransferGranularity = {1, 1, 1}}};

    gfxstream::host::FeatureSet features;
    EmulatedPhysicalDeviceQueueProperties helper(hostQueueFamilyProperties, features);

    // Passthrough when no features enabled:
    const auto actualQueueProperties = helper.getQueueFamilyProperties();
    EXPECT_THAT(actualQueueProperties.size(), 1);
    EXPECT_THAT(actualQueueProperties[0], EqsVkQueueFamilyProperties(hostQueueFamilyProperties[0]));
}

// Use VulkanVirtualQueue feature to multiplex physical queues.
TEST(VkGuestQueueUtilsTest, VulkanVirtualQueue) {
    const std::vector<VkQueueFamilyProperties> hostQueueFamilyProperties = {
        {.queueFlags = VK_QUEUE_GRAPHICS_BIT,
         .queueCount = 1,
         .timestampValidBits = 16,
         .minImageTransferGranularity = {1, 1, 1}}};

    const std::vector<VkQueueFamilyProperties> expectedQueueFamilyProperties = {
        {.queueFlags = VK_QUEUE_GRAPHICS_BIT,
         .queueCount = 2,
         .timestampValidBits = 16,
         .minImageTransferGranularity = {1, 1, 1}}};

    // Enable VulkanVirtualQueue, expect 2 graphics queues
    gfxstream::host::FeatureSet features;
    features.Vulkan.setEnabled(true);
    features.VulkanVirtualQueue.setEnabled(true);

    EmulatedPhysicalDeviceQueueProperties helper(hostQueueFamilyProperties, features);

    const auto actualQueueProperties = helper.getQueueFamilyProperties();
    EXPECT_THAT(actualQueueProperties.size(), 1);
    EXPECT_THAT(actualQueueProperties[0], EqsVkQueueFamilyProperties(expectedQueueFamilyProperties[0]));
}

// Use VulkanProtectedMemoryEmulation feature to emulate protected queues.
TEST(VkGuestQueueUtilsTest, VulkanProtectedQueue) {
    const std::vector<VkQueueFamilyProperties> hostQueueFamilyProperties = {
        {.queueFlags = VK_QUEUE_GRAPHICS_BIT,
         .queueCount = 1,
         .timestampValidBits = 16,
         .minImageTransferGranularity = {1, 1, 1}}};

    const std::vector<VkQueueFamilyProperties> expectedQueueFamilyProperties = {
        {.queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_PROTECTED_BIT,
         .queueCount = 1,
         .timestampValidBits = 16,
         .minImageTransferGranularity = {1, 1, 1}}};

    // Enable VulkanProtectedMemoryEmulation, expect 1 protected graphics queue
    gfxstream::host::FeatureSet features;
    features.Vulkan.setEnabled(true);
    features.VulkanProtectedMemoryEmulation.setEnabled(true);

    EmulatedPhysicalDeviceQueueProperties helper(hostQueueFamilyProperties, features);

    const auto actualQueueProperties = helper.getQueueFamilyProperties();
    EXPECT_THAT(actualQueueProperties.size(), 1);
    EXPECT_THAT(actualQueueProperties[0], EqsVkQueueFamilyProperties(expectedQueueFamilyProperties[0]));
}

// Use VulkanVirtualQueue+VulkanProtectedMemoryEmulation
TEST(VkGuestQueueUtilsTest, VulkanProtectedVirtualQueue) {
    const std::vector<VkQueueFamilyProperties> hostQueueFamilyProperties = {
        {.queueFlags = VK_QUEUE_GRAPHICS_BIT,
         .queueCount = 1,
         .timestampValidBits = 16,
         .minImageTransferGranularity = {1, 1, 1}}};

    const std::vector<VkQueueFamilyProperties> expectedQueueFamilyProperties = {
        {.queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_PROTECTED_BIT,
         .queueCount = 2,
         .timestampValidBits = 16,
         .minImageTransferGranularity = {1, 1, 1}}};

    // Enable VulkanVirtualQueue and VulkanProtectedMemoryEmulation, expect 2 graphics queues with
    // protected bit
    gfxstream::host::FeatureSet features;
    features.Vulkan.setEnabled(true);
    features.VulkanVirtualQueue.setEnabled(true);
    features.VulkanProtectedMemoryEmulation.setEnabled(true);

    EmulatedPhysicalDeviceQueueProperties helper(hostQueueFamilyProperties, features);

    const auto actualQueueProperties = helper.getQueueFamilyProperties();
    EXPECT_THAT(actualQueueProperties.size(), 1);
    EXPECT_THAT(actualQueueProperties[0], EqsVkQueueFamilyProperties(expectedQueueFamilyProperties[0]));
}

// Gfxstream supports ETC2 emulation by inspecting `vkCmdPipelineBarrier()` calls
// and inserting extra commands into the provided command buffers to dispatch
// compute shaders. Gfxstream, however, does not control which queues these command
// buffers are submitted to so we must hide transfer-only queues to ensure compute
// shader work is not scheduled on transfer-only queues.
TEST(VkGuestQueueUtilsTest, VulkanEtc2EmulationHidesTransferOnlyQueue) {
    const std::vector<VkQueueFamilyProperties> hostQueueFamilyProperties = {
        {.queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT,
         .queueCount = 16,
         .timestampValidBits = 64,
         .minImageTransferGranularity = {1, 1, 1}},
        {.queueFlags = VK_QUEUE_TRANSFER_BIT | VK_QUEUE_SPARSE_BINDING_BIT,
         .queueCount = 2,
         .timestampValidBits = 64,
         .minImageTransferGranularity = {1, 1, 1}},
        {.queueFlags = VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT,
         .queueCount = 8,
         .timestampValidBits = 64,
         .minImageTransferGranularity = {1, 1, 1}}};

    const std::vector<VkQueueFamilyProperties> expectedQueueFamilyProperties = {
        {.queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT,
         .queueCount = 16,
         .timestampValidBits = 64,
         .minImageTransferGranularity = {1, 1, 1}},
        // NOTE: VK_QUEUE_TRANSFER_BIT is removed here:
        {.queueFlags = VK_QUEUE_SPARSE_BINDING_BIT,
         .queueCount = 2,
         .timestampValidBits = 64,
         .minImageTransferGranularity = {1, 1, 1}},
        {.queueFlags = VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT,
         .queueCount = 8,
         .timestampValidBits = 64,
         .minImageTransferGranularity = {1, 1, 1}}};

    gfxstream::host::FeatureSet features;
    features.Vulkan.setEnabled(true);
    features.VulkanEtc2Emulation.setEnabled(true);

    EmulatedPhysicalDeviceQueueProperties helper(hostQueueFamilyProperties, features);

    const auto actualQueueProperties = helper.getQueueFamilyProperties();
    EXPECT_THAT(actualQueueProperties.size(), 3);
    EXPECT_THAT(actualQueueProperties[0], EqsVkQueueFamilyProperties(expectedQueueFamilyProperties[0]));
    EXPECT_THAT(actualQueueProperties[1], EqsVkQueueFamilyProperties(expectedQueueFamilyProperties[1]));
    EXPECT_THAT(actualQueueProperties[2], EqsVkQueueFamilyProperties(expectedQueueFamilyProperties[2]));
}

// Gfxstream supports ASTC emulation by inspecting `vkCmdPipelineBarrier()` calls
// and inserting extra commands into the provided command buffers to dispatch
// compute shaders. Gfxstream, however, does not control which queues these command
// buffers are submitted to so we must hide transfer-only queues to ensure compute
// shader work is not scheduled on transfer-only queues.
TEST(VkGuestQueueUtilsTest, VulkanAstcEmulationHidesTransferOnlyQueue) {
    const std::vector<VkQueueFamilyProperties> hostQueueFamilyProperties = {
        {.queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT,
         .queueCount = 16,
         .timestampValidBits = 64,
         .minImageTransferGranularity = {1, 1, 1}},
        {.queueFlags = VK_QUEUE_TRANSFER_BIT | VK_QUEUE_SPARSE_BINDING_BIT,
         .queueCount = 2,
         .timestampValidBits = 64,
         .minImageTransferGranularity = {1, 1, 1}},
        {.queueFlags = VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT,
         .queueCount = 8,
         .timestampValidBits = 64,
         .minImageTransferGranularity = {1, 1, 1}}};

    const std::vector<VkQueueFamilyProperties> expectedQueueFamilyProperties = {
        {.queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT,
         .queueCount = 16,
         .timestampValidBits = 64,
         .minImageTransferGranularity = {1, 1, 1}},
        // NOTE: VK_QUEUE_TRANSFER_BIT is removed here:
        {.queueFlags = VK_QUEUE_SPARSE_BINDING_BIT,
         .queueCount = 2,
         .timestampValidBits = 64,
         .minImageTransferGranularity = {1, 1, 1}},
        {.queueFlags = VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT,
         .queueCount = 8,
         .timestampValidBits = 64,
         .minImageTransferGranularity = {1, 1, 1}}};

    gfxstream::host::FeatureSet features;
    features.Vulkan.setEnabled(true);
    features.VulkanAstcEmulation.setEnabled(true);

    EmulatedPhysicalDeviceQueueProperties helper(hostQueueFamilyProperties, features);

    const auto actualQueueProperties = helper.getQueueFamilyProperties();
    EXPECT_THAT(actualQueueProperties.size(), 3);
    EXPECT_THAT(actualQueueProperties[0], EqsVkQueueFamilyProperties(expectedQueueFamilyProperties[0]));
    EXPECT_THAT(actualQueueProperties[1], EqsVkQueueFamilyProperties(expectedQueueFamilyProperties[1]));
    EXPECT_THAT(actualQueueProperties[2], EqsVkQueueFamilyProperties(expectedQueueFamilyProperties[2]));
}

}  // namespace
}  // namespace vk
}  // namespace host
}  // namespace gfxstream