// Finger poses for holding the pin and the screwdriver (FRIK API v2).
#pragma once

namespace vrlockpick::grip
{
    // Shapes the fingers: the pin hand pinches, the screwdriver hand grips.
    void hold(bool left, bool pinch) noexcept;
    void release(bool left) noexcept;
}
