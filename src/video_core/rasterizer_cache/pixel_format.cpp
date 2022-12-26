// Copyright 2022 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "video_core/rasterizer_cache/pixel_format.h"

namespace VideoCore {

std::string_view PixelFormatAsString(PixelFormat format) {
    switch (format) {
    case PixelFormat::RGBA8:
        return "RGBA8";
    case PixelFormat::RGB8:
        return "RGB8";
    case PixelFormat::RGB5A1:
        return "RGB5A1";
    case PixelFormat::RGB565:
        return "RGB565";
    case PixelFormat::RGBA4:
        return "RGBA4";
    case PixelFormat::IA8:
        return "IA8";
    case PixelFormat::RG8:
        return "RG8";
    case PixelFormat::I8:
        return "I8";
    case PixelFormat::A8:
        return "A8";
    case PixelFormat::IA4:
        return "IA4";
    case PixelFormat::I4:
        return "I4";
    case PixelFormat::A4:
        return "A4";
    case PixelFormat::ETC1:
        return "ETC1";
    case PixelFormat::ETC1A4:
        return "ETC1A4";
    case PixelFormat::D16:
        return "D16";
    case PixelFormat::D24:
        return "D24";
    case PixelFormat::D24S8:
        return "D24S8";
    default:
        return "NotReal";
    }
}

bool CheckFormatsBlittable(PixelFormat source_format, PixelFormat dest_format) {
    SurfaceType source_type = GetFormatType(source_format);
    SurfaceType dest_type = GetFormatType(dest_format);

    if ((source_type == SurfaceType::Color || source_type == SurfaceType::Texture) &&
        (dest_type == SurfaceType::Color || dest_type == SurfaceType::Texture)) {
        return true;
    }

    if (source_type == SurfaceType::Depth && dest_type == SurfaceType::Depth) {
        return true;
    }

    if (source_type == SurfaceType::DepthStencil && dest_type == SurfaceType::DepthStencil) {
        return true;
    }

    return false;
}

PixelFormat PixelFormatFromTextureFormat(Pica::TexturingRegs::TextureFormat format) {
    const u32 format_index = static_cast<u32>(format);
    return (format_index < 14) ? static_cast<PixelFormat>(format) : PixelFormat::Invalid;
}

PixelFormat PixelFormatFromColorFormat(Pica::FramebufferRegs::ColorFormat format) {
    const u32 format_index = static_cast<u32>(format);
    return (format_index < 5) ? static_cast<PixelFormat>(format) : PixelFormat::Invalid;
}

PixelFormat PixelFormatFromDepthFormat(Pica::FramebufferRegs::DepthFormat format) {
    const u32 format_index = static_cast<u32>(format);
    return (format_index < 4) ? static_cast<PixelFormat>(format_index + 14) : PixelFormat::Invalid;
}

PixelFormat PixelFormatFromGPUPixelFormat(GPU::Regs::PixelFormat format) {
    const u32 format_index = static_cast<u32>(format);
    switch (format) {
    // RGB565 and RGB5A1 are switched in PixelFormat compared to ColorFormat
    case GPU::Regs::PixelFormat::RGB565:
        return PixelFormat::RGB565;
    case GPU::Regs::PixelFormat::RGB5A1:
        return PixelFormat::RGB5A1;
    default:
        return (format_index < 5) ? static_cast<PixelFormat>(format) : PixelFormat::Invalid;
    }
}

} // namespace VideoCore
