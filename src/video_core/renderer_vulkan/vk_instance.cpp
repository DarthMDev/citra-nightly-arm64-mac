// Copyright 2022 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <span>
#include "common/assert.h"
#include "common/settings.h"
#include "core/frontend/emu_window.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"

#include <vk_mem_alloc.h>

namespace Vulkan {

vk::DynamicLoader Instance::dl;

static VKAPI_ATTR VkBool32 VKAPI_CALL
DebugHandler(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT type,
             const VkDebugUtilsMessengerCallbackDataEXT* callback_data, void* user_data) {

    switch (callback_data->messageIdNumber) {
    case 0x609a13b: // Vertex attribute at location not consumed by shader
        return VK_FALSE;
    default:
        break;
    }

    Log::Level level{};
    switch (severity) {
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
        level = Log::Level::Error;
        break;
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
        level = Log::Level::Info;
        break;
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
        level = Log::Level::Debug;
        break;
    default:
        level = Log::Level::Info;
    }

    LOG_GENERIC(Log::Class::Render_Vulkan, level, "{}: {}", callback_data->pMessageIdName,
                callback_data->pMessage);

    return VK_FALSE;
}

vk::Format ToVkFormat(VideoCore::PixelFormat format) {
    switch (format) {
    case VideoCore::PixelFormat::RGBA8:
        return vk::Format::eR8G8B8A8Unorm;
    case VideoCore::PixelFormat::RGB8:
        return vk::Format::eB8G8R8Unorm;
    case VideoCore::PixelFormat::RGB5A1:
        return vk::Format::eR5G5B5A1UnormPack16;
    case VideoCore::PixelFormat::RGB565:
        return vk::Format::eR5G6B5UnormPack16;
    case VideoCore::PixelFormat::RGBA4:
        return vk::Format::eR4G4B4A4UnormPack16;
    case VideoCore::PixelFormat::D16:
        return vk::Format::eD16Unorm;
    case VideoCore::PixelFormat::D24:
        return vk::Format::eX8D24UnormPack32;
    case VideoCore::PixelFormat::D24S8:
        return vk::Format::eD24UnormS8Uint;
    case VideoCore::PixelFormat::Invalid:
        LOG_ERROR(Render_Vulkan, "Unknown texture format {}!", format);
        return vk::Format::eUndefined;
    default:
        // Use default case for the texture formats
        return vk::Format::eR8G8B8A8Unorm;
    }
}

[[nodiscard]] vk::DebugUtilsMessengerCreateInfoEXT MakeDebugUtilsMessengerInfo() {
    return vk::DebugUtilsMessengerCreateInfoEXT{
        .messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo |
                           vk::DebugUtilsMessageSeverityFlagBitsEXT::eError |
                           vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
                           vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose,
        .messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
                       vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
                       vk::DebugUtilsMessageTypeFlagBitsEXT::eDeviceAddressBinding |
                       vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance,
        .pfnUserCallback = DebugHandler,
    };
}

std::vector<std::string> GetSupportedExtensions(vk::PhysicalDevice physical) {
    const std::vector extensions = physical.enumerateDeviceExtensionProperties();
    std::vector<std::string> supported_extensions;
    supported_extensions.reserve(extensions.size());
    for (const auto& extension : extensions) {
        supported_extensions.emplace_back(extension.extensionName.data());
    }
    return supported_extensions;
}

Instance::Instance(bool validation, bool dump_command_buffers)
    : enable_validation{validation}, dump_command_buffers{dump_command_buffers} {
    auto vkGetInstanceProcAddr =
        dl.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr);

    // Enable the instance extensions the platform requires
    const std::vector extensions = GetInstanceExtensions(Frontend::WindowSystemType::Headless, false);

    const vk::ApplicationInfo application_info = {
        .pApplicationName = "Citra",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "Citra Vulkan",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_0,
    };

    std::array<const char*, 2> layers;
    u32 layer_count = 0;

    if (enable_validation) {
        layers[layer_count++] = "VK_LAYER_KHRONOS_validation";
    }
    if (dump_command_buffers) {
        layers[layer_count++] = "VK_LAYER_LUNARG_api_dump";
    }

    const vk::StructureChain instance_chain = {
        vk::InstanceCreateInfo{
            .flags = GetInstanceFlags(),
            .pApplicationInfo = &application_info,
            .enabledLayerCount = layer_count,
            .ppEnabledLayerNames = layers.data(),
            .enabledExtensionCount = static_cast<u32>(extensions.size()),
            .ppEnabledExtensionNames = extensions.data(),
        },
    };

    instance = vk::createInstance(instance_chain.get());

    // Load required function pointers for querying the physical device
    VULKAN_HPP_DEFAULT_DISPATCHER.vkEnumeratePhysicalDevices = PFN_vkEnumeratePhysicalDevices(
        vkGetInstanceProcAddr(instance, "vkEnumeratePhysicalDevices"));
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceProperties = PFN_vkGetPhysicalDeviceProperties(
        vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties"));
    VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyInstance =
        PFN_vkDestroyInstance(vkGetInstanceProcAddr(instance, "vkDestroyInstance"));

    physical_devices = instance.enumeratePhysicalDevices();
}

Instance::Instance(Frontend::EmuWindow& window, u32 physical_device_index)
    : enable_validation{Settings::values.renderer_debug},
      dump_command_buffers{Settings::values.dump_command_buffers} {
    const Frontend::EmuWindow::WindowSystemInfo window_info = window.GetWindowInfo();

    // Fetch instance independant function pointers
    auto vkGetInstanceProcAddr =
        dl.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr);

    // Enable the instance extensions the backend uses
    const std::vector extensions = GetInstanceExtensions(window_info.type, enable_validation);

    // We require a Vulkan 1.1 driver
    const u32 available_version = vk::enumerateInstanceVersion();
    if (available_version < VK_API_VERSION_1_1) {
        LOG_CRITICAL(Render_Vulkan, "Vulkan 1.0 is not supported, 1.1 is required!");
        return;
    }

    const vk::ApplicationInfo application_info = {
        .pApplicationName = "Citra",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "Citra Vulkan",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = available_version,
    };

    std::array<const char*, 3> layers;
#ifdef ANDROID
    u32 layer_count = 1;
    layers[0] = "VK_LAYER_KHRONOS_timeline_semaphore";
#else
    u32 layer_count = 0;
#endif

    if (enable_validation) {
        layers[layer_count++] = "VK_LAYER_KHRONOS_validation";
    }
    if (dump_command_buffers) {
        layers[layer_count++] = "VK_LAYER_LUNARG_api_dump";
    }

    vk::StructureChain instance_chain = {
        vk::InstanceCreateInfo{
            .flags = GetInstanceFlags(),
            .pApplicationInfo = &application_info,
            .enabledLayerCount = layer_count,
            .ppEnabledLayerNames = layers.data(),
            .enabledExtensionCount = static_cast<u32>(extensions.size()),
            .ppEnabledExtensionNames = extensions.data(),
        },
        MakeDebugUtilsMessengerInfo(),
    };

    try {
        instance = vk::createInstance(instance_chain.get());
    } catch (vk::LayerNotPresentError& err) {
        LOG_CRITICAL(Render_Vulkan, "Validation requested but layer is not available {}",
                     err.what());
        UNREACHABLE();
    }

    surface = CreateSurface(instance, window);

    // If validation is enabled attempt to also enable debug messenger
    if (enable_validation) {
        const auto it = std::find_if(extensions.begin(), extensions.end(), [](const char* extension) {
            return std::strcmp(extension, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0;
        });

        const bool debug_messenger_supported = it != extensions.end();
        if (debug_messenger_supported) {
            debug_messenger = instance.createDebugUtilsMessengerEXT(MakeDebugUtilsMessengerInfo());
        } else {
            instance_chain.unlink<vk::DebugUtilsMessengerCreateInfoEXT>();
        }
    }

    // Pick physical device
    physical_devices = instance.enumeratePhysicalDevices();
    if (const u16 physical_device_count = static_cast<u16>(physical_devices.size());
        physical_device_index >= physical_devices.size()) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan,
                     "Invalid physical device index {} provided when only {} devices exist",
                     physical_device_index, physical_device_count);
        UNREACHABLE();
    }

    physical_device = physical_devices[physical_device_index];
    properties = physical_device.getProperties();
    limits = properties.limits;

    LOG_INFO(Render_Vulkan, "Creating logical device for physical device: {}",
             properties.deviceName);

    CreateDevice();
    CreateFormatTable();
    CollectTelemetryParameters();
}

Instance::~Instance() {
    if (device) {
        vmaDestroyAllocator(allocator);
        device.destroy();

        if (debug_messenger) {
            instance.destroyDebugUtilsMessengerEXT(debug_messenger);
        }
    }

    instance.destroy();
}

FormatTraits Instance::GetTraits(VideoCore::PixelFormat pixel_format) const {
    if (pixel_format == VideoCore::PixelFormat::Invalid) [[unlikely]] {
        return FormatTraits{};
    }

    const u32 index = static_cast<u32>(pixel_format);
    return format_table[index];
}

void Instance::CreateFormatTable() {
    constexpr std::array pixel_formats = {
        VideoCore::PixelFormat::RGBA8,  VideoCore::PixelFormat::RGB8,
        VideoCore::PixelFormat::RGB5A1, VideoCore::PixelFormat::RGB565,
        VideoCore::PixelFormat::RGBA4,  VideoCore::PixelFormat::IA8,
        VideoCore::PixelFormat::RG8,    VideoCore::PixelFormat::I8,
        VideoCore::PixelFormat::A8,     VideoCore::PixelFormat::IA4,
        VideoCore::PixelFormat::I4,     VideoCore::PixelFormat::A4,
        VideoCore::PixelFormat::ETC1,   VideoCore::PixelFormat::ETC1A4,
        VideoCore::PixelFormat::D16,    VideoCore::PixelFormat::D24,
        VideoCore::PixelFormat::D24S8};

    const vk::FormatFeatureFlags storage_usage = vk::FormatFeatureFlagBits::eStorageImage;
    const vk::FormatFeatureFlags transfer_usage = vk::FormatFeatureFlagBits::eSampledImage;
    const vk::FormatFeatureFlags blit_usage =
        vk::FormatFeatureFlagBits::eBlitSrc | vk::FormatFeatureFlagBits::eBlitDst;

    for (const auto& pixel_format : pixel_formats) {
        const vk::Format format = ToVkFormat(pixel_format);
        const vk::FormatProperties properties = physical_device.getFormatProperties(format);
        const vk::ImageAspectFlags aspect = GetImageAspect(format);

        const vk::FormatFeatureFlagBits attachment_usage =
            (aspect & vk::ImageAspectFlagBits::eDepth)
                ? vk::FormatFeatureFlagBits::eDepthStencilAttachment
                : vk::FormatFeatureFlagBits::eColorAttachmentBlend;

        const bool supports_transfer =
            (properties.optimalTilingFeatures & transfer_usage) == transfer_usage;
        const bool supports_blit = (properties.optimalTilingFeatures & blit_usage) == blit_usage;
        bool supports_attachment =
            (properties.optimalTilingFeatures & attachment_usage) == attachment_usage;
        const bool supports_storage =
            (properties.optimalTilingFeatures & storage_usage) == storage_usage;

        // Find the most inclusive usage flags for this format
        vk::ImageUsageFlags best_usage;
        if (supports_blit || supports_transfer) {
            best_usage |= vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst |
                          vk::ImageUsageFlagBits::eTransferSrc;
        }
        if (supports_attachment) {
            best_usage |= (aspect & vk::ImageAspectFlagBits::eDepth)
                              ? vk::ImageUsageFlagBits::eDepthStencilAttachment
                              : vk::ImageUsageFlagBits::eColorAttachment;
        }
        if (supports_storage) {
            best_usage |= vk::ImageUsageFlagBits::eStorage;
        }

        // Always fallback to RGBA8 or D32(S8) for convenience
        vk::Format fallback = vk::Format::eR8G8B8A8Unorm;
        if (aspect & vk::ImageAspectFlagBits::eDepth) {
            fallback = vk::Format::eD32Sfloat;
            if (aspect & vk::ImageAspectFlagBits::eStencil) {
                fallback = vk::Format::eD32SfloatS8Uint;
            }
        }

        // Report completely unsupported formats
        if (!supports_blit && !supports_attachment && !supports_storage) {
            LOG_WARNING(Render_Vulkan, "Format {} unsupported, falling back unconditionally to {}",
                        vk::to_string(format), vk::to_string(fallback));
        }

        if (pixel_format == VideoCore::PixelFormat::RGB8) {
            supports_attachment = false;
        }

        const u32 index = static_cast<u32>(pixel_format);
        format_table[index] = FormatTraits{
            .transfer_support = supports_transfer,
            .blit_support = supports_blit,
            .attachment_support = supports_attachment,
            .storage_support = supports_storage,
            .usage = best_usage,
            .native = format,
            .fallback = fallback,
        };
    }
}

bool Instance::CreateDevice() {
    const vk::StructureChain feature_chain =
        physical_device.getFeatures2<vk::PhysicalDeviceFeatures2,
                                     vk::PhysicalDevicePortabilitySubsetFeaturesKHR,
                                     vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT,
                                     vk::PhysicalDeviceTimelineSemaphoreFeaturesKHR,
                                     vk::PhysicalDeviceCustomBorderColorFeaturesEXT,
                                     vk::PhysicalDeviceIndexTypeUint8FeaturesEXT>();

    // Not having geometry shaders will cause issues with accelerated rendering.
    features = feature_chain.get().features;
    if (!features.geometryShader) {
        LOG_WARNING(Render_Vulkan,
                    "Geometry shaders not availabe! Accelerated rendering not possible!");
    }

    available_extensions = GetSupportedExtensions(physical_device);
    if (available_extensions.empty()) {
        LOG_CRITICAL(Render_Vulkan, "No extensions supported by device.");
        return false;
    }

    // Helper lambda for adding extensions
    std::array<const char*, 10> enabled_extensions;
    u32 enabled_extension_count = 0;

    auto AddExtension = [&](std::string_view extension) -> bool {
        auto result = std::find_if(available_extensions.begin(), available_extensions.end(),
                                   [&](const std::string& name) { return name == extension; });

        if (result != available_extensions.end()) {
            LOG_INFO(Render_Vulkan, "Enabling extension: {}", extension);
            enabled_extensions[enabled_extension_count++] = extension.data();
            return true;
        }

        LOG_WARNING(Render_Vulkan, "Extension {} unavailable.", extension);
        return false;
    };

    AddExtension(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    // According to the Vulkan spec, VK_KHR_portability_subset must be added if supported.
    bool portability_subset = AddExtension(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
    timeline_semaphores = AddExtension(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
    extended_dynamic_state = AddExtension(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);
    push_descriptors = AddExtension(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    custom_border_color = AddExtension(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    index_type_uint8 = AddExtension(VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME);
    image_format_list = AddExtension(VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME);

    // Search queue families for graphics and present queues
    auto family_properties = physical_device.getQueueFamilyProperties();
    if (family_properties.empty()) {
        LOG_CRITICAL(Render_Vulkan, "Physical device reported no queues.");
        return false;
    }

    bool graphics_queue_found = false;
    bool present_queue_found = false;
    for (std::size_t i = 0; i < family_properties.size(); i++) {
        // Check if queue supports graphics
        const u32 index = static_cast<u32>(i);
        if (family_properties[i].queueFlags & vk::QueueFlagBits::eGraphics) {
            graphics_queue_family_index = index;
            graphics_queue_found = true;

            // If this queue also supports presentation we are finished
            if (physical_device.getSurfaceSupportKHR(static_cast<u32>(i), surface)) {
                present_queue_family_index = index;
                present_queue_found = true;
                break;
            }
        }

        // Check if queue supports presentation
        if (physical_device.getSurfaceSupportKHR(index, surface)) {
            present_queue_family_index = index;
            present_queue_found = true;
        }
    }

    if (!graphics_queue_found || !present_queue_found) {
        LOG_CRITICAL(Render_Vulkan, "Unable to find graphics and/or present queues.");
        return false;
    }

    static constexpr float queue_priorities[] = {1.0f};

    const std::array queue_infos = {
        vk::DeviceQueueCreateInfo{
            .queueFamilyIndex = graphics_queue_family_index,
            .queueCount = 1,
            .pQueuePriorities = queue_priorities,
        },
        vk::DeviceQueueCreateInfo{
            .queueFamilyIndex = present_queue_family_index,
            .queueCount = 1,
            .pQueuePriorities = queue_priorities,
        },
    };

    const u32 queue_count = graphics_queue_family_index != present_queue_family_index ? 2u : 1u;
    vk::StructureChain device_chain = {
        vk::DeviceCreateInfo{
            .queueCreateInfoCount = queue_count,
            .pQueueCreateInfos = queue_infos.data(),
            .enabledExtensionCount = enabled_extension_count,
            .ppEnabledExtensionNames = enabled_extensions.data(),
        },
        vk::PhysicalDeviceFeatures2{
            .features{
                .robustBufferAccess = features.robustBufferAccess,
                .geometryShader = features.geometryShader,
                .dualSrcBlend = features.dualSrcBlend,
                .logicOp = features.logicOp,
                .depthClamp = features.depthClamp,
                .largePoints = features.largePoints,
                .samplerAnisotropy = features.samplerAnisotropy,
                .fragmentStoresAndAtomics = features.fragmentStoresAndAtomics,
                .shaderStorageImageMultisample = features.shaderStorageImageMultisample,
                .shaderClipDistance = features.shaderClipDistance,
            },
        },
        feature_chain.get<vk::PhysicalDevicePortabilitySubsetFeaturesKHR>(),
        feature_chain.get<vk::PhysicalDeviceTimelineSemaphoreFeaturesKHR>(),
        feature_chain.get<vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT>(),
        feature_chain.get<vk::PhysicalDeviceCustomBorderColorFeaturesEXT>(),
        feature_chain.get<vk::PhysicalDeviceIndexTypeUint8FeaturesEXT>(),
    };

    if (portability_subset) {
        const vk::PhysicalDevicePortabilitySubsetFeaturesKHR portability_features =
            feature_chain.get<vk::PhysicalDevicePortabilitySubsetFeaturesKHR>();
        triangle_fan_supported = portability_features.triangleFans;

        const vk::StructureChain portability_properties_chain =
            physical_device.getProperties2<vk::PhysicalDeviceProperties2,
                                           vk::PhysicalDevicePortabilitySubsetPropertiesKHR>();
        const vk::PhysicalDevicePortabilitySubsetPropertiesKHR portability_properties =
            portability_properties_chain.get<vk::PhysicalDevicePortabilitySubsetPropertiesKHR>();
        min_vertex_stride_alignment = portability_properties.minVertexInputBindingStrideAlignment;
    } else {
        device_chain.unlink<vk::PhysicalDevicePortabilitySubsetFeaturesKHR>();
    }

    if (!index_type_uint8) {
        device_chain.unlink<vk::PhysicalDeviceIndexTypeUint8FeaturesEXT>();
    }

    if (!extended_dynamic_state) {
        device_chain.unlink<vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT>();
    }

    if (!custom_border_color) {
        device_chain.unlink<vk::PhysicalDeviceCustomBorderColorFeaturesEXT>();
    }

    try {
        device = physical_device.createDevice(device_chain.get());
    } catch (vk::ExtensionNotPresentError& err) {
        LOG_CRITICAL(Render_Vulkan, "Some required extensions are not available {}", err.what());
        UNREACHABLE();
    }

    VULKAN_HPP_DEFAULT_DISPATCHER.init(device);

    // Grab the graphics and present queues.
    graphics_queue = device.getQueue(graphics_queue_family_index, 0);
    present_queue = device.getQueue(present_queue_family_index, 0);

    CreateAllocator();
    return true;
}

void Instance::CreateAllocator() {
    const VmaVulkanFunctions functions = {
        .vkGetInstanceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr,
        .vkGetDeviceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr,
    };

    const VmaAllocatorCreateInfo allocator_info = {
        .physicalDevice = physical_device,
        .device = device,
        .pVulkanFunctions = &functions,
        .instance = instance,
        .vulkanApiVersion = VK_API_VERSION_1_1,
    };

    if (VkResult result = vmaCreateAllocator(&allocator_info, &allocator); result != VK_SUCCESS) {
        LOG_CRITICAL(Render_Vulkan, "Failed to initialize VMA with error {}", result);
        UNREACHABLE();
    }
}

void Instance::CollectTelemetryParameters() {
    const vk::StructureChain property_chain =
        physical_device
            .getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceDriverProperties>();
    const vk::PhysicalDeviceDriverProperties driver =
        property_chain.get<vk::PhysicalDeviceDriverProperties>();

    driver_id = driver.driverID;
    vendor_name = driver.driverName.data();
}

} // namespace Vulkan
