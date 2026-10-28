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
            // From the user's 3D reference (bend per joint, 90 degrees = closed).
            if (pinch) {
                // Bobby pin pinched by thumb and index; the others curled.
                pose.thumb = { 1.0f, 1.0f, 0.82f, 0.0f };
                pose.index = { 0.87f, 0.41f, 0.87f, 0.0f };
                pose.middle = { 0.68f, 0.65f, 0.65f, 0.0f };
                pose.ring = { 0.51f, 0.36f, 0.35f, 0.0f };
                pose.pinky = { 0.55f, 0.36f, 0.35f, 0.0f };
            } else {
                // Screwdriver handle in the palm, thumb along it.
                pose.thumb = { 1.0f, 1.0f, 1.0f, 0.0f };
                pose.index = { 1.0f, 0.59f, 0.86f, 0.0f };
                pose.middle = { 0.63f, 0.3f, 0.3f, 0.0f };
                pose.ring = { 0.65f, 0.31f, 0.54f, 0.0f };
                pose.pinky = { 0.86f, 0.45f, 0.45f, 0.0f };
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

    namespace
    {
        bool g_glued[2]{};
        bool g_glueLogged{ false };

        const FRIKApiV2* body() noexcept
        {
            auto* frik = api();
            return frik && frik->getVersion() >= 3 && frik->isSkeletonReady() ? frik : nullptr;
        }
    }

    bool bone(const char* name, RE::NiTransform& out) noexcept
    {
        auto* frik = body();
        return frik && frik->getBoneWorldTransform(name, &out);
    }

    bool indexTip(const bool left, RE::NiPoint3& out) noexcept
    {
        auto* frik = body();
        if (!frik) {
            return false;
        }
        out = frik->getIndexFingerTipPosition(frikHand(left));
        return true;
    }

    bool trackedHand(const bool left, RE::NiTransform& out) noexcept
    {
        auto* frik = body();
        return frik && frik->getTrackedHandTransform(frikHand(left), FRIKApiV2::TrackedHandKind::FirstPersonHand, &out);
    }

    bool wand(const bool left, RE::NiTransform& out) noexcept
    {
        auto* frik = body();
        return frik && frik->getTrackedHandTransform(frikHand(left), FRIKApiV2::TrackedHandKind::Wand, &out);
    }

    bool glue(const bool left, const RE::NiTransform& wrist) noexcept
    {
        auto* frik = body();
        if (!frik) {
            return false;
        }
        const bool set = frik->setHandWorldTransform(kTag, frikHand(left), wrist, kPriority);
        g_glued[left ? 1 : 0] = set;
        if (!g_glueLogged) {
            g_glueLogged = true;
            vrlockpick::runtime::logInfo(set ? "Grip: hand held on the tool" : "Grip: FRIK refused to hold the hand on the tool");
        }
        return set;
    }

    void unglue(const bool left) noexcept
    {
        const int index = left ? 1 : 0;
        if (!g_glued[index]) {
            return;
        }
        g_glued[index] = false;
        if (auto* frik = api()) {
            (void)frik->clearHandWorldTransform(kTag, frikHand(left));
        }
    }

    bool glued(const bool left) noexcept
    {
        return g_glued[left ? 1 : 0];
    }
}
