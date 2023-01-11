// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "common/common_types.h"
#include "common/vector_math.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/video_core.h"

namespace Frontend {
class EmuWindow;
}

class RendererBase : NonCopyable {
public:
    explicit RendererBase(Frontend::EmuWindow& window, Frontend::EmuWindow* secondary_window);
    virtual ~RendererBase();

    /// Initialize the renderer
    virtual VideoCore::ResultStatus Init() = 0;

    /// Returns the rasterizer owned by the renderer
    virtual VideoCore::RasterizerInterface* Rasterizer() = 0;

    /// Shutdown the renderer
    virtual void ShutDown() = 0;

    /// Finalize rendering the guest frame and draw into the presentation texture
    virtual void SwapBuffers() = 0;

    /// Draws the latest frame to the window waiting timeout_ms for a frame to arrive (Renderer
    /// specific implementation)
    virtual void TryPresent(int timeout_ms, bool is_secondary) = 0;
    virtual void TryPresent(int timeout_ms) {
        TryPresent(timeout_ms, false);
    }

    /// This is called to notify the rendering backend of a surface change
    virtual void NotifySurfaceChanged() {}

    /// Prepares for video dumping (e.g. create necessary buffers, etc)
    virtual void PrepareVideoDumping() = 0;

    /// Cleans up after video dumping is ended
    virtual void CleanupVideoDumping() = 0;

    virtual void Sync() = 0;

    /// Updates the framebuffer layout of the contained render window handle.
    void UpdateCurrentFramebufferLayout(bool is_portrait_mode = {});

    // Getter/setter functions:
    // ------------------------

    f32 GetCurrentFPS() const {
        return m_current_fps;
    }

    int GetCurrentFrame() const {
        return m_current_frame;
    }

    Frontend::EmuWindow& GetRenderWindow() {
        return render_window;
    }

    const Frontend::EmuWindow& GetRenderWindow() const {
        return render_window;
    }

protected:
    Frontend::EmuWindow& render_window;    ///< Reference to the render window handle.
    Frontend::EmuWindow* secondary_window; ///< Reference to the secondary render window handle.
    f32 m_current_fps = 0.0f;              ///< Current framerate, should be set by the renderer
    int m_current_frame = 0;               ///< Current frame, should be set by the renderer
};
