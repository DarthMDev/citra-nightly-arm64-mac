// Copyright 2019 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <cstdlib>
#include <string>
#include <android/native_window_jni.h>
#include "common/logging/log.h"
#include "common/settings.h"
#include "input_common/main.h"
#include "jni/emu_window/emu_window_vk.h"
#include "jni/id_cache.h"
#include "jni/input_manager.h"
#include "network/network.h"
#include "video_core/renderer_base.h"
#include "video_core/video_core.h"

static bool IsPortraitMode() {
    return JNI_FALSE != IDCache::GetEnvForThread()->CallStaticBooleanMethod(
                            IDCache::GetNativeLibraryClass(), IDCache::GetIsPortraitMode());
}

static void UpdateLandscapeScreenLayout() {
    Settings::values.layout_option =
        static_cast<Settings::LayoutOption>(IDCache::GetEnvForThread()->CallStaticIntMethod(
            IDCache::GetNativeLibraryClass(), IDCache::GetLandscapeScreenLayout()));
}

void EmuWindow_Android_Vulkan::OnSurfaceChanged(ANativeWindow* surface) {
    render_window = surface;
    StopPresenting();
}

bool EmuWindow_Android_Vulkan::OnTouchEvent(int x, int y, bool pressed) {
    if (pressed) {
        return TouchPressed((unsigned)std::max(x, 0), (unsigned)std::max(y, 0));
    }

    TouchReleased();
    return true;
}

void EmuWindow_Android_Vulkan::OnTouchMoved(int x, int y) {
    TouchMoved((unsigned)std::max(x, 0), (unsigned)std::max(y, 0));
}

void EmuWindow_Android_Vulkan::OnFramebufferSizeChanged() {
    UpdateLandscapeScreenLayout();
    const bool is_portrait_mode{IsPortraitMode()};
    const int bigger{window_width > window_height ? window_width : window_height};
    const int smaller{window_width < window_height ? window_width : window_height};
    if (is_portrait_mode) {
        UpdateCurrentFramebufferLayout(smaller, bigger, is_portrait_mode);
    } else {
        UpdateCurrentFramebufferLayout(bigger, smaller, is_portrait_mode);
    }
}

EmuWindow_Android_Vulkan::EmuWindow_Android_Vulkan(ANativeWindow* surface) {
    LOG_DEBUG(Frontend, "Initializing EmuWindow_Android_Vulkan");

    if (!surface) {
        LOG_CRITICAL(Frontend, "surface is nullptr");
        return;
    }

    Network::Init();

    host_window = surface;
    CreateWindowSurface();

    if (core_context = CreateSharedContext(); !core_context) {
        LOG_CRITICAL(Frontend, "CreateSharedContext() failed");
        return;
    }

    OnFramebufferSizeChanged();
}

bool EmuWindow_Android_Vulkan::CreateWindowSurface() {
    if (!host_window) {
        return true;
    }

    window_info.type = Frontend::WindowSystemType::Android;
    window_info.render_surface = host_window;

    return true;
}

void EmuWindow_Android_Vulkan::DestroyWindowSurface() {
    /*if (!egl_surface) {
        return;
    }
    if (eglGetCurrentSurface(EGL_DRAW) == egl_surface) {
        eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (!eglDestroySurface(egl_display, egl_surface)) {
        LOG_CRITICAL(Frontend, "eglDestroySurface() failed");
    }
    egl_surface = EGL_NO_SURFACE;*/
}

void EmuWindow_Android_Vulkan::DestroyContext() {
    /*if (!egl_context) {
        return;
    }
    if (eglGetCurrentContext() == egl_context) {
        eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (!eglDestroyContext(egl_display, egl_context)) {
        LOG_CRITICAL(Frontend, "eglDestroySurface() failed");
    }
    if (!eglTerminate(egl_display)) {
        LOG_CRITICAL(Frontend, "eglTerminate() failed");
    }
    egl_context = EGL_NO_CONTEXT;
    egl_display = EGL_NO_DISPLAY;*/
}

EmuWindow_Android_Vulkan::~EmuWindow_Android_Vulkan() {
    DestroyWindowSurface();
    DestroyContext();
}

std::unique_ptr<Frontend::GraphicsContext> EmuWindow_Android_Vulkan::CreateSharedContext() const {
    return std::make_unique<SharedContext_Android>();
}

void EmuWindow_Android_Vulkan::StopPresenting() {
    /*if (presenting_state == PresentingState::Running) {
        eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }*/
    presenting_state = PresentingState::Stopped;
}

void EmuWindow_Android_Vulkan::TryPresenting() {
    if (presenting_state != PresentingState::Running) {
        if (presenting_state == PresentingState::Initial) {
            /*eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);*/
            presenting_state = PresentingState::Running;
        } else {
            return;
        }
    }
    /*eglSwapInterval(egl_display, Settings::values.use_vsync_new ? 1 : 0);
    if (VideoCore::g_renderer) {
        VideoCore::g_renderer->TryPresent(0);
        eglSwapBuffers(egl_display, egl_surface);
    }*/
}

void EmuWindow_Android_Vulkan::PollEvents() {
    if (!render_window) {
        return;
    }

    host_window = render_window;
    render_window = nullptr;

    DestroyWindowSurface();
    CreateWindowSurface();
    OnFramebufferSizeChanged();
    presenting_state = PresentingState::Initial;
}

void EmuWindow_Android_Vulkan::MakeCurrent() {
    core_context->MakeCurrent();
}

void EmuWindow_Android_Vulkan::DoneCurrent() {
    core_context->DoneCurrent();
}
