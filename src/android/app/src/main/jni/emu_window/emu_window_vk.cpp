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

class SharedContext_Android : public Frontend::GraphicsContext {};

EmuWindow_Android_Vulkan::EmuWindow_Android_Vulkan(ANativeWindow* surface) : EmuWindow_Android{surface} {
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

std::unique_ptr<Frontend::GraphicsContext> EmuWindow_Android_Vulkan::CreateSharedContext() const {
    return std::make_unique<SharedContext_Android>();
}

void EmuWindow_Android_Vulkan::StopPresenting() {
    presenting_state = PresentingState::Stopped;
}

void EmuWindow_Android_Vulkan::TryPresenting() {
    if (presenting_state != PresentingState::Running) {
        if (presenting_state == PresentingState::Initial) {
            presenting_state = PresentingState::Running;
        } else {
            return;
        }
    }
}