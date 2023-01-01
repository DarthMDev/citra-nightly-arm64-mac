// Copyright 2022 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <bit>
#include "common/microprofile.h"
#include "video_core/rasterizer_cache/morton_swizzle.h"
#include "video_core/rasterizer_cache/utils.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_renderpass_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_texture_runtime.h"

#include <vk_mem_alloc.h>
#include <vulkan/vulkan_format_traits.hpp>

namespace Vulkan {

struct RecordParams {
    vk::ImageAspectFlags aspect;
    vk::Filter filter;
    vk::PipelineStageFlags pipeline_flags;
    vk::AccessFlags src_access;
    vk::AccessFlags dst_access;
    vk::Image src_image;
    vk::Image dst_image;
};

[[nodiscard]] vk::ImageAspectFlags MakeAspect(VideoCore::SurfaceType type) {
    switch (type) {
    case VideoCore::SurfaceType::Color:
    case VideoCore::SurfaceType::Texture:
    case VideoCore::SurfaceType::Fill:
        return vk::ImageAspectFlagBits::eColor;
    case VideoCore::SurfaceType::Depth:
        return vk::ImageAspectFlagBits::eDepth;
    case VideoCore::SurfaceType::DepthStencil:
        return vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
    default:
        LOG_CRITICAL(Render_Vulkan, "Invalid surface type {}", type);
        UNREACHABLE();
    }

    return vk::ImageAspectFlagBits::eColor;
}

[[nodiscard]] vk::Filter MakeFilter(VideoCore::PixelFormat pixel_format) {
    switch (pixel_format) {
    case VideoCore::PixelFormat::D16:
    case VideoCore::PixelFormat::D24:
    case VideoCore::PixelFormat::D24S8:
        return vk::Filter::eNearest;
    default:
        return vk::Filter::eLinear;
    }
}

[[nodiscard]] vk::ClearValue MakeClearValue(VideoCore::ClearValue clear) {
    static_assert(sizeof(VideoCore::ClearValue) == sizeof(vk::ClearValue));

    vk::ClearValue value{};
    std::memcpy(&value, &clear, sizeof(vk::ClearValue));
    return value;
}

[[nodiscard]] vk::ClearColorValue MakeClearColorValue(VideoCore::ClearValue clear) {
    return vk::ClearColorValue{
        .float32 = std::array{clear.color[0], clear.color[1], clear.color[2], clear.color[3]},
    };
}

[[nodiscard]] vk::ClearDepthStencilValue MakeClearDepthStencilValue(VideoCore::ClearValue clear) {
    return vk::ClearDepthStencilValue{
        .depth = clear.depth,
        .stencil = clear.stencil,
    };
}

u32 UnpackDepthStencil(const StagingData& data, vk::Format dest) {
    u32 depth_offset = 0;
    u32 stencil_offset = 4 * data.size / 5;
    const auto& mapped = data.mapped;

    switch (dest) {
    case vk::Format::eD24UnormS8Uint: {
        for (; stencil_offset < data.size; depth_offset += 4) {
            std::byte* ptr = mapped.data() + depth_offset;
            const u32 d24s8 = VideoCore::MakeInt<u32>(ptr);
            const u32 d24 = d24s8 >> 8;
            mapped[stencil_offset] = static_cast<std::byte>(d24s8 & 0xFF);
            std::memcpy(ptr, &d24, 4);
            stencil_offset++;
        }
        break;
    }
    default:
        LOG_ERROR(Render_Vulkan, "Unimplemtend convertion for depth format {}",
                  vk::to_string(dest));
        UNREACHABLE();
    }

    ASSERT(depth_offset == 4 * data.size / 5);
    return depth_offset;
}

constexpr u64 UPLOAD_BUFFER_SIZE = 32 * 1024 * 1024;
constexpr u64 DOWNLOAD_BUFFER_SIZE = 32 * 1024 * 1024;

TextureRuntime::TextureRuntime(const Instance& instance, Scheduler& scheduler,
                               RenderpassCache& renderpass_cache, DescriptorManager& desc_manager)
    : instance{instance}, scheduler{scheduler}, renderpass_cache{renderpass_cache},
      desc_manager{desc_manager}, blit_helper{instance, scheduler, desc_manager},
      upload_buffer{instance, scheduler, vk::BufferUsageFlagBits::eTransferSrc, UPLOAD_BUFFER_SIZE},
      download_buffer{instance, scheduler, vk::BufferUsageFlagBits::eTransferDst,
                      DOWNLOAD_BUFFER_SIZE, true} {

    auto Register = [this](VideoCore::PixelFormat dest,
                           std::unique_ptr<FormatReinterpreterBase>&& obj) {
        const u32 dst_index = static_cast<u32>(dest);
        return reinterpreters[dst_index].push_back(std::move(obj));
    };

    Register(VideoCore::PixelFormat::RGBA8,
             std::make_unique<D24S8toRGBA8>(instance, scheduler, desc_manager, *this));
}

TextureRuntime::~TextureRuntime() {
    VmaAllocator allocator = instance.GetAllocator();
    vk::Device device = instance.GetDevice();
    device.waitIdle();

    for (const auto& [key, alloc] : texture_recycler) {
        vmaDestroyImage(allocator, alloc.image, alloc.allocation);
        device.destroyImageView(alloc.image_view);
        if (alloc.base_view) {
            device.destroyImageView(alloc.base_view);
        }
        if (alloc.depth_view) {
            device.destroyImageView(alloc.depth_view);
            device.destroyImageView(alloc.stencil_view);
        }
        if (alloc.storage_view) {
            device.destroyImageView(alloc.storage_view);
        }
    }

    for (const auto& [key, framebuffer] : clear_framebuffers) {
        device.destroyFramebuffer(framebuffer);
    }

    texture_recycler.clear();
}

StagingData TextureRuntime::FindStaging(u32 size, bool upload) {
    auto& buffer = upload ? upload_buffer : download_buffer;
    auto [data, offset, invalidate] = buffer.Map(size, 4);

    return StagingData{
        .buffer = buffer.Handle(),
        .size = size,
        .mapped = std::span<std::byte>{reinterpret_cast<std::byte*>(data), size},
        .buffer_offset = offset,
    };
}

MICROPROFILE_DEFINE(Vulkan_Finish, "Vulkan", "Scheduler Finish", MP_RGB(52, 192, 235));
void TextureRuntime::Finish() {
    MICROPROFILE_SCOPE(Vulkan_Finish);
    scheduler.Finish();
}

ImageAlloc TextureRuntime::Allocate(u32 width, u32 height, VideoCore::PixelFormat format,
                                    VideoCore::TextureType type) {
    const FormatTraits traits = instance.GetTraits(format);
    const vk::ImageAspectFlags aspect = MakeAspect(VideoCore::GetFormatType(format));

    // Depth buffers are not supposed to support blit by the spec so don't require it.
    const bool is_suitable = traits.transfer_support && traits.attachment_support &&
                             (traits.blit_support || aspect & vk::ImageAspectFlagBits::eDepth);
    const vk::Format vk_format = is_suitable ? traits.native : traits.fallback;
    const vk::ImageUsageFlags vk_usage = is_suitable ? traits.usage : GetImageUsage(aspect);

    return Allocate(width, height, format, type, vk_format, vk_usage);
}

MICROPROFILE_DEFINE(Vulkan_ImageAlloc, "Vulkan", "TextureRuntime Finish", MP_RGB(192, 52, 235));
ImageAlloc TextureRuntime::Allocate(u32 width, u32 height, VideoCore::PixelFormat pixel_format,
                                    VideoCore::TextureType type, vk::Format format,
                                    vk::ImageUsageFlags usage) {
    MICROPROFILE_SCOPE(Vulkan_ImageAlloc);

    ImageAlloc alloc{};
    alloc.format = format;
    alloc.aspect = GetImageAspect(format);

    // The internal format does not provide enough guarantee of texture uniqueness
    // especially when many pixel formats fallback to RGBA8
    ASSERT(pixel_format != VideoCore::PixelFormat::Invalid);
    const HostTextureTag key = {
        .format = format,
        .pixel_format = pixel_format,
        .type = type,
        .width = width,
        .height = height,
    };

    // Attempt to recycle an unused allocation
    if (auto it = texture_recycler.find(key); it != texture_recycler.end()) {
        ImageAlloc alloc = std::move(it->second);
        texture_recycler.erase(it);
        return alloc;
    }

    const bool create_storage_view = pixel_format == VideoCore::PixelFormat::RGBA8;
    const u32 levels = std::log2(std::max(width, height)) + 1;
    const u32 layers = type == VideoCore::TextureType::CubeMap ? 6 : 1;

    vk::ImageCreateFlags flags;
    if (type == VideoCore::TextureType::CubeMap) {
        flags |= vk::ImageCreateFlagBits::eCubeCompatible;
    }
    if (create_storage_view) {
        flags |= vk::ImageCreateFlagBits::eMutableFormat;
    }

    const bool need_format_list = create_storage_view && instance.IsImageFormatListSupported();
    const vk::Format storage_format = vk::Format::eR32Uint;
    const vk::ImageFormatListCreateInfo image_format_list = {
        .viewFormatCount = 1,
        .pViewFormats = &storage_format,
    };

    const vk::ImageCreateInfo image_info = {
        .pNext = need_format_list ? &image_format_list : nullptr,
        .flags = flags,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = levels,
        .arrayLayers = layers,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = usage,
    };

    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    VkImage unsafe_image{};
    VkImageCreateInfo unsafe_image_info = static_cast<VkImageCreateInfo>(image_info);

    VkResult result = vmaCreateImage(instance.GetAllocator(), &unsafe_image_info, &alloc_info,
                                     &unsafe_image, &alloc.allocation, nullptr);
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating texture with error {}", result);
        UNREACHABLE();
    }

    const vk::ImageViewType view_type =
        type == VideoCore::TextureType::CubeMap ? vk::ImageViewType::eCube : vk::ImageViewType::e2D;

    alloc.image = vk::Image{unsafe_image};
    const vk::ImageViewCreateInfo view_info = {
        .image = alloc.image,
        .viewType = view_type,
        .format = format,
        .subresourceRange{
            .aspectMask = alloc.aspect,
            .baseMipLevel = 0,
            .levelCount = levels,
            .baseArrayLayer = 0,
            .layerCount = layers,
        },
    };

    vk::Device device = instance.GetDevice();
    alloc.image_view = device.createImageView(view_info);

    // Also create a base mip view in case this is used as an attachment
    if (levels > 1) [[likely]] {
        const vk::ImageViewCreateInfo base_view_info = {
            .image = alloc.image,
            .viewType = view_type,
            .format = format,
            .subresourceRange{
                .aspectMask = alloc.aspect,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = layers,
            },
        };

        alloc.base_view = device.createImageView(base_view_info);
    }

    // Create seperate depth/stencil views in case this gets reinterpreted with a compute shader
    if (alloc.aspect & vk::ImageAspectFlagBits::eStencil) {
        vk::ImageViewCreateInfo view_info = {
            .image = alloc.image,
            .viewType = view_type,
            .format = format,
            .subresourceRange{
                .aspectMask = vk::ImageAspectFlagBits::eDepth,
                .baseMipLevel = 0,
                .levelCount = levels,
                .baseArrayLayer = 0,
                .layerCount = layers,
            },
        };

        alloc.depth_view = device.createImageView(view_info);
        view_info.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eStencil;
        alloc.stencil_view = device.createImageView(view_info);
    }

    if (create_storage_view) {
        const vk::ImageViewCreateInfo storage_view_info = {
            .image = alloc.image,
            .viewType = view_type,
            .format = vk::Format::eR32Uint,
            .subresourceRange{
                .aspectMask = alloc.aspect,
                .baseMipLevel = 0,
                .levelCount = levels,
                .baseArrayLayer = 0,
                .layerCount = layers,
            },
        };
        alloc.storage_view = device.createImageView(storage_view_info);
    }

    scheduler.Record([image = alloc.image, aspect = alloc.aspect](vk::CommandBuffer cmdbuf) {
        const vk::ImageMemoryBarrier init_barrier = {
            .srcAccessMask = vk::AccessFlagBits::eNone,
            .dstAccessMask = vk::AccessFlagBits::eNone,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange{
                .aspectMask = aspect,
                .baseMipLevel = 0,
                .levelCount = VK_REMAINING_MIP_LEVELS,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        };

        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                                      vk::PipelineStageFlagBits::eTopOfPipe,
                                      vk::DependencyFlagBits::eByRegion, {}, {}, init_barrier);
    });

    return alloc;
}

void TextureRuntime::Recycle(const HostTextureTag tag, ImageAlloc&& alloc) {
    texture_recycler.emplace(tag, std::move(alloc));
}

void TextureRuntime::FormatConvert(const Surface& surface, bool upload, std::span<std::byte> source,
                                   std::span<std::byte> dest) {
    if (!NeedsConvertion(surface.pixel_format)) {
        std::memcpy(dest.data(), source.data(), source.size());
        return;
    }

    if (upload) {
        switch (surface.pixel_format) {
        case VideoCore::PixelFormat::RGBA8:
            return Pica::Texture::ConvertABGRToRGBA(source, dest);
        case VideoCore::PixelFormat::RGB8:
            return Pica::Texture::ConvertBGRToRGBA(source, dest);
        case VideoCore::PixelFormat::RGBA4:
            return Pica::Texture::ConvertRGBA4ToRGBA8(source, dest);
        default:
            break;
        }
    } else {
        switch (surface.pixel_format) {
        case VideoCore::PixelFormat::RGBA8:
            return Pica::Texture::ConvertABGRToRGBA(source, dest);
        case VideoCore::PixelFormat::RGBA4:
            return Pica::Texture::ConvertRGBA8ToRGBA4(source, dest);
        case VideoCore::PixelFormat::RGB8:
            return Pica::Texture::ConvertRGBAToBGR(source, dest);
        default:
            break;
        }
    }

    LOG_WARNING(Render_Vulkan, "Missing linear format convertion: {} {} {}",
                vk::to_string(surface.traits.native), upload ? "->" : "<-",
                vk::to_string(surface.alloc.format));
}

bool TextureRuntime::ClearTexture(Surface& surface, const VideoCore::TextureClear& clear,
                                  VideoCore::ClearValue value) {
    renderpass_cache.ExitRenderpass();

    const RecordParams params = {
        .aspect = surface.alloc.aspect,
        .pipeline_flags = surface.PipelineStageFlags(),
        .src_access = surface.AccessFlags(),
        .src_image = surface.alloc.image,
    };

    if (clear.texture_rect == surface.GetScaledRect()) {
        scheduler.Record([params, clear, value](vk::CommandBuffer cmdbuf) {
            const vk::ImageSubresourceRange range = {
                .aspectMask = params.aspect,
                .baseMipLevel = clear.texture_level,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            };

            const vk::ImageMemoryBarrier pre_barrier = {
                .srcAccessMask = params.src_access,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.src_image,
                .subresourceRange{
                    .aspectMask = params.aspect,
                    .baseMipLevel = clear.texture_level,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            };

            const vk::ImageMemoryBarrier post_barrier = {
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = params.src_access,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.src_image,
                .subresourceRange{
                    .aspectMask = params.aspect,
                    .baseMipLevel = clear.texture_level,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            };

            cmdbuf.pipelineBarrier(params.pipeline_flags,
                                          vk::PipelineStageFlagBits::eTransfer,
                                          vk::DependencyFlagBits::eByRegion, {}, {}, pre_barrier);

            const bool is_color =
                static_cast<bool>(params.aspect & vk::ImageAspectFlagBits::eColor);
            if (is_color) {
                cmdbuf.clearColorImage(params.src_image,
                                              vk::ImageLayout::eTransferDstOptimal,
                                              MakeClearColorValue(value), range);
            } else {
                cmdbuf.clearDepthStencilImage(params.src_image,
                                                     vk::ImageLayout::eTransferDstOptimal,
                                                     MakeClearDepthStencilValue(value), range);
            }

            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                          params.pipeline_flags, vk::DependencyFlagBits::eByRegion,
                                          {}, {}, post_barrier);
        });
        return true;
    }

    ClearTextureWithRenderpass(surface, clear, value);
    return true;
}

void TextureRuntime::ClearTextureWithRenderpass(Surface& surface,
                                                const VideoCore::TextureClear& clear,
                                                VideoCore::ClearValue value) {
    const bool is_color = surface.type != VideoCore::SurfaceType::Depth &&
                          surface.type != VideoCore::SurfaceType::DepthStencil;

    const vk::RenderPass clear_renderpass =
        is_color ? renderpass_cache.GetRenderpass(surface.pixel_format,
                                                  VideoCore::PixelFormat::Invalid, true)
                 : renderpass_cache.GetRenderpass(VideoCore::PixelFormat::Invalid,
                                                  surface.pixel_format, true);

    const vk::ImageView framebuffer_view = surface.GetFramebufferView();

    auto [it, new_framebuffer] =
        clear_framebuffers.try_emplace(framebuffer_view, vk::Framebuffer{});
    if (new_framebuffer) {
        const vk::FramebufferCreateInfo framebuffer_info = {
            .renderPass = clear_renderpass,
            .attachmentCount = 1,
            .pAttachments = &framebuffer_view,
            .width = surface.GetScaledWidth(),
            .height = surface.GetScaledHeight(),
            .layers = 1,
        };

        it->second = instance.GetDevice().createFramebuffer(framebuffer_info);
    }

    const RenderpassState clear_info = {
        .renderpass = clear_renderpass,
        .framebuffer = it->second,
        .render_area =
            vk::Rect2D{
                .offset = {static_cast<s32>(clear.texture_rect.left),
                           static_cast<s32>(clear.texture_rect.bottom)},
                .extent = {clear.texture_rect.GetWidth(), clear.texture_rect.GetHeight()},
            },
        .clear = MakeClearValue(value),
    };

    const RecordParams params = {
        .aspect = surface.alloc.aspect,
        .pipeline_flags = surface.PipelineStageFlags(),
        .src_access = surface.AccessFlags(),
        .src_image = surface.alloc.image,
    };

    scheduler.Record([params, level = clear.texture_level](vk::CommandBuffer cmdbuf) {
        const vk::ImageMemoryBarrier pre_barrier = {
            .srcAccessMask = params.src_access,
            .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = params.src_image,
            .subresourceRange{
                .aspectMask = params.aspect,
                .baseMipLevel = level,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        };

        cmdbuf.pipelineBarrier(params.pipeline_flags, vk::PipelineStageFlagBits::eTransfer,
                                      vk::DependencyFlagBits::eByRegion, {}, {}, pre_barrier);
    });

    renderpass_cache.EnterRenderpass(clear_info);
    renderpass_cache.ExitRenderpass();

    scheduler.Record([params, level = clear.texture_level](vk::CommandBuffer cmdbuf) {
        const vk::ImageMemoryBarrier post_barrier = {
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = params.src_access,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = params.src_image,
            .subresourceRange{
                .aspectMask = params.aspect,
                .baseMipLevel = level,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        };

        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, params.pipeline_flags,
                                      vk::DependencyFlagBits::eByRegion, {}, {}, post_barrier);
    });
}

bool TextureRuntime::CopyTextures(Surface& source, Surface& dest,
                                  const VideoCore::TextureCopy& copy) {
    renderpass_cache.ExitRenderpass();

    const RecordParams params = {
        .aspect = source.alloc.aspect,
        .filter = MakeFilter(source.pixel_format),
        .pipeline_flags = source.PipelineStageFlags() | dest.PipelineStageFlags(),
        .src_access = source.AccessFlags(),
        .dst_access = dest.AccessFlags(),
        .src_image = source.alloc.image,
        .dst_image = dest.alloc.image,
    };

    scheduler.Record([params, copy](vk::CommandBuffer cmdbuf) {
        const vk::ImageCopy image_copy = {
            .srcSubresource{
                .aspectMask = params.aspect,
                .mipLevel = copy.src_level,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
            .srcOffset = {static_cast<s32>(copy.src_offset.x), static_cast<s32>(copy.src_offset.y),
                          0},
            .dstSubresource{
                .aspectMask = params.aspect,
                .mipLevel = copy.dst_level,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
            .dstOffset = {static_cast<s32>(copy.dst_offset.x), static_cast<s32>(copy.dst_offset.y),
                          0},
            .extent = {copy.extent.width, copy.extent.height, 1},
        };

        const std::array pre_barriers = {
            vk::ImageMemoryBarrier{
                .srcAccessMask = params.src_access,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.src_image,
                .subresourceRange{
                    .aspectMask = params.aspect,
                    .baseMipLevel = copy.src_level,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = params.dst_access,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.dst_image,
                .subresourceRange{
                    .aspectMask = params.aspect,
                    .baseMipLevel = copy.dst_level,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            },
        };
        const std::array post_barriers = {
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eNone,
                .dstAccessMask = vk::AccessFlagBits::eNone,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.src_image,
                .subresourceRange{
                    .aspectMask = params.aspect,
                    .baseMipLevel = copy.src_level,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = params.dst_access,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.dst_image,
                .subresourceRange{
                    .aspectMask = params.aspect,
                    .baseMipLevel = copy.dst_level,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            },
        };

        cmdbuf.pipelineBarrier(params.pipeline_flags, vk::PipelineStageFlagBits::eTransfer,
                                      vk::DependencyFlagBits::eByRegion, {}, {}, pre_barriers);

        cmdbuf.copyImage(params.src_image, vk::ImageLayout::eTransferSrcOptimal,
                                params.dst_image, vk::ImageLayout::eTransferDstOptimal, image_copy);

        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, params.pipeline_flags,
                                      vk::DependencyFlagBits::eByRegion, {}, {}, post_barriers);
    });

    return true;
}

bool TextureRuntime::BlitTextures(Surface& source, Surface& dest,
                                  const VideoCore::TextureBlit& blit) {
    renderpass_cache.ExitRenderpass();

    const RecordParams params = {
        .aspect = source.alloc.aspect,
        .filter = MakeFilter(source.pixel_format),
        .pipeline_flags = source.PipelineStageFlags() | dest.PipelineStageFlags(),
        .src_access = source.AccessFlags(),
        .dst_access = dest.AccessFlags(),
        .src_image = source.alloc.image,
        .dst_image = dest.alloc.image,
    };

    scheduler.Record([params, blit](vk::CommandBuffer cmdbuf) {
        const std::array source_offsets = {
            vk::Offset3D{static_cast<s32>(blit.src_rect.left),
                         static_cast<s32>(blit.src_rect.bottom), 0},
            vk::Offset3D{static_cast<s32>(blit.src_rect.right), static_cast<s32>(blit.src_rect.top),
                         1},
        };

        const std::array dest_offsets = {
            vk::Offset3D{static_cast<s32>(blit.dst_rect.left),
                         static_cast<s32>(blit.dst_rect.bottom), 0},
            vk::Offset3D{static_cast<s32>(blit.dst_rect.right), static_cast<s32>(blit.dst_rect.top),
                         1},
        };

        const vk::ImageBlit blit_area = {
            .srcSubresource{
                .aspectMask = params.aspect,
                .mipLevel = blit.src_level,
                .baseArrayLayer = blit.src_layer,
                .layerCount = 1,
            },
            .srcOffsets = source_offsets,
            .dstSubresource{
                .aspectMask = params.aspect,
                .mipLevel = blit.dst_level,
                .baseArrayLayer = blit.dst_layer,
                .layerCount = 1,
            },
            .dstOffsets = dest_offsets,
        };

        const std::array read_barriers = {
            vk::ImageMemoryBarrier{
                .srcAccessMask = params.src_access,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.src_image,
                .subresourceRange{
                    .aspectMask = params.aspect,
                    .baseMipLevel = blit.src_level,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = params.dst_access,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.dst_image,
                .subresourceRange{
                    .aspectMask = params.aspect,
                    .baseMipLevel = blit.dst_level,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            },
        };
        const std::array write_barriers = {
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eNone,
                .dstAccessMask = vk::AccessFlagBits::eNone,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.src_image,
                .subresourceRange{
                    .aspectMask = params.aspect,
                    .baseMipLevel = blit.src_level,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = params.dst_access,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.dst_image,
                .subresourceRange{
                    .aspectMask = params.aspect,
                    .baseMipLevel = blit.dst_level,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            },
        };

        cmdbuf.pipelineBarrier(params.pipeline_flags, vk::PipelineStageFlagBits::eTransfer,
                                      vk::DependencyFlagBits::eByRegion, {}, {}, read_barriers);

        cmdbuf.blitImage(params.src_image, vk::ImageLayout::eTransferSrcOptimal,
                                params.dst_image, vk::ImageLayout::eTransferDstOptimal, blit_area,
                                params.filter);

        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, params.pipeline_flags,
                                      vk::DependencyFlagBits::eByRegion, {}, {}, write_barriers);
    });

    return true;
}

void TextureRuntime::GenerateMipmaps(Surface& surface, u32 max_level) {
    /*renderpass_cache.ExitRenderpass();

    // TODO: Investigate AMD single pass downsampler
    s32 current_width = surface.GetScaledWidth();
    s32 current_height = surface.GetScaledHeight();

    const u32 levels = std::bit_width(std::max(surface.width, surface.height));
    vk::ImageAspectFlags aspect = ToVkAspect(surface.type);
    vk::CommandBuffer command_buffer = scheduler.GetRenderCommandBuffer();
    for (u32 i = 1; i < levels; i++) {
        surface.Transition(vk::ImageLayout::eTransferSrcOptimal, i - 1, 1);
        surface.Transition(vk::ImageLayout::eTransferDstOptimal, i, 1);

        const std::array source_offsets = {vk::Offset3D{0, 0, 0},
                                           vk::Offset3D{current_width, current_height, 1}};

        const std::array dest_offsets = {
            vk::Offset3D{0, 0, 0}, vk::Offset3D{current_width > 1 ? current_width / 2 : 1,
                                                current_height > 1 ? current_height / 2 : 1, 1}};

        const vk::ImageBlit blit_area = {.srcSubresource = {.aspectMask = aspect,
                                                            .mipLevel = i - 1,
                                                            .baseArrayLayer = 0,
                                                            .layerCount = 1},
                                         .srcOffsets = source_offsets,
                                         .dstSubresource = {.aspectMask = aspect,
                                                            .mipLevel = i,
                                                            .baseArrayLayer = 0,
                                                            .layerCount = 1},
                                         .dstOffsets = dest_offsets};

        command_buffer.blitImage(surface.alloc.image, vk::ImageLayout::eTransferSrcOptimal,
                                 surface.alloc.image, vk::ImageLayout::eTransferDstOptimal,
                                 blit_area, vk::Filter::eLinear);
    }*/
}

const ReinterpreterList& TextureRuntime::GetPossibleReinterpretations(
    VideoCore::PixelFormat dest_format) const {
    return reinterpreters[static_cast<u32>(dest_format)];
}

bool TextureRuntime::NeedsConvertion(VideoCore::PixelFormat format) const {
    const FormatTraits traits = instance.GetTraits(format);
    const VideoCore::SurfaceType type = VideoCore::GetFormatType(format);
    return type == VideoCore::SurfaceType::Color &&
           (format == VideoCore::PixelFormat::RGBA8 || !traits.blit_support ||
            !traits.attachment_support);
}

Surface::Surface(TextureRuntime& runtime)
    : runtime{runtime}, instance{runtime.GetInstance()}, scheduler{runtime.GetScheduler()} {}

Surface::Surface(const VideoCore::SurfaceParams& params, TextureRuntime& runtime)
    : VideoCore::SurfaceBase<Surface>{params}, runtime{runtime}, instance{runtime.GetInstance()},
      scheduler{runtime.GetScheduler()}, traits{instance.GetTraits(pixel_format)} {

    if (pixel_format != VideoCore::PixelFormat::Invalid) {
        alloc = runtime.Allocate(GetScaledWidth(), GetScaledHeight(), params.pixel_format,
                                 texture_type);
    }
}

Surface::Surface(const VideoCore::SurfaceParams& params, vk::Format format,
                 vk::ImageUsageFlags usage, TextureRuntime& runtime)
    : VideoCore::SurfaceBase<Surface>{params}, runtime{runtime}, instance{runtime.GetInstance()},
      scheduler{runtime.GetScheduler()} {
    if (format != vk::Format::eUndefined) {
        alloc = runtime.Allocate(GetScaledWidth(), GetScaledHeight(), pixel_format, texture_type,
                                 format, usage);
    }
}

Surface::~Surface() {
    if (pixel_format != VideoCore::PixelFormat::Invalid) {
        const HostTextureTag tag = {
            .format = alloc.format,
            .pixel_format = pixel_format,
            .type = texture_type,
            .width = GetScaledWidth(),
            .height = GetScaledHeight(),
        };

        runtime.Recycle(tag, std::move(alloc));
    }
}

MICROPROFILE_DEFINE(Vulkan_Upload, "Vulkan", "Texture Upload", MP_RGB(128, 192, 64));
void Surface::Upload(const VideoCore::BufferTextureCopy& upload, const StagingData& staging) {
    MICROPROFILE_SCOPE(Vulkan_Upload);

    if (type == VideoCore::SurfaceType::DepthStencil && !traits.blit_support) {
        LOG_ERROR(Render_Vulkan, "Depth blit unsupported by hardware, ignoring");
        return;
    }

    runtime.renderpass_cache.ExitRenderpass();

    const bool is_scaled = res_scale != 1;
    if (is_scaled) {
        ScaledUpload(upload, staging);
    } else {
        const RecordParams params = {
            .aspect = alloc.aspect,
            .pipeline_flags = PipelineStageFlags(),
            .src_access = AccessFlags(),
            .src_image = alloc.image,
        };

        scheduler.Record([format = alloc.format, params, staging,
                          upload](vk::CommandBuffer cmdbuf) {
            u32 num_copies = 1;
            std::array<vk::BufferImageCopy, 2> buffer_image_copies;

            const VideoCore::Rect2D rect = upload.texture_rect;
            buffer_image_copies[0] = vk::BufferImageCopy{
                .bufferOffset = staging.buffer_offset + upload.buffer_offset,
                .bufferRowLength = rect.GetWidth(),
                .bufferImageHeight = rect.GetHeight(),
                .imageSubresource{
                    .aspectMask = params.aspect,
                    .mipLevel = upload.texture_level,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
                .imageOffset = {static_cast<s32>(rect.left), static_cast<s32>(rect.bottom), 0},
                .imageExtent = {rect.GetWidth(), rect.GetHeight(), 1},
            };

            if (params.aspect & vk::ImageAspectFlagBits::eStencil) {
                buffer_image_copies[0].imageSubresource.aspectMask =
                    vk::ImageAspectFlagBits::eDepth;
                vk::BufferImageCopy& stencil_copy = buffer_image_copies[1];
                stencil_copy = buffer_image_copies[0];
                stencil_copy.bufferOffset += UnpackDepthStencil(staging, format);
                stencil_copy.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eStencil;
                num_copies++;
            }

            const vk::ImageMemoryBarrier read_barrier = {
                .srcAccessMask = params.src_access,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.src_image,
                .subresourceRange{
                    .aspectMask = params.aspect,
                    .baseMipLevel = upload.texture_level,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            };
            const vk::ImageMemoryBarrier write_barrier = {
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = params.src_access,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.src_image,
                .subresourceRange{
                    .aspectMask = params.aspect,
                    .baseMipLevel = upload.texture_level,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            };

            cmdbuf.pipelineBarrier(params.pipeline_flags,
                                          vk::PipelineStageFlagBits::eTransfer,
                                          vk::DependencyFlagBits::eByRegion, {}, {}, read_barrier);

            cmdbuf.copyBufferToImage(staging.buffer, params.src_image,
                                            vk::ImageLayout::eTransferDstOptimal, num_copies,
                                            buffer_image_copies.data());

            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                          params.pipeline_flags, vk::DependencyFlagBits::eByRegion,
                                          {}, {}, write_barrier);
        });

        runtime.upload_buffer.Commit(staging.size);
    }

    InvalidateAllWatcher();
}

MICROPROFILE_DEFINE(Vulkan_Download, "Vulkan", "Texture Download", MP_RGB(128, 192, 64));
void Surface::Download(const VideoCore::BufferTextureCopy& download, const StagingData& staging) {
    MICROPROFILE_SCOPE(Vulkan_Download);

    runtime.renderpass_cache.ExitRenderpass();

    // For depth stencil downloads always use the compute shader fallback
    // to avoid having the interleave the data later. These should(?) be
    // uncommon anyways and the perf hit is very small
    if (type == VideoCore::SurfaceType::DepthStencil) {
        return DepthStencilDownload(download, staging);
    }

    const bool is_scaled = res_scale != 1;
    if (is_scaled) {
        ScaledDownload(download, staging);
    } else {
        const RecordParams params = {
            .aspect = alloc.aspect,
            .pipeline_flags = PipelineStageFlags(),
            .src_access = AccessFlags(),
            .src_image = alloc.image,
        };

        scheduler.Record([params, staging, download](vk::CommandBuffer cmdbuf) {
            const VideoCore::Rect2D rect = download.texture_rect;
            const vk::BufferImageCopy buffer_image_copy = {
                .bufferOffset = staging.buffer_offset + download.buffer_offset,
                .bufferRowLength = rect.GetWidth(),
                .bufferImageHeight = rect.GetHeight(),
                .imageSubresource{
                    .aspectMask = params.aspect,
                    .mipLevel = download.texture_level,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
                .imageOffset = {static_cast<s32>(rect.left), static_cast<s32>(rect.bottom), 0},
                .imageExtent = {rect.GetWidth(), rect.GetHeight(), 1},
            };

            const vk::ImageMemoryBarrier read_barrier = {
                .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.src_image,
                .subresourceRange{
                    .aspectMask = params.aspect,
                    .baseMipLevel = download.texture_level,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            };
            const vk::ImageMemoryBarrier image_write_barrier = {
                .srcAccessMask = vk::AccessFlagBits::eNone,
                .dstAccessMask = vk::AccessFlagBits::eMemoryWrite,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = params.src_image,
                .subresourceRange{
                    .aspectMask = params.aspect,
                    .baseMipLevel = download.texture_level,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            };
            const vk::MemoryBarrier memory_write_barrier = {
                .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
                .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
            };

            cmdbuf.pipelineBarrier(params.pipeline_flags,
                                          vk::PipelineStageFlagBits::eTransfer,
                                          vk::DependencyFlagBits::eByRegion, {}, {}, read_barrier);

            cmdbuf.copyImageToBuffer(params.src_image, vk::ImageLayout::eTransferSrcOptimal,
                                            staging.buffer, buffer_image_copy);

            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                          params.pipeline_flags, vk::DependencyFlagBits::eByRegion,
                                          memory_write_barrier, {}, image_write_barrier);
        });
        runtime.download_buffer.Commit(staging.size);
    }
}

u32 Surface::GetInternalBytesPerPixel() const {
    // Request 5 bytes for D24S8 as well because we can use the
    // extra space when deinterleaving the data during upload
    if (alloc.format == vk::Format::eD24UnormS8Uint) {
        return 5;
    }

    return vk::blockSize(alloc.format);
}

vk::AccessFlags Surface::AccessFlags() const noexcept {
    const bool is_color = static_cast<bool>(alloc.aspect & vk::ImageAspectFlagBits::eColor);
    const vk::AccessFlags attachment_flags =
        is_color
            ? vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite
            : vk::AccessFlagBits::eDepthStencilAttachmentRead |
                  vk::AccessFlagBits::eDepthStencilAttachmentWrite;

    return vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eTransferRead |
           vk::AccessFlagBits::eTransferWrite |
           (is_framebuffer ? attachment_flags : vk::AccessFlagBits::eNone) |
           (is_storage ? vk::AccessFlagBits::eShaderWrite : vk::AccessFlagBits::eNone);
}

vk::PipelineStageFlags Surface::PipelineStageFlags() const noexcept {
    const bool is_color = static_cast<bool>(alloc.aspect & vk::ImageAspectFlagBits::eColor);
    const vk::PipelineStageFlags attachment_flags =
        is_color ? vk::PipelineStageFlagBits::eColorAttachmentOutput
                 : vk::PipelineStageFlagBits::eEarlyFragmentTests |
                       vk::PipelineStageFlagBits::eLateFragmentTests;

    return vk::PipelineStageFlagBits::eTransfer | vk::PipelineStageFlagBits::eFragmentShader |
           (is_framebuffer ? attachment_flags : vk::PipelineStageFlagBits::eNone) |
           (is_storage ? vk::PipelineStageFlagBits::eComputeShader
                       : vk::PipelineStageFlagBits::eNone);
}

void Surface::ScaledUpload(const VideoCore::BufferTextureCopy& upload, const StagingData& staging) {
    const u32 rect_width = upload.texture_rect.GetWidth();
    const u32 rect_height = upload.texture_rect.GetHeight();
    const auto scaled_rect = upload.texture_rect * res_scale;
    const auto unscaled_rect = VideoCore::Rect2D{0, rect_height, rect_width, 0};

    SurfaceParams unscaled_params = *this;
    unscaled_params.width = rect_width;
    unscaled_params.stride = rect_width;
    unscaled_params.height = rect_height;
    unscaled_params.res_scale = 1;
    Surface unscaled_surface{unscaled_params, runtime};

    const VideoCore::BufferTextureCopy unscaled_upload = {
        .buffer_offset = upload.buffer_offset,
        .buffer_size = upload.buffer_size,
        .texture_rect = unscaled_rect,
    };

    unscaled_surface.Upload(unscaled_upload, staging);

    const VideoCore::TextureBlit blit = {
        .src_level = 0,
        .dst_level = upload.texture_level,
        .src_layer = 0,
        .dst_layer = 0,
        .src_rect = unscaled_rect,
        .dst_rect = scaled_rect,
    };

    runtime.BlitTextures(unscaled_surface, *this, blit);
}

void Surface::ScaledDownload(const VideoCore::BufferTextureCopy& download,
                             const StagingData& staging) {
    const u32 rect_width = download.texture_rect.GetWidth();
    const u32 rect_height = download.texture_rect.GetHeight();
    const VideoCore::Rect2D scaled_rect = download.texture_rect * res_scale;
    const VideoCore::Rect2D unscaled_rect = VideoCore::Rect2D{0, rect_height, rect_width, 0};

    // Allocate an unscaled texture that fits the download rectangle to use as a blit destination
    SurfaceParams unscaled_params = *this;
    unscaled_params.width = rect_width;
    unscaled_params.stride = rect_width;
    unscaled_params.height = rect_height;
    unscaled_params.res_scale = 1;
    Surface unscaled_surface{unscaled_params, runtime};

    const VideoCore::TextureBlit blit = {
        .src_level = download.texture_level,
        .dst_level = 0,
        .src_layer = 0,
        .dst_layer = 0,
        .src_rect = scaled_rect,
        .dst_rect = unscaled_rect,
    };

    // Blit the scaled rectangle to the unscaled texture
    runtime.BlitTextures(*this, unscaled_surface, blit);

    const VideoCore::BufferTextureCopy unscaled_download = {
        .buffer_offset = download.buffer_offset,
        .buffer_size = download.buffer_size,
        .texture_rect = unscaled_rect,
        .texture_level = 0,
    };

    unscaled_surface.Download(unscaled_download, staging);
}

void Surface::DepthStencilDownload(const VideoCore::BufferTextureCopy& download,
                                   const StagingData& staging) {
    const u32 rect_width = download.texture_rect.GetWidth();
    const u32 rect_height = download.texture_rect.GetHeight();
    const VideoCore::Rect2D scaled_rect = download.texture_rect * res_scale;
    const VideoCore::Rect2D unscaled_rect = VideoCore::Rect2D{0, rect_height, rect_width, 0};
    const VideoCore::Rect2D r32_scaled_rect =
        VideoCore::Rect2D{0, scaled_rect.GetHeight(), scaled_rect.GetWidth(), 0};

    // For depth downloads create an R32UI surface and use a compute shader for convert.
    // Then we blit and download that surface.
    // NOTE: We keep the pixel format to D24S8 to avoid linear filtering during scale
    SurfaceParams r32_params = *this;
    r32_params.width = scaled_rect.GetWidth();
    r32_params.stride = scaled_rect.GetWidth();
    r32_params.height = scaled_rect.GetHeight();
    r32_params.type = VideoCore::SurfaceType::Color;
    r32_params.res_scale = 1;
    Surface r32_surface{r32_params, vk::Format::eR32Uint,
                        vk::ImageUsageFlagBits::eTransferSrc |
                            vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eStorage,
                        runtime};

    const VideoCore::TextureBlit blit = {
        .src_level = download.texture_level,
        .dst_level = 0,
        .src_layer = 0,
        .dst_layer = 0,
        .src_rect = scaled_rect,
        .dst_rect = r32_scaled_rect,
    };

    runtime.blit_helper.BlitD24S8ToR32(*this, r32_surface, blit);

    // Blit the upper mip level to the lower one to scale without additional allocations
    const bool is_scaled = res_scale != 1;
    if (is_scaled) {
        const VideoCore::TextureBlit r32_blit = {
            .src_level = 0,
            .dst_level = 1,
            .src_layer = 0,
            .dst_layer = 0,
            .src_rect = r32_scaled_rect,
            .dst_rect = unscaled_rect,
        };

        runtime.BlitTextures(r32_surface, r32_surface, r32_blit);
    }

    const VideoCore::BufferTextureCopy r32_download = {
        .buffer_offset = download.buffer_offset,
        .buffer_size = download.buffer_size,
        .texture_rect = unscaled_rect,
        .texture_level = is_scaled ? 1u : 0u,
    };

    r32_surface.Download(r32_download, staging);
}

} // namespace Vulkan
