// Copyright 2022 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <glad/glad.h>
#include "common/assert.h"
#include "core/core.h"
#include "video_core/renderer_opengl/gl_driver.h"

namespace OpenGL {

DECLARE_ENUM_FLAG_OPERATORS(DriverBug);

inline std::string_view GetSource(GLenum source) {
#define RET(s)                                                                                     \
    case GL_DEBUG_SOURCE_##s:                                                                      \
        return #s
    switch (source) {
        RET(API);
        RET(WINDOW_SYSTEM);
        RET(SHADER_COMPILER);
        RET(THIRD_PARTY);
        RET(APPLICATION);
        RET(OTHER);
    default:
        UNREACHABLE();
    }
#undef RET

    return std::string_view{};
}

inline std::string_view GetType(GLenum type) {
#define RET(t)                                                                                     \
    case GL_DEBUG_TYPE_##t:                                                                        \
        return #t
    switch (type) {
        RET(ERROR);
        RET(DEPRECATED_BEHAVIOR);
        RET(UNDEFINED_BEHAVIOR);
        RET(PORTABILITY);
        RET(PERFORMANCE);
        RET(OTHER);
        RET(MARKER);
    default:
        UNREACHABLE();
    }
#undef RET

    return std::string_view{};
}

static void APIENTRY DebugHandler(GLenum source, GLenum type, GLuint id, GLenum severity,
                                  GLsizei length, const GLchar* message, const void* user_param) {
    Log::Level level = Log::Level::Info;
    switch (severity) {
    case GL_DEBUG_SEVERITY_HIGH:
        level = Log::Level::Critical;
        break;
    case GL_DEBUG_SEVERITY_MEDIUM:
        level = Log::Level::Warning;
        break;
    case GL_DEBUG_SEVERITY_NOTIFICATION:
    case GL_DEBUG_SEVERITY_LOW:
        level = Log::Level::Debug;
        break;
    }
    LOG_GENERIC(Log::Class::Render_OpenGL, level, "{} {} {}: {}", GetSource(source), GetType(type),
                id, message);
}

Driver::Driver(bool gles, bool enable_debug) : is_gles{gles} {
#ifndef ANDROID
    if (!gladLoadGL()) {
        return;
    }

    /*
     * Qualcomm has some spammy info messages that are marked as errors but not important
     * https://developer.qualcomm.com/comment/11845
     */
    if (!gles) {
        glEnable(GL_DEBUG_OUTPUT);
        glDebugMessageCallback(DebugHandler, nullptr);
    }
#endif

    ReportDriverInfo();
    DeduceVendor();
    CheckExtensionSupport();
    FindBugs();
}

bool Driver::HasBug(DriverBug bug) const {
    return True(bugs & bug);
}

void Driver::ReportDriverInfo() {
    // Report the context version and the vendor string
    gl_version = std::string_view{reinterpret_cast<const char*>(glGetString(GL_VERSION))};
    gpu_vendor = std::string_view{reinterpret_cast<const char*>(glGetString(GL_VENDOR))};
    gpu_model = std::string_view{reinterpret_cast<const char*>(glGetString(GL_RENDERER))};

    LOG_INFO(Render_OpenGL, "GL_VERSION: {}", gl_version);
    LOG_INFO(Render_OpenGL, "GL_VENDOR: {}", gpu_vendor);
    LOG_INFO(Render_OpenGL, "GL_RENDERER: {}", gpu_model);

    // Add the information to the telemetry system
    auto& telemetry_session = Core::System::GetInstance().TelemetrySession();
    constexpr auto user_system = Common::Telemetry::FieldType::UserSystem;
    telemetry_session.AddField(user_system, "GPU_Vendor", std::string{gpu_vendor});
    telemetry_session.AddField(user_system, "GPU_Model", std::string{gpu_model});
    telemetry_session.AddField(user_system, "GPU_OpenGL_Version", std::string{gl_version});
}

void Driver::DeduceVendor() {
    if (gpu_vendor.find("NVIDIA") != gpu_vendor.npos) {
        vendor = Vendor::Nvidia;
    } else if ((gpu_vendor.find("ATI") != gpu_vendor.npos) ||
               (gpu_vendor.find("AMD") != gpu_vendor.npos) ||
               (gpu_vendor.find("Advanced Micro Devices") != gpu_vendor.npos)) {
        vendor = Vendor::AMD;
    } else if (gpu_vendor.find("Intel") != gpu_vendor.npos) {
        vendor = Vendor::Intel;
    } else if (gpu_vendor.find("ARM") != gpu_vendor.npos) {
        vendor = Vendor::ARM;
    } else if (gpu_vendor.find("GDI Generic") != gpu_vendor.npos) {
            vendor = Vendor::Generic;
    }
}

void Driver::CheckExtensionSupport() {
    ext_buffer_storage = GLAD_GL_EXT_buffer_storage;
    arb_buffer_storage = GLAD_GL_ARB_buffer_storage;
    ext_clip_cull_distance = GLAD_GL_EXT_clip_cull_distance;
    arb_direct_state_access = GLAD_GL_ARB_direct_state_access;
}

void Driver::FindBugs() {
#ifdef __unix__
    const bool is_linux = true;
#else
    const bool is_linux = false;
#endif

    // TODO: Check if these have been fixed in the newer driver
    if (vendor == Vendor::AMD) {
        bugs |= DriverBug::ShaderStageChangeFreeze | DriverBug::VertexArrayOutOfBound;
    }

    if (vendor == Vendor::AMD || (vendor == Vendor::Intel && !is_linux)) {
        bugs |= DriverBug::BrokenTextureView;
    }
}

} // namespace OpenGL
