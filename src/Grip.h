// Finger poses for holding the pin and the screwdriver (FRIK API v2).
#pragma once

#include <RE/Fallout.h>

namespace vrlockpick::grip
{
    // Shapes the fingers: the pin hand pinches, the screwdriver hand grips.
    void hold(bool left, bool pinch) noexcept;
    void release(bool left) noexcept;

    // FRIK body: a bone's world transform by name, the index fingertip, the
    // visible hand (wrist) and where the controller puts the hand.
    bool bone(const char* name, RE::NiTransform& out) noexcept;
    bool indexTip(bool left, RE::NiPoint3& out) noexcept;
    bool trackedHand(bool left, RE::NiTransform& out) noexcept;
    bool wand(bool left, RE::NiTransform& out) noexcept;

    // Holds the visible hand at this wrist transform (like a weapon grip)
    // until unglued; the controller keeps moving freely.
    bool glue(bool left, const RE::NiTransform& wrist) noexcept;
    void unglue(bool left) noexcept;
    bool glued(bool left) noexcept;
}
