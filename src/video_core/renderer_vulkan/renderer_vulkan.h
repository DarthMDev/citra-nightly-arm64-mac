// Copyright 2022 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <glm/glm.hpp>
#include "common/common_types.h"
#include "common/math_util.h"
#include "core/hw/gpu.h"
#include "video_core/renderer_base.h"
#include "video_core/renderer_vulkan/vk_descriptor_manager.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_renderpass_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"

namespace Core {
class TelemetrySession;
}

namespace Layout {
struct FramebufferLayout;
}

namespace Vulkan {

/// Structure used for storing information about the textures for each 3DS screen
struct TextureInfo {
    ImageAlloc alloc;
    u32 width;
    u32 height;
    GPU::Regs::PixelFormat format;
};

/// Structure used for storing information about the display target for each 3DS screen
struct ScreenInfo {
    ImageAlloc* display_texture = nullptr;
    Common::Rectangle<float> display_texcoords;
    TextureInfo texture;
    vk::Sampler sampler;
};

// Uniform data used for presenting the 3DS screens
struct PresentUniformData {
    glm::mat4 modelview;
    Common::Vec4f i_resolution;
    Common::Vec4f o_resolution;
    int screen_id_l = 0;
    int screen_id_r = 0;
    int layer = 0;
    int reverse_interlaced = 0;

    // Returns an immutable byte view of the uniform data
    auto AsBytes() const {
        return std::as_bytes(std::span{this, 1});
    }
};

static_assert(sizeof(PresentUniformData) < 256, "PresentUniformData must be below 256 bytes!");

constexpr u32 PRESENT_PIPELINES = 3;

class RasterizerVulkan;

class RendererVulkan : public RendererBase {
public:
    explicit RendererVulkan(Frontend::EmuWindow& window, Frontend::EmuWindow* secondary_window);
    ~RendererVulkan() override;

    VideoCore::ResultStatus Init() override;
    VideoCore::RasterizerInterface* Rasterizer() override;
    void ShutDown() override;
    void SwapBuffers() override;
    void TryPresent(int timeout_ms, bool is_secondary) override {}
    void PrepareVideoDumping() override {}
    void CleanupVideoDumping() override {}
    void Sync() override;

private:
    void ReloadSampler();
    void ReloadPipeline();
    void CompileShaders();
    void BuildLayouts();
    void BuildPipelines();
    void ConfigureFramebufferTexture(TextureInfo& texture,
                                     const GPU::Regs::FramebufferConfig& framebuffer);
    void LoadColorToActiveVkTexture(u8 color_r, u8 color_g, u8 color_b, const TextureInfo& texture);
    void ConfigureRenderPipeline();
    void PrepareRendertarget();
    void BeginRendering();

    void DrawScreens(const Layout::FramebufferLayout& layout, bool flipped);
    void DrawSingleScreenRotated(u32 screen_id, float x, float y, float w, float h);
    void DrawSingleScreen(u32 screen_id, float x, float y, float w, float h);
    void DrawSingleScreenStereoRotated(u32 screen_id_l, u32 screen_id_r, float x, float y, float w,
                                       float h);
    void DrawSingleScreenStereo(u32 screen_id_l, u32 screen_id_r, float x, float y, float w,
                                float h);

    void UpdateFramerate();

    /// Loads framebuffer from emulated memory into the display information structure
    void LoadFBToScreenInfo(const GPU::Regs::FramebufferConfig& framebuffer,
                            ScreenInfo& screen_info, bool right_eye);

    void Report() const;

private:
    Core::TelemetrySession& telemetry_session;

    Instance instance;
    Scheduler scheduler;
    RenderpassCache renderpass_cache;
    DescriptorManager desc_manager;
    TextureRuntime runtime;
    Swapchain swapchain;
    StreamBuffer vertex_buffer;
    RasterizerVulkan rasterizer;

    // Present pipelines (Normal, Anaglyph, Interlaced)
    vk::PipelineLayout present_pipeline_layout;
    vk::DescriptorSetLayout present_descriptor_layout;
    vk::DescriptorUpdateTemplate present_update_template;
    std::array<vk::Pipeline, PRESENT_PIPELINES> present_pipelines;
    std::array<vk::DescriptorSet, PRESENT_PIPELINES> present_descriptor_sets;
    std::array<vk::ShaderModule, PRESENT_PIPELINES> present_shaders;
    std::array<vk::Sampler, 2> present_samplers;
    vk::ShaderModule present_vertex_shader;
    u32 current_pipeline = 0;
    u32 current_sampler = 0;

    /// Display information for top and bottom screens respectively
    std::array<ScreenInfo, 3> screen_infos{};
    PresentUniformData draw_info{};
    vk::ClearColorValue clear_color{};
};

} // namespace Vulkan
