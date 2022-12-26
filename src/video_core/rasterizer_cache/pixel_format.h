// Copyright 2022 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <string_view>
#include "core/hw/gpu.h"
#include "video_core/regs_framebuffer.h"
#include "video_core/regs_texturing.h"

namespace VideoCore {

constexpr std::size_t PIXEL_FORMAT_COUNT = 18;

enum class PixelFormat : u32 {
    RGBA8 = 0,
    RGB8 = 1,
    RGB5A1 = 2,
    RGB565 = 3,
    RGBA4 = 4,
    IA8 = 5,
    RG8 = 6,
    I8 = 7,
    A8 = 8,
    IA4 = 9,
    I4 = 10,
    A4 = 11,
    ETC1 = 12,
    ETC1A4 = 13,
    D16 = 14,
    D24 = 16,
    D24S8 = 17,
    Max = 18,
    Invalid = 255,
};

enum class SurfaceType : u32 {
    Color = 0,
    Texture = 1,
    Depth = 2,
    DepthStencil = 3,
    Fill = 4,
    Invalid = 5,
};

enum class TextureType : u32 {
    Texture2D = 0,
    CubeMap = 1,
};

constexpr std::array<u8, PIXEL_FORMAT_COUNT> BITS_PER_BLOCK_TABLE = {{
    32, // RGBA8
    24, // RGB8
    16, // RGB5A1
    16, // RGB565
    16, // RGBA4
    16, // IA8
    16, // RG8
    8,  // I8
    8,  // A8
    8,  // IA4
    4,  // I4
    4,  // A4
    4,  // ETC1
    8,  // ETC1A4
    16, // D16
    0,
    24, // D24
    32, // D24S8
}};

constexpr u32 GetFormatBpp(PixelFormat format) {
    ASSERT(static_cast<std::size_t>(format) < BITS_PER_BLOCK_TABLE.size());
    return BITS_PER_BLOCK_TABLE[static_cast<std::size_t>(format)];
}

constexpr std::array<SurfaceType, PIXEL_FORMAT_COUNT> FORMAT_TYPE_TABLE = {{
    SurfaceType::Color,   // RGBA8
    SurfaceType::Color,   // RGB8
    SurfaceType::Color,   // RGB5A1
    SurfaceType::Color,   // RGB565
    SurfaceType::Color,   // RGBA4
    SurfaceType::Texture, // IA8
    SurfaceType::Texture, // RG8
    SurfaceType::Texture, // I8
    SurfaceType::Texture, // A8
    SurfaceType::Texture, // IA4
    SurfaceType::Texture, // I4
    SurfaceType::Texture, // A4
    SurfaceType::Texture, // ETC1
    SurfaceType::Texture, // ETC1A4
    SurfaceType::Depth,   // D16
    SurfaceType::Invalid,
    SurfaceType::Depth,        // D24
    SurfaceType::DepthStencil, // D24S8
}};

constexpr SurfaceType GetFormatType(PixelFormat format) {
    ASSERT(static_cast<std::size_t>(format) < FORMAT_TYPE_TABLE.size());
    return FORMAT_TYPE_TABLE[static_cast<std::size_t>(format)];
}

constexpr u32 GetBytesPerPixel(PixelFormat format) {
    // Modern GPUs need 4 bpp alignment for D24
    if (format == PixelFormat::D24 || GetFormatType(format) == SurfaceType::Texture) {
        return 4;
    }

    return GetFormatBpp(format) / 8;
}

std::string_view PixelFormatAsString(PixelFormat format);

bool CheckFormatsBlittable(PixelFormat source_format, PixelFormat dest_format);

PixelFormat PixelFormatFromTextureFormat(Pica::TexturingRegs::TextureFormat format);

PixelFormat PixelFormatFromColorFormat(Pica::FramebufferRegs::ColorFormat format);

PixelFormat PixelFormatFromDepthFormat(Pica::FramebufferRegs::DepthFormat format);

PixelFormat PixelFormatFromGPUPixelFormat(GPU::Regs::PixelFormat format);

} // namespace VideoCore
