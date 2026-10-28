#include "Haptics.h"

#include "Runtime.h"

#include <RE/Fallout.h>

#include "api/openvr.h"

namespace vrlockpick::haptics
{
    namespace
    {
        bool g_tried{ false };
        vr::IVRSystem* g_system{ nullptr };

        // The game has already started OpenVR: ask its openvr_api.dll for the
        // system interface instead of linking our own.
        vr::IVRSystem* system() noexcept
        {
            if (!g_tried) {
                g_tried = true;
                using GetInterface_t = void* (*)(const char*, vr::EVRInitError*);
                const auto module = REX::W32::GetModuleHandleW(L"openvr_api.dll");
                const auto get = module ? reinterpret_cast<GetInterface_t>(REX::W32::GetProcAddress(module, "VR_GetGenericInterface")) : nullptr;
                vr::EVRInitError error = vr::VRInitError_None;
                g_system = get ? static_cast<vr::IVRSystem*>(get(vr::IVRSystem_Version, &error)) : nullptr;
                vrlockpick::runtime::logNote(g_system ? "Haptics: controller vibration ready" : "Haptics: OpenVR not found; no vibration");
            }
            return g_system;
        }
    }

    void pulse(const bool left, const unsigned short microseconds) noexcept
    {
        auto* vrSystem = system();
        if (!vrSystem || microseconds == 0) {
            return;
        }
        const auto device = vrSystem->GetTrackedDeviceIndexForControllerRole(left ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);
        if (device != vr::k_unTrackedDeviceIndexInvalid) {
            vrSystem->TriggerHapticPulse(device, 0, microseconds);
        }
    }
}
