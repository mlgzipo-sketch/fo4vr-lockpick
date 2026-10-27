#include "Grip.h"

#include "Runtime.h"

#include <RE/Fallout.h>

#include "api/FRIKApiV2.h"

#include <cstdio>

namespace vrlockpick::grip
{
    namespace
    {
        using frik::api::FRIKApiV2;

        constexpr const char* kTag = "VRLockpick_Tools";
        // Above FRIK's own poses (90) and ROCK's grab and contact claims (100).
        constexpr int kPriority = 120;

        bool g_tried{ false };
        bool g_holding[2]{};

        const FRIKApiV2* api() noexcept
        {
            if (!g_tried) {
                g_tried = true;
                const int error = FRIKApiV2::initialize(1);
                char message[160]{};
                if (error == 0) {
                    std::snprintf(message, sizeof(message), "Grip: FRIK %s, API v2.%u; fingers hold the tools",
                        FRIKApiV2::inst->getModVersion(), FRIKApiV2::inst->getVersion());
                } else {
                    std::snprintf(message, sizeof(message), "Grip: FRIK API v2 unavailable (error %d); fingers stay as they are", error);
                }
                vrlockpick::runtime::logNote(message);
            }
            return FRIKApiV2::inst;
        }

        // FRIK's finger values run from 0 (closed) to 1 (open).
        FRIKApiV2::HandPoseData poseFor(const bool pinch) noexcept
        {
            FRIKApiV2::HandPoseData pose{};
            if (pinch) {
                // Bobby pin between thumb and index; the other fingers curled away.
                pose.thumb = { 0.45f, 0.5f, 0.55f, 0.0f };
                pose.index = { 0.5f, 0.45f, 0.5f, 0.0f };
                pose.middle = { 0.25f, 0.2f, 0.3f, 0.0f };
                pose.ring = { 0.2f, 0.15f, 0.25f, 0.0f };
                pose.pinky = { 0.15f, 0.15f, 0.2f, 0.0f };
            } else {
                // Screwdriver handle in a fist.
                pose.thumb = { 0.35f, 0.35f, 0.4f, 0.0f };
                pose.index = { 0.25f, 0.25f, 0.3f, 0.0f };
                pose.middle = { 0.2f, 0.2f, 0.25f, 0.0f };
                pose.ring = { 0.2f, 0.2f, 0.25f, 0.0f };
                pose.pinky = { 0.2f, 0.2f, 0.25f, 0.0f };
            }
            return pose;
        }

        FRIKApiV2::Hand frikHand(const bool left) noexcept
        {
            return left ? FRIKApiV2::Hand::Left : FRIKApiV2::Hand::Right;
        }
    }

    void hold(const bool left, const bool pinch) noexcept
    {
        const int index = left ? 1 : 0;
        if (g_holding[index]) {
            return;
        }
        auto* frik = api();
        if (!frik || !frik->isSkeletonReady()) {
            return;
        }
        g_holding[index] = frik->setHandPoseCustom(kTag, frikHand(left), poseFor(pinch), kPriority);
        const auto state = frik->getHandPoseSetTagState(kTag, frikHand(left));
        char message[160]{};
        std::snprintf(message, sizeof(message), "Grip: %s fingers %s (%s)", left ? "left" : "right", pinch ? "pinch the pin" : "grip the screwdriver",
            !g_holding[index] ? "refused" : state == FRIKApiV2::HandPoseTagState::Active ? "active" : "overridden by another mod");
        vrlockpick::runtime::logInfo(message);
    }

    void release(const bool left) noexcept
    {
        const int index = left ? 1 : 0;
        if (!g_holding[index]) {
            return;
        }
        g_holding[index] = false;
        if (auto* frik = api()) {
            (void)frik->clearHandPose(kTag, frikHand(left));
        }
    }
}
