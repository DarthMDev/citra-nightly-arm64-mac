// Copyright 2022 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "jni/emu_window/emu_window.h"

struct ANativeWindow;

class EmuWindow_Android_Vulkan : public EmuWindow_Android {
public:
    EmuWindow_Android_Vulkan(ANativeWindow* surface);
    ~EmuWindow_Android_Vulkan() override = default;

    void TryPresenting() override;
    void StopPresenting() override;

    std::unique_ptr<GraphicsContext> CreateSharedContext() const override;

private:
    bool CreateWindowSurface() override;
};
