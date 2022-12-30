// Copyright 2022 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <set>
#include <span>
#include <vulkan/vulkan_hash.hpp>
#include "video_core/rasterizer_cache/rasterizer_cache.h"
#include "video_core/rasterizer_cache/surface_base.h"
#include "video_core/renderer_vulkan/vk_blit_helper.h"
#include "video_core/renderer_vulkan/vk_format_reinterpreter.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_stream_buffer.h"

VK_DEFINE_HANDLE(VmaAllocation)

namespace Vulkan {

struct StagingData {
    vk::Buffer buffer;
    u32 size = 0;
    std::span<std::byte> mapped{};
    u64 buffer_offset = 0;
};

struct ImageAlloc {
    ImageAlloc() = default;

    ImageAlloc(const ImageAlloc&) = delete;
    ImageAlloc& operator=(const ImageAlloc&) = delete;

    ImageAlloc(ImageAlloc&&) = default;
    ImageAlloc& operator=(ImageAlloc&&) = default;

    vk::Image image;
    vk::ImageView image_view;
    vk::ImageView base_view;
    vk::ImageView depth_view;
    vk::ImageView stencil_view;
    vk::ImageView storage_view;
    VmaAllocation allocation;
    vk::ImageUsageFlags usage;
    vk::Format format;
    vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor;
};

struct HostTextureTag {
    vk::Format format = vk::Format::eUndefined;
    VideoCore::PixelFormat pixel_format = VideoCore::PixelFormat::Invalid;
    VideoCore::TextureType type = VideoCore::TextureType::Texture2D;
    u32 width = 1;
    u32 height = 1;

    auto operator<=>(const HostTextureTag&) const noexcept = default;

    const u64 Hash() const {
        return Common::ComputeHash64(this, sizeof(HostTextureTag));
    }
};

} // namespace Vulkan

namespace std {
template <>
struct hash<Vulkan::HostTextureTag> {
    std::size_t operator()(const Vulkan::HostTextureTag& tag) const noexcept {
        return tag.Hash();
    }
};
} // namespace std

namespace Vulkan {

class Instance;
class RenderpassCache;
class DescriptorManager;
class Surface;

/**
 * Provides texture manipulation functions to the rasterizer cache
 * Separating this into a class makes it easier to abstract graphics API code
 */
class TextureRuntime {
    friend class Surface;

public:
    TextureRuntime(const Instance& instance, Scheduler& scheduler,
                   RenderpassCache& renderpass_cache, DescriptorManager& desc_manager);
    ~TextureRuntime();

    /// Causes a GPU command flush
    void Finish();

    /// Takes back ownership of the allocation for recycling
    void Recycle(const HostTextureTag tag, ImageAlloc&& alloc);

    /// Maps an internal staging buffer of the provided size of pixel uploads/downloads
    [[nodiscard]] StagingData FindStaging(u32 size, bool upload);

    /// Allocates a vulkan image possibly resusing an existing one
    [[nodiscard]] ImageAlloc Allocate(u32 width, u32 height, VideoCore::PixelFormat format,
                                      VideoCore::TextureType type);

    /// Allocates a vulkan image
    [[nodiscard]] ImageAlloc Allocate(u32 width, u32 height, VideoCore::PixelFormat pixel_format,
                                      VideoCore::TextureType type, vk::Format format,
                                      vk::ImageUsageFlags usage);

    /// Performs required format convertions on the staging data
    void FormatConvert(const Surface& surface, bool upload, std::span<std::byte> source,
                       std::span<std::byte> dest);

    /// Fills the rectangle of the texture with the clear value provided
    bool ClearTexture(Surface& surface, const VideoCore::TextureClear& clear,
                      VideoCore::ClearValue value);

    /// Copies a rectangle of src_tex to another rectange of dst_rect
    bool CopyTextures(Surface& source, Surface& dest, const VideoCore::TextureCopy& copy);

    /// Blits a rectangle of src_tex to another rectange of dst_rect
    bool BlitTextures(Surface& surface, Surface& dest, const VideoCore::TextureBlit& blit);

    /// Generates mipmaps for all the available levels of the texture
    void GenerateMipmaps(Surface& surface, u32 max_level);

    /// Returns all source formats that support reinterpretation to the dest format
    [[nodiscard]] const ReinterpreterList& GetPossibleReinterpretations(
        VideoCore::PixelFormat dest_format) const;

    /// Returns true if the provided pixel format needs convertion
    [[nodiscard]] bool NeedsConvertion(VideoCore::PixelFormat format) const;

    /// Returns a reference to the renderpass cache
    [[nodiscard]] RenderpassCache& GetRenderpassCache() {
        return renderpass_cache;
    }

private:
    /// Clears a partial texture rect using a clear rectangle
    void ClearTextureWithRenderpass(Surface& surface, const VideoCore::TextureClear& clear,
                                    VideoCore::ClearValue value);

    /// Returns the current Vulkan instance
    const Instance& GetInstance() const {
        return instance;
    }

    /// Returns the current Vulkan scheduler
    Scheduler& GetScheduler() const {
        return scheduler;
    }

private:
    const Instance& instance;
    Scheduler& scheduler;
    RenderpassCache& renderpass_cache;
    DescriptorManager& desc_manager;
    BlitHelper blit_helper;
    StreamBuffer upload_buffer;
    StreamBuffer download_buffer;
    std::array<ReinterpreterList, VideoCore::PIXEL_FORMAT_COUNT> reinterpreters;
    std::unordered_multimap<HostTextureTag, ImageAlloc> texture_recycler;
    std::unordered_map<vk::ImageView, vk::Framebuffer> clear_framebuffers;
};

class Surface : public VideoCore::SurfaceBase<Surface> {
    friend class TextureRuntime;
    friend class RasterizerVulkan;

public:
    Surface(TextureRuntime& runtime);
    Surface(const VideoCore::SurfaceParams& params, TextureRuntime& runtime);
    Surface(const VideoCore::SurfaceParams& params, vk::Format format, vk::ImageUsageFlags usage,
            TextureRuntime& runtime);
    ~Surface() override;

    /// Uploads pixel data in staging to a rectangle region of the surface texture
    void Upload(const VideoCore::BufferTextureCopy& upload, const StagingData& staging);

    /// Downloads pixel data to staging from a rectangle region of the surface texture
    void Download(const VideoCore::BufferTextureCopy& download, const StagingData& staging);

    /// Returns the bpp of the internal surface format
    u32 GetInternalBytesPerPixel() const;

    /// Returns the access flags indicative of the surface
    vk::AccessFlags AccessFlags() const noexcept;

    /// Returns the pipeline stage flags indicative of the surface
    vk::PipelineStageFlags PipelineStageFlags() const noexcept;

    /// Returns an image view used to sample the surface from a shader
    vk::ImageView GetImageView() const noexcept {
        return alloc.image_view;
    }

    /// Returns an image view used to create a framebuffer
    vk::ImageView GetFramebufferView() noexcept {
        is_framebuffer = true;
        return alloc.base_view;
    }

    /// Returns the depth only image view of the surface, null otherwise
    vk::ImageView GetDepthView() const noexcept {
        return alloc.depth_view;
    }

    /// Returns the stencil only image view of the surface, null otherwise
    vk::ImageView GetStencilView() const noexcept {
        return alloc.stencil_view;
    }

    /// Returns the R32 image view used for atomic load/store
    vk::ImageView GetStorageView() noexcept {
        if (!alloc.storage_view) {
            LOG_CRITICAL(Render_Vulkan,
                         "Surface with pixel format {} and internal format {} "
                         "does not provide requested storage view!",
                         VideoCore::PixelFormatAsString(pixel_format), vk::to_string(alloc.format));
            UNREACHABLE();
        }
        is_storage = true;
        return alloc.storage_view;
    }

    /// Returns the internal format of the allocated texture
    vk::Format GetInternalFormat() const noexcept {
        return alloc.format;
    }

private:
    /// Uploads pixel data to scaled texture
    void ScaledUpload(const VideoCore::BufferTextureCopy& upload, const StagingData& staging);

    /// Downloads scaled image by downscaling the requested rectangle
    void ScaledDownload(const VideoCore::BufferTextureCopy& download, const StagingData& stagings);

    /// Downloads scaled depth stencil data
    void DepthStencilDownload(const VideoCore::BufferTextureCopy& download,
                              const StagingData& staging);

private:
    TextureRuntime& runtime;
    const Instance& instance;
    Scheduler& scheduler;

public:
    bool is_framebuffer{};
    bool is_storage{};
    ImageAlloc alloc;
    FormatTraits traits;
};

struct Traits {
    using RuntimeType = TextureRuntime;
    using SurfaceType = Surface;
};

using RasterizerCache = VideoCore::RasterizerCache<Traits>;

} // namespace Vulkan
