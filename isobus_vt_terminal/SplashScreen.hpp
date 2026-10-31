#pragma once

#include "imgui.h"
#include <string>

namespace isobus_vt
{
    class SplashScreen
    {
    public:
        SplashScreen();

        void start();
        void dismiss();
        bool is_active() const { return active; }

        // Render overlay over full ImGui viewport
        void render(ImFont* titleFont, ImFont* subtitleFont, ImFont* regularFont);

    private:
        bool active = true;
        bool fadingOut = false;
        float elapsed = 0.0f;
        float fadeTimer = 0.0f;

        const float totalDuration = 3.2f;    // Boot sequence animation time in seconds
        const float fadeOutDuration = 0.4f;  // Smooth fade-out duration
    };
}
