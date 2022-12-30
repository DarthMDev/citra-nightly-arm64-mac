// Copyright 2022 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <algorithm>
#include "common/common_types.h"

// Include vulkan-hpp header
#define VK_ENABLE_BETA_EXTENSIONS
#define VK_NO_PROTOTYPES 1
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#define VULKAN_HPP_NO_CONSTRUCTORS
#define VULKAN_HPP_NO_STRUCT_SETTERS
#define VULKAN_HPP_NO_SMART_HANDLE
#include <vulkan/vulkan.hpp>

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1

namespace Vulkan {

/// Return the image aspect associated on the provided format
constexpr vk::ImageAspectFlags GetImageAspect(vk::Format format) {
    switch (format) {
    case vk::Format::eD16UnormS8Uint:
    case vk::Format::eD24UnormS8Uint:
    case vk::Format::eD32SfloatS8Uint:
        return vk::ImageAspectFlagBits::eStencil | vk::ImageAspectFlagBits::eDepth;
        break;
    case vk::Format::eD16Unorm:
    case vk::Format::eX8D24UnormPack32:
    case vk::Format::eD32Sfloat:
        return vk::ImageAspectFlagBits::eDepth;
        break;
    default:
        return vk::ImageAspectFlagBits::eColor;
    }
}

/// Returns a bit mask with the required usage of a format with a particular aspect
constexpr vk::ImageUsageFlags GetImageUsage(vk::ImageAspectFlags aspect) {
    auto usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst |
                 vk::ImageUsageFlagBits::eTransferSrc;

    if (aspect & vk::ImageAspectFlagBits::eDepth) {
        return usage | vk::ImageUsageFlagBits::eDepthStencilAttachment;
    } else {
        return usage | vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eColorAttachment;
    }
}

/// Returns a bit mask with the required features of a format with a particular aspect
constexpr vk::FormatFeatureFlags GetFormatFeatures(vk::ImageAspectFlags aspect) {
    auto usage = vk::FormatFeatureFlagBits::eSampledImage |
                 vk::FormatFeatureFlagBits::eTransferDst | vk::FormatFeatureFlagBits::eTransferSrc |
                 vk::FormatFeatureFlagBits::eBlitSrc | vk::FormatFeatureFlagBits::eBlitDst;

    if (aspect & vk::ImageAspectFlagBits::eDepth) {
        return usage | vk::FormatFeatureFlagBits::eDepthStencilAttachment;
    } else {
        return usage | vk::FormatFeatureFlagBits::eStorageImage |
               vk::FormatFeatureFlagBits::eColorAttachment;
    }
}

} // namespace Vulkan
