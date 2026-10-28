// Controller vibration through the game's OpenVR runtime.
#pragma once

namespace vrlockpick::haptics
{
    // Buzzes a controller for this long (OpenVR takes at most 3999 per call;
    // call it every frame for longer).
    void pulse(bool left, unsigned short microseconds) noexcept;
}
