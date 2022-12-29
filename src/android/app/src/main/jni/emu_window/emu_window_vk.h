// Copyright 2022 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <vector>
#include "core/frontend/emu_window.h"

struct ANativeWindow;

class SharedContext_Android : public Frontend::GraphicsContext {};

class EmuWindow_Android_Vulkan : public Frontend::EmuWindow {
public:
    EmuWindow_Android_Vulkan(ANativeWindow* surface);
    ~EmuWindow_Android_Vulkan();

    void Present();

    /// Called by the onSurfaceChanges() method to change the surface
    void OnSurfaceChanged(ANativeWindow* surface);

    /// Handles touch event that occur.(Touched or released)
    bool OnTouchEvent(int x, int y, bool pressed);

    /// Handles movement of touch pointer
    void OnTouchMoved(int x, int y);

    void PollEvents() override;
    void MakeCurrent() override;
    void DoneCurrent() override;

    void TryPresenting();
    void StopPresenting();

    std::unique_ptr<GraphicsContext> CreateSharedContext() const override;

private:
    void OnFramebufferSizeChanged();
    bool CreateWindowSurface();
    void DestroyWindowSurface();
    void DestroyContext();

    ANativeWindow* render_window{};
    ANativeWindow* host_window{};

    int window_width{1080};
    int window_height{2220};

    std::unique_ptr<Frontend::GraphicsContext> core_context;

    enum class PresentingState {
        Initial,
        Running,
        Stopped,
    };
    PresentingState presenting_state{};
};
