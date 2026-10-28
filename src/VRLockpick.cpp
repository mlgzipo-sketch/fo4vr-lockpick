// VRLockpick: pick locks with your own hands in Fallout 4 VR.
// The bobby pin follows the roll of one hand and the lock turns when the
// other hand rolls like a screwdriver; the game's own rules do the rest.
#include <ROCK/Hands.h>
#include <ROCK/Input.h>
#include "Grip.h"
#include "Haptics.h"
#include "Runtime.h"

#include <RE/Fallout.h>
#include <REL/Relocation.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <iterator>

namespace
{
    namespace hands = rock::api::hands;
    namespace input = rock::api::input;
    using Clock = std::chrono::steady_clock;

    constexpr float kPi = 3.14159265f;

    struct Settings
    {
        bool drivePick{ true };
        bool leftHandPick{ true };
        float pickDegreesPerHandDegree{ 1.0f };
        float maxPickAngle{ 90.0f };
        bool turnWithHand{ true };
        float turnStartDegrees{ 25.0f };
        float turnStopDegrees{ 15.0f };
        float pinModelSign{ 1.0f };
        bool setPinModel{ true };
        // Hand roll at which the lock turns at full speed (like the stick
        // pushed all the way); less roll turns it slower.
        float turnFullDegrees{ 60.0f };
        // 1 = move the hands around the keyhole (pin like a clock hand,
        // screwdriver like a lever), 0 = roll the wrists.
        bool armControl{ true };
        bool gripFingers{ true };
        float armPickSign{ 1.0f };
        float armTurnStartDegrees{ 10.0f };
        float armTurnStopDegrees{ 5.0f };
        float armTurnFullDegrees{ 35.0f };
        // Arm control: the pin moves only while the trigger is held (held
        // in the fingers); the screwdriver turns like a bolt, the lock
        // following the twist (a quarter turn opens it).
        bool pinNeedsTrigger{ false };
        // Arm control: the docked pin points at the pinching fingers, like a
        // clock hand whose tip stays in the keyhole (0 = it moves by as much
        // as the hand moves around the keyhole, from where it was).
        bool pinPointsAtHand{ true };
        // The docked screwdriver's handle follows the hand around the
        // keyhole: down (clockwise as you look at the lock) turns the lock.
        bool screwPointsAtHand{ true };
        float screwSign{ 1.0f };
        // The visible hands stay on the docked tools, like on a weapon,
        // until the controller is this far (game units) from them.
        // The pin hand's controller buzzes when the pin reaches the spot that
        // opens the lock, and softly while it stays there (0 = off).
        bool sweetSpotBuzz{ true };
        float sweetSpotHoldBuzz{ 0.3f };
        bool glueHands{ true };
        float glueMaxDistance{ 45.0f };
        // The tools start in the hands; bringing a tool's tip this close to
        // the keyhole puts it in place.
        bool toolsInHands{ true };
        float dockDistance{ 6.0f };
        // 0 = the screwdriver hand swings around the keyhole (as in the
        // user's 3D reference), 1 = it twists in place like a bolt.
        bool screwTwist{ false };
        float screwLockDegreesPerTwist{ 1.0f };
    };
    Settings g_settings;

    const hands::ApiV1* g_hands{};
    const input::ApiV1* g_input{};
    std::uint64_t g_ownerToken{ 0 };
    std::uint64_t g_rockFrames{ 0 };

    struct Vec3
    {
        float x{}, y{}, z{};
    };
    float dot(const Vec3& a, const Vec3& b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
    Vec3 cross(const Vec3& a, const Vec3& b) noexcept
    {
        return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
    }

    // ROCK hand frame: rotate is row-major and world = R^T * v, so row 0 is
    // the hand's +X (along the fingers) in world space and row 1 its +Y.
    struct HandPose
    {
        Vec3 position;
        Vec3 alongFingers;
        Vec3 side;
    };

    bool readHand(const bool left, HandPose& out) noexcept
    {
        if (!g_hands || g_ownerToken == 0) {
            return false;
        }
        hands::HandFrameV1 frame{};
        const auto hand = left ? rock::api::Hand::Left : rock::api::Hand::Right;
        if (g_hands->getHandFrameV1(g_ownerToken, hand, &frame) != rock::api::Status::Ok ||
            (frame.flags & static_cast<std::uint32_t>(hands::HandFrameFlagV1::Valid)) == 0) {
            return false;
        }
        const auto& r = frame.transform.rotate;
        out.position = { frame.transform.translate[0], frame.transform.translate[1], frame.transform.translate[2] };
        out.alongFingers = { r[0], r[1], r[2] };
        out.side = { r[3], r[4], r[5] };
        return true;
    }

    // How far the hand has rolled around its finger axis since the reference
    // pose, in degrees (twist part of the rotation).
    float rollDegrees(const HandPose& reference, const HandPose& now) noexcept
    {
        const Vec3& axis = now.alongFingers;
        const float along = dot(reference.side, axis);
        const Vec3 projected{ reference.side.x - axis.x * along, reference.side.y - axis.y * along, reference.side.z - axis.z * along };
        const float sine = dot(axis, cross(projected, now.side));
        const float cosine = dot(projected, now.side);
        return std::atan2(sine, cosine) * 180.0f / kPi;
    }

    void readSettings() noexcept
    {
        try {
            std::ifstream file("Data/F4SE/Plugins/VRLockpick.ini");
            std::string line;
            while (file && std::getline(file, line)) {
                const auto equals = line.find('=');
                if (line.empty() || line[0] == ';' || line[0] == '[' || equals == std::string::npos) {
                    continue;
                }
                auto key = line.substr(0, equals);
                while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) {
                    key.pop_back();
                }
                const float value = std::strtof(line.c_str() + equals + 1, nullptr);
                if (key == "bDrivePick") {
                    g_settings.drivePick = value != 0.0f;
                } else if (key == "bLeftHandPick") {
                    g_settings.leftHandPick = value != 0.0f;
                } else if (key == "fPickDegreesPerHandDegree") {
                    g_settings.pickDegreesPerHandDegree = value;
                } else if (key == "fMaxPickAngle") {
                    g_settings.maxPickAngle = value;
                } else if (key == "bTurnWithHand") {
                    g_settings.turnWithHand = value != 0.0f;
                } else if (key == "fTurnStartDegrees") {
                    g_settings.turnStartDegrees = value;
                } else if (key == "bSetPinModel") {
                    g_settings.setPinModel = value != 0.0f;
                } else if (key == "bGripFingers") {
                    g_settings.gripFingers = value != 0.0f;
                } else if (key == "bToolsInHands") {
                    g_settings.toolsInHands = value != 0.0f;
                } else if (key == "fDockDistance") {
                    g_settings.dockDistance = value;
                } else if (key == "bScrewTwist") {
                    g_settings.screwTwist = value != 0.0f;
                } else if (key == "bScrewPointsAtHand") {
                    g_settings.screwPointsAtHand = value != 0.0f;
                } else if (key == "fScrewSign") {
                    g_settings.screwSign = value;
                } else if (key == "bSweetSpotBuzz") {
                    g_settings.sweetSpotBuzz = value != 0.0f;
                } else if (key == "fSweetSpotHoldBuzz") {
                    g_settings.sweetSpotHoldBuzz = value;
                } else if (key == "bGlueHands") {
                    g_settings.glueHands = value != 0.0f;
                } else if (key == "fGlueMaxDistance") {
                    g_settings.glueMaxDistance = value;
                } else if (key == "bPinPointsAtHand") {
                    g_settings.pinPointsAtHand = value != 0.0f;
                } else if (key == "bPinNeedsTrigger") {
                    g_settings.pinNeedsTrigger = value != 0.0f;
                } else if (key == "fScrewLockDegreesPerTwist") {
                    g_settings.screwLockDegreesPerTwist = value;
                } else if (key == "bArmControl") {
                    g_settings.armControl = value != 0.0f;
                } else if (key == "fArmPickSign") {
                    g_settings.armPickSign = value;
                } else if (key == "fArmTurnStartDegrees") {
                    g_settings.armTurnStartDegrees = value;
                } else if (key == "fArmTurnStopDegrees") {
                    g_settings.armTurnStopDegrees = value;
                } else if (key == "fArmTurnFullDegrees") {
                    g_settings.armTurnFullDegrees = value;
                } else if (key == "fTurnFullDegrees") {
                    g_settings.turnFullDegrees = value;
                } else if (key == "fPinModelSign") {
                    g_settings.pinModelSign = value;
                } else if (key == "fTurnStopDegrees") {
                    g_settings.turnStopDegrees = value;
                }
            }
        } catch (...) {
        }
        char message[400]{};
        std::snprintf(message, sizeof(message), "Settings: bArmControl=%d fArmPickSign=%.0f fArmTurn=%.0f/%.0f/%.0f bDrivePick=%d bLeftHandPick=%d fPickDegreesPerHandDegree=%.2f fMaxPickAngle=%.1f bTurnWithHand=%d fTurnStartDegrees=%.1f fTurnStopDegrees=%.1f fTurnFullDegrees=%.1f",
            g_settings.armControl ? 1 : 0, g_settings.armPickSign, g_settings.armTurnStartDegrees, g_settings.armTurnStopDegrees,
            g_settings.armTurnFullDegrees, g_settings.drivePick ? 1 : 0, g_settings.leftHandPick ? 1 : 0, g_settings.pickDegreesPerHandDegree, g_settings.maxPickAngle,
            g_settings.turnWithHand ? 1 : 0, g_settings.turnStartDegrees, g_settings.turnStopDegrees, g_settings.turnFullDegrees);
        vrlockpick::runtime::logNote(message);
    }

    // State of one lockpicking session (menu open to close).
    struct Session
    {
        bool active{ false };
        bool haveReference{ false };
        HandPose reference;
        bool haveWrenchReference{ false };
        HandPose wrenchReference;
        bool haveHand{ false };
        bool haveWrench{ false };
        float handRoll{ 0.0f };
        float wrenchRoll{ 0.0f };
        // Arm control: where each hand is around the keyhole, in degrees
        // from where it was when the menu opened.
        bool haveOrbitReference{ false };
        Vec3 orbitReference;
        bool haveWrenchOrbitReference{ false };
        Vec3 wrenchOrbitReference;
        float handOrbit{ 0.0f };
        float wrenchOrbit{ 0.0f };
        float handDistance{ 0.0f };
        float wrenchDistance{ 0.0f };
        // Tools in the hands until brought to the keyhole.
        bool toolsPrepared{ false };
        bool pinDocked{ true };
        bool screwDocked{ true };
        // Pin held by the trigger: angles where it was taken.
        bool pinGrabbed{ false };
        float grabOrbit{ 0.0f };
        float grabPickAngle{ 0.0f };
        float armPickAngle{ 0.0f };
        float pinchDistance{ 0.0f };
        float screwAngle{ 0.0f };
        // Where the hand was around the keyhole when the screwdriver went in:
        // the turn counts from there, not from the screwdriver's rest.
        bool haveScrewZero{ false };
        float screwZero{ 0.0f };
        bool loggedFingers{ false };
        bool inSweetSpot{ false };
        int buzzFrames{ 0 };
        // Screwdriver twist around the keyhole axis.
        bool haveTwistReference{ false };
        bool twistUsesFingers{ false };
        Vec3 twistReference;
        float wrenchTwist{ 0.0f };
        bool handTurning{ false };
        bool pinHeld{ false };
        float heldPickAngle{ 0.0f };
        float gamePinAngle{ 0.0f };
        bool haveBasePin{ false };
        RE::NiPoint3 basePinPos;
        bool pinWritten{ false };
        float writtenPinAngle{ 0.0f };
        RE::NiPoint3 writtenPinPos;
        std::uint64_t menuFrames{ 0 };
        std::uint64_t rockFramesAtOpen{ 0 };
        Clock::time_point lastLog{};
        Clock::time_point lastNodeLog{};
        float lastPickAngle{ 0.0f };
        float lastLockAngle{ 0.0f };
        bool lastTurning{ false };
        std::uint32_t lastPicksBroken{ 0 };
    };
    Session g_session;

    void logPoint(const char* label, const RE::NiAVObject* object) noexcept
    {
        char message[256]{};
        if (!object) {
            std::snprintf(message, sizeof(message), "  %s: none", label);
        } else {
            const auto& t = object->world.translate;
            std::snprintf(message, sizeof(message), "  %s: world (%.1f, %.1f, %.1f) scale %.3f", label, t.x, t.y, t.z, object->world.scale);
        }
        vrlockpick::runtime::logNote(message);
    }

    void logMenuState(const char* why, const RE::LockpickingMenu& menu) noexcept
    {
        char message[320]{};
        std::snprintf(message, sizeof(message),
            "%s: pickAngle=%.2f lockAngle=%.2f sweetSpot=%.2f len=%.2f partial=%.2f damageAngle=%.2f turning=%d animating=%d picksBroken=%u modelsInit=%d",
            why, menu.pickAngle, menu.lockAngle, menu.sweetSpotCenter, menu.sweetSpotLength, menu.partialPickLength,
            menu.lockDamagePickAngle, menu.turningLock ? 1 : 0, menu.animating ? 1 : 0, menu.picksBroken, menu.modelsInit ? 1 : 0);
        vrlockpick::runtime::logNote(message);
    }

    // The lock and pin models hang under VRPlayerNodes::lockPickParentNode.
    // Which node turns with the pin is not known yet, so every node there is
    // listed when the menu opens and logged whenever its rotation changes.
    struct TrackedNode
    {
        RE::NiAVObject* object{ nullptr };
        int depth{ 0 };
        float euler[3]{};
    };
    std::vector<TrackedNode> g_nodes;
    // The node the game turns when the stick moves the pin (found in test 3).
    RE::NiAVObject* g_pinNode{ nullptr };
    RE::NiAVObject* g_screwNode{ nullptr };

    // NiMatrix3 rows are NiPoint4 (x, y, z, w).
    float& cell(RE::NiMatrix3& m, const int row, const int column) noexcept
    {
        return reinterpret_cast<float*>(&m)[row * 4 + column];
    }

    // LockPick01 turns about its own Y axis.
    float pinNodeDegrees(RE::NiAVObject* node) noexcept
    {
        auto& m = node->local.rotate;
        return std::atan2(cell(m, 0, 2), cell(m, 0, 0)) * 180.0f / kPi;
    }

    void setPinNodeDegrees(RE::NiAVObject* node, const float degrees) noexcept
    {
        const float r = degrees * kPi / 180.0f;
        const float c = std::cos(r);
        const float sn = std::sin(r);
        auto& m = node->local.rotate;
        cell(m, 0, 0) = c;   cell(m, 0, 1) = 0.0f; cell(m, 0, 2) = sn;
        cell(m, 1, 0) = 0.0f; cell(m, 1, 1) = 1.0f; cell(m, 1, 2) = 0.0f;
        cell(m, 2, 0) = -sn; cell(m, 2, 1) = 0.0f; cell(m, 2, 2) = c;
    }

    // The menu's floats, to find where VR keeps the pin angle the stick moves.
    constexpr std::size_t kDumpBegin = 0xE0;
    constexpr std::size_t kDumpEnd = 0x1C0;
    float g_menuFloats[(kDumpEnd - kDumpBegin) / 4]{};
    bool g_haveMenuFloats{ false };

    void logChangedMenuFloats(const RE::LockpickingMenu& menu) noexcept
    {
        const auto* base = reinterpret_cast<const std::uint8_t*>(&menu);
        std::string line;
        for (std::size_t offset = kDumpBegin; offset < kDumpEnd; offset += 4) {
            float value{};
            std::memcpy(&value, base + offset, sizeof(value));
            auto& previous = g_menuFloats[(offset - kDumpBegin) / 4];
            if (g_haveMenuFloats && std::isfinite(value) && std::fabs(value) < 100000.0f && std::fabs(value - previous) > 0.5f) {
                char item[48]{};
                std::snprintf(item, sizeof(item), " %03zX=%.1f", offset, value);
                line += item;
            }
            previous = value;
        }
        g_haveMenuFloats = true;
        if (!line.empty()) {
            vrlockpick::runtime::logInfo("    menu floats changed:" + line);
        }
    }

    void eulerDegrees(const RE::NiAVObject* object, float out[3]) noexcept
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
        object->local.rotate.ToEulerAnglesXYZ(x, y, z);
        out[0] = x * 180.0f / kPi;
        out[1] = y * 180.0f / kPi;
        out[2] = z * 180.0f / kPi;
    }

    void collectNodes(RE::NiAVObject* object, const int depth) noexcept
    {
        if (!object || depth > 8 || g_nodes.size() >= 64) {
            return;
        }
        TrackedNode tracked{ object, depth };
        if (object->GetName() == "LockPick01") {
            g_pinNode = object;
        } else if (object->GetName() == "screwdriver") {
            g_screwNode = object;
        }
        eulerDegrees(object, tracked.euler);
        g_nodes.push_back(tracked);
        const auto name = object->GetName();
        const auto& t = object->local.translate;
        char message[256]{};
        std::snprintf(message, sizeof(message), "  node %*s%.*s: local (%.1f, %.1f, %.1f) rot (%.1f, %.1f, %.1f) scale %.2f%s",
            depth * 2, "", static_cast<int>(name.size()), name.data(), t.x, t.y, t.z,
            tracked.euler[0], tracked.euler[1], tracked.euler[2], object->local.scale, object->IsNode() ? "" : " (geometry)");
        vrlockpick::runtime::logNote(message);
        if (auto* node = object->IsNode()) {
            auto& children = node->GetRuntimeData().children;
            for (std::uint16_t index = 0; index < children.size(); ++index) {
                collectNodes(children[index].get(), depth + 1);
            }
        }
    }

    void logTurnedNodes() noexcept
    {
        for (auto& tracked : g_nodes) {
            float now[3]{};
            eulerDegrees(tracked.object, now);
            if (std::fabs(now[0] - tracked.euler[0]) + std::fabs(now[1] - tracked.euler[1]) + std::fabs(now[2] - tracked.euler[2]) > 1.0f) {
                const auto name = tracked.object->GetName();
                char message[200]{};
                std::snprintf(message, sizeof(message), "    turned: %.*s rot (%.1f, %.1f, %.1f)",
                    static_cast<int>(name.size()), name.data(), now[0], now[1], now[2]);
                vrlockpick::runtime::logInfo(message);
                std::copy(std::begin(now), std::end(now), std::begin(tracked.euler));
            }
        }
    }

    // Where the game puts LockPick01 for a pin angle when the stick turns it,
    // relative to its rest position (logged in test 6). It is not a turn
    // about one point, so it is looked up and interpolated.
    struct PinOffset
    {
        float angle, x, z;
    };
    constexpr PinOffset kPinOffsets[]{
        { -90.0f, -1.900f, 1.500f },
        { -79.5f, -1.835f, 1.193f },
        { -70.0f, -1.729f, 0.930f },
        { -59.7f, -1.568f, 0.671f },
        { -50.1f, -1.375f, 0.456f },
        { -41.6f, -1.178f, 0.295f },
        { -34.2f, -0.989f, 0.181f },
        { -16.7f, -0.497f, 0.014f },
        { -10.8f, -0.323f, -0.007f },
        { 0.0f, 0.0f, 0.0f },
        { 8.2f, 0.240f, 0.046f },
        { 16.0f, 0.459f, 0.120f },
        { 53.2f, 1.280f, 0.841f },
        { 60.1f, 1.547f, 1.013f },
        { 81.9f, 1.511f, 1.658f },
        { 90.0f, 1.500f, 1.900f },
    };

    PinOffset pinOffset(const float angle) noexcept
    {
        const auto count = std::size(kPinOffsets);
        if (angle <= kPinOffsets[0].angle) {
            return kPinOffsets[0];
        }
        for (std::size_t i = 1; i < count; ++i) {
            const auto& a = kPinOffsets[i - 1];
            const auto& b = kPinOffsets[i];
            if (angle <= b.angle) {
                const float t = (angle - a.angle) / (b.angle - a.angle);
                return { angle, a.x + (b.x - a.x) * t, a.z + (b.z - a.z) * t };
            }
        }
        return kPinOffsets[count - 1];
    }

    float clampPick(const float angle) noexcept
    {
        if (angle > g_settings.maxPickAngle) {
            return g_settings.maxPickAngle;
        }
        if (angle < -g_settings.maxPickAngle) {
            return -g_settings.maxPickAngle;
        }
        return angle;
    }

    // The game's own lock turn (VR, found in the code saved in test 8): the
    // menu's thumbstick handler calls it with the turn stick's x every
    // frame; x != 0 starts the turn (damage angle, sound) and turns the lock
    // by |x|, x == 0 lets go. Called the same way for the screwdriver hand.
    // Only used if the code there is exactly what was analysed.
    using TurnLock_t = void (*)(RE::LockpickingMenu*, float);
    constexpr std::uintptr_t kTurnLockOffset = 0x131DF10;
    constexpr std::uint8_t kTurnLockBytes[]{ 0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x0F, 0x29, 0x74, 0x24, 0x30,
        0x0F, 0x29, 0x7C, 0x24, 0x20, 0x0F, 0x57, 0xFF, 0x48, 0x8B, 0xD9, 0x0F, 0x28 };
    TurnLock_t g_turnLock{ nullptr };

    void findTurnLock() noexcept
    {
        const auto address = REL::Module::get().base() + kTurnLockOffset;
        if (std::memcmp(reinterpret_cast<const void*>(address), kTurnLockBytes, sizeof(kTurnLockBytes)) == 0) {
            g_turnLock = reinterpret_cast<TurnLock_t>(address);
            vrlockpick::runtime::logNote("Found the game's lock turn function");
        } else {
            vrlockpick::runtime::logWarning("The game's lock turn function is not where expected (other game version?); the hand can't turn the lock");
        }
    }

    void openSession(RE::LockpickingMenu& menu) noexcept
    {
        auto& s = g_session;
        s = Session{};
        s.active = true;
        s.rockFramesAtOpen = g_rockFrames;
        s.lastLog = Clock::now();
        logMenuState("Lockpicking opened", menu);
        logPoint("lockRoot", menu.lockRoot.get());
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            logPoint("lockPickParentNode", player->vrNodes.lockPickParentNode);
            g_nodes.clear();
            g_pinNode = nullptr;
            g_screwNode = nullptr;
            g_haveMenuFloats = false;
            collectNodes(player->vrNodes.lockPickParentNode, 0);
            logPoint("primaryWand", player->vrNodes.primaryWandNode);
            logPoint("hmd", player->vrNodes.hmdNode);
        }
        HandPose right, left;
        if (readHand(false, right)) {
            char message[160]{};
            std::snprintf(message, sizeof(message), "  right hand: (%.1f, %.1f, %.1f)", right.position.x, right.position.y, right.position.z);
            vrlockpick::runtime::logNote(message);
        }
        if (readHand(true, left)) {
            char message[160]{};
            std::snprintf(message, sizeof(message), "  left hand: (%.1f, %.1f, %.1f)", left.position.x, left.position.y, left.position.z);
            vrlockpick::runtime::logNote(message);
        }
    }

    // The keyhole in world space: the pin's rest position, and the axis the
    // pin turns about (its parent's Y axis). Node matrices are row-style (as
    // FRIK uses them): rows are the node's axes in world, so a local point
    // goes to world as t + R^T * (s * v) and a world vector to local as R * v.
    bool keyhole(Vec3& center, Vec3& axis) noexcept
    {
        if (!g_pinNode || !g_pinNode->parent || !g_session.haveBasePin) {
            return false;
        }
        auto& parent = *g_pinNode->parent;
        auto& r = parent.world.rotate;
        const auto& b = g_session.basePinPos;
        const float sc = parent.world.scale;
        center = {
            parent.world.translate.x + sc * (cell(r, 0, 0) * b.x + cell(r, 1, 0) * b.y + cell(r, 2, 0) * b.z),
            parent.world.translate.y + sc * (cell(r, 0, 1) * b.x + cell(r, 1, 1) * b.y + cell(r, 2, 1) * b.z),
            parent.world.translate.z + sc * (cell(r, 0, 2) * b.x + cell(r, 1, 2) * b.y + cell(r, 2, 2) * b.z),
        };
        axis = { cell(r, 1, 0), cell(r, 1, 1), cell(r, 1, 2) };
        const float length = std::sqrt(dot(axis, axis));
        if (length < 0.001f) {
            return false;
        }
        axis = { axis.x / length, axis.y / length, axis.z / length };
        return true;
    }

    // Hand position around the keyhole axis: the part of (hand - center)
    // across the axis. Returns its length.
    float acrossAxis(const Vec3& hand, const Vec3& center, const Vec3& axis, Vec3& out) noexcept
    {
        const Vec3 d{ hand.x - center.x, hand.y - center.y, hand.z - center.z };
        const float along = dot(d, axis);
        out = { d.x - axis.x * along, d.y - axis.y * along, d.z - axis.z * along };
        return std::sqrt(dot(out, out));
    }

    // Signed angle from reference to now around the axis, degrees.
    float orbitDegrees(const Vec3& reference, const Vec3& now, const Vec3& axis) noexcept
    {
        return std::atan2(dot(axis, cross(reference, now)), dot(reference, now)) * 180.0f / kPi;
    }

    // Updates one hand's angle around the keyhole; a hand too close to the
    // axis keeps its last angle (the direction would be noise).
    void updateOrbit(const HandPose& pose, bool& haveReference, Vec3& reference, float& angle, float& distance) noexcept
    {
        Vec3 center, axis, across;
        if (!keyhole(center, axis)) {
            return;
        }
        distance = acrossAxis(pose.position, center, axis, across);
        if (distance < 3.0f) {
            return;
        }
        if (!haveReference) {
            reference = across;
            haveReference = true;
            char message[200]{};
            std::snprintf(message, sizeof(message), "  orbit reference: keyhole (%.1f, %.1f, %.1f) axis (%.2f, %.2f, %.2f), hand %.1f from the axis",
                center.x, center.y, center.z, axis.x, axis.y, axis.z, distance);
            vrlockpick::runtime::logNote(message);
        }
        angle = orbitDegrees(reference, across, axis);
    }

    // Twist of the screwdriver hand around the keyhole axis: the angle of a
    // hand axis across the keyhole (its side, or the fingers if the side
    // points along the keyhole) since the menu opened.
    void updateTwist(const HandPose& pose) noexcept
    {
        auto& s = g_session;
        Vec3 center, axis;
        if (!keyhole(center, axis)) {
            return;
        }
        if (!s.haveTwistReference) {
            s.twistUsesFingers = std::fabs(dot(pose.side, axis)) > 0.8f;
        }
        const Vec3& v = s.twistUsesFingers ? pose.alongFingers : pose.side;
        const float along = dot(v, axis);
        const Vec3 across{ v.x - axis.x * along, v.y - axis.y * along, v.z - axis.z * along };
        if (dot(across, across) < 0.04f) {
            return;  // that hand axis points along the keyhole right now
        }
        if (!s.haveTwistReference) {
            s.twistReference = across;
            s.haveTwistReference = true;
        }
        s.wrenchTwist = orbitDegrees(s.twistReference, across, axis);
    }

    bool triggerHeld(const bool left) noexcept
    {
        if (!g_input || g_ownerToken == 0) {
            return true;  // no input API: the pin always follows the hand
        }
        // OpenVR k_EButton_SteamVR_Trigger.
        constexpr std::uint32_t kTriggerButton = 33;
        input::RawWandButtonStateV1 button{};
        const auto status = g_input->getRawWandButtonStateV1(g_ownerToken, left ? rock::api::Hand::Left : rock::api::Hand::Right, kTriggerButton, &button);
        return status == rock::api::Status::Ok && button.available && button.held;
    }

    float wrapDegrees(float angle) noexcept
    {
        while (angle > 180.0f) {
            angle -= 360.0f;
        }
        while (angle < -180.0f) {
            angle += 360.0f;
        }
        return angle;
    }

    // ---- Tools in the hands ----
    // Each tool is described in its own space: the tip (the point in the
    // keyhole) and the hold point (where the fingers are). Measured once
    // the intro is over, from where the game put the tool and its bounds.
    struct Tool
    {
        RE::NiAVObject* node{ nullptr };
        RE::NiTransform rest;  // local transform the game gave it
        Vec3 tip;
        Vec3 hold;
        Vec3 side;  // the keyhole axis, in tool space
        Vec3 restDir;  // tip to hold at rest, in the lock parent's space
    };
    Tool g_pin;
    Tool g_screw;

    Vec3 worldToLocalDir(const RE::NiMatrix3& r, const Vec3& v) noexcept
    {
        auto& m = const_cast<RE::NiMatrix3&>(r);
        return {
            cell(m, 0, 0) * v.x + cell(m, 0, 1) * v.y + cell(m, 0, 2) * v.z,
            cell(m, 1, 0) * v.x + cell(m, 1, 1) * v.y + cell(m, 1, 2) * v.z,
            cell(m, 2, 0) * v.x + cell(m, 2, 1) * v.y + cell(m, 2, 2) * v.z,
        };
    }

    Vec3 normalized(const Vec3& v) noexcept
    {
        const float length = std::sqrt(dot(v, v));
        return length > 1e-5f ? Vec3{ v.x / length, v.y / length, v.z / length } : Vec3{ 1.0f, 0.0f, 0.0f };
    }

    Vec3 sub(const Vec3& a, const Vec3& b) noexcept { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
    Vec3 add(const Vec3& a, const Vec3& b) noexcept { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
    Vec3 scaled(const Vec3& a, const float k) noexcept { return { a.x * k, a.y * k, a.z * k }; }
    Vec3 fromNi(const RE::NiPoint3& p) noexcept { return { p.x, p.y, p.z }; }

    Vec3 localToWorldDir(const RE::NiMatrix3& r, const Vec3& v) noexcept
    {
        auto& m = const_cast<RE::NiMatrix3&>(r);
        return {
            cell(m, 0, 0) * v.x + cell(m, 1, 0) * v.y + cell(m, 2, 0) * v.z,
            cell(m, 0, 1) * v.x + cell(m, 1, 1) * v.y + cell(m, 2, 1) * v.z,
            cell(m, 0, 2) * v.x + cell(m, 1, 2) * v.y + cell(m, 2, 2) * v.z,
        };
    }

    // A point given in a node's own space, in world space.
    Vec3 nodePointWorld(const RE::NiAVObject& node, const Vec3& local) noexcept
    {
        return add(fromNi(node.world.translate), localToWorldDir(node.world.rotate, scaled(local, node.world.scale)));
    }

    Vec3 acrossOnly(const Vec3& v, const Vec3& axis) noexcept
    {
        return sub(v, scaled(axis, dot(v, axis)));
    }

    bool prepareTool(Tool& tool, RE::NiAVObject* node, const float holdFraction, const char* name) noexcept
    {
        Vec3 center, axis;
        if (!node || !node->parent || !keyhole(center, axis)) {
            return false;
        }
        tool.node = node;
        tool.rest = node->local;
        const auto& w = node->world;
        const float sc = w.scale > 1e-4f ? w.scale : 1.0f;
        const Vec3 origin = fromNi(w.translate);
        const Vec3 tip = worldToLocalDir(w.rotate, sub(center, origin));
        const Vec3 mid = worldToLocalDir(w.rotate, sub(fromNi(node->worldBound.center), origin));
        tool.tip = { tip.x / sc, tip.y / sc, tip.z / sc };
        const Vec3 boundCenter{ mid.x / sc, mid.y / sc, mid.z / sc };
        // The far end is opposite the tip across the bound's center.
        const Vec3 end{ 2.0f * boundCenter.x - tool.tip.x, 2.0f * boundCenter.y - tool.tip.y, 2.0f * boundCenter.z - tool.tip.z };
        tool.hold = { tool.tip.x + (end.x - tool.tip.x) * holdFraction, tool.tip.y + (end.y - tool.tip.y) * holdFraction,
            tool.tip.z + (end.z - tool.tip.z) * holdFraction };
        tool.side = worldToLocalDir(w.rotate, axis);
        tool.restDir = worldToLocalDir(g_pinNode->parent->world.rotate, localToWorldDir(w.rotate, normalized(sub(tool.hold, tool.tip))));
        const Vec3 length = sub(tool.hold, tool.tip);
        char message[256]{};
        std::snprintf(message, sizeof(message), "  %s: tip (%.1f, %.1f, %.1f) hold (%.1f, %.1f, %.1f) length %.1f, bound radius %.1f, scale %.2f",
            name, tool.tip.x, tool.tip.y, tool.tip.z, tool.hold.x, tool.hold.y, tool.hold.z, std::sqrt(dot(length, length)),
            node->worldBound.fRadius, sc);
        vrlockpick::runtime::logNote(message);
        return std::sqrt(dot(length, length)) > 0.5f;
    }

    // How a hand holds a tool, in the ROCK hand frame (x along the fingers,
    // y out of the back of the hand, z = x cross y; game units), measured
    // from the user's 3D reference (Blender, frame 1, 70 units per metre).
    struct HoldPose
    {
        Vec3 grip;     // where the fingers hold the tool
        Vec3 toTip;    // direction from the grip to the tool's tip
        Vec3 keyhole;  // the keyhole axis when the tool is in the lock
    };
    // Pin pinched between thumb and index, hanging below them.
    constexpr HoldPose kPinHold{ { 11.6f, -6.2f, 5.3f }, { -0.106f, -0.266f, -0.958f }, { -0.781f, -0.173f, 0.600f } };
    // Screwdriver handle in the palm, the shaft along the fingers.
    constexpr HoldPose kScrewHold{ { 3.9f, -3.2f, 1.3f }, { 0.921f, -0.174f, -0.348f }, { -0.915f, -0.014f, -0.403f } };

    // Where a tool goes: rows of rotate are its axes in world, origin its
    // node's position.
    struct Placement
    {
        RE::NiMatrix3 rotate;
        Vec3 origin;
    };

    // A tool in the hand as in the reference: its hold point at the grip,
    // pointing to its tip and turned so its keyhole axis matches.
    Placement toolInHand(const Tool& tool, const HandPose& hand, const HoldPose& pose, const Vec3* fingers, const Vec3* toTip) noexcept
    {
        auto* node = tool.node;
        auto& parent = *node->parent;
        const Vec3 hx = normalized(hand.alongFingers);
        const Vec3 hy = normalized(hand.side);
        const Vec3 hz = cross(hx, hy);
        const auto toWorld = [&](const Vec3& v) {
            return Vec3{ hx.x * v.x + hy.x * v.y + hz.x * v.z, hx.y * v.x + hy.y * v.y + hz.y * v.z, hx.z * v.x + hy.z * v.y + hz.z * v.z };
        };
        const Vec3 offset = toWorld(pose.grip);
        const Vec3 grip = fingers ? *fingers : Vec3{ hand.position.x + offset.x, hand.position.y + offset.y, hand.position.z + offset.z };
        // Matching bases: tool space (u, v, w) -> world (u2, v2, w2).
        const Vec3 u = normalized(sub(tool.tip, tool.hold));
        Vec3 v = sub(tool.side, { u.x * dot(tool.side, u), u.y * dot(tool.side, u), u.z * dot(tool.side, u) });
        v = normalized(v);
        const Vec3 w = cross(u, v);
        const Vec3 u2 = normalized(toTip ? *toTip : toWorld(pose.toTip));
        const Vec3 side = toWorld(pose.keyhole);
        Vec3 v2 = sub(side, { u2.x * dot(side, u2), u2.y * dot(side, u2), u2.z * dot(side, u2) });
        v2 = normalized(v2);
        const Vec3 w2 = cross(u2, v2);
        // World rotation, rows = tool axes in world: W[i][j] = u_i u2_j + v_i v2_j + w_i w2_j.
        const float uu[3]{ u.x, u.y, u.z }, vv[3]{ v.x, v.y, v.z }, ww[3]{ w.x, w.y, w.z };
        const float u2a[3]{ u2.x, u2.y, u2.z }, v2a[3]{ v2.x, v2.y, v2.z }, w2a[3]{ w2.x, w2.y, w2.z };
        Placement p;
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                cell(p.rotate, i, j) = uu[i] * u2a[j] + vv[i] * v2a[j] + ww[i] * w2a[j];
            }
        }
        // Origin so that the hold point lands on the grip: O = G - W^T (s * hold).
        const float s = parent.world.scale * node->local.scale;
        p.origin = sub(grip, localToWorldDir(p.rotate, scaled(tool.hold, s)));
        return p;
    }

    // Children's world transforms from their parent's (world rotation =
    // local * parent, position = parent + parent R^T (scale * local)).
    void updateChildrenWorld(RE::NiAVObject* object, const int depth) noexcept
    {
        auto* node = object ? object->IsNode() : nullptr;
        if (!node || depth > 6) {
            return;
        }
        auto& children = node->GetRuntimeData().children;
        for (std::uint16_t index = 0; index < children.size(); ++index) {
            auto* child = children[index].get();
            if (!child) {
                continue;
            }
            auto& P = object->world.rotate;
            auto& L = child->local.rotate;
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) {
                    cell(child->world.rotate, i, j) = cell(L, i, 0) * cell(P, 0, j) + cell(L, i, 1) * cell(P, 1, j) + cell(L, i, 2) * cell(P, 2, j);
                }
            }
            const Vec3 t = add(fromNi(object->world.translate), localToWorldDir(P, scaled(fromNi(child->local.translate), object->world.scale)));
            child->world.translate = { t.x, t.y, t.z };
            child->world.scale = object->world.scale * child->local.scale;
            updateChildrenWorld(child, depth + 1);
        }
    }

    // Moves a tool to a placement: its local transform and, with world (the
    // scene already updated this frame), its and its children's world
    // transforms. Returns where its tip is, in world space.
    Vec3 putTool(const Tool& tool, const Placement& p, const bool world) noexcept
    {
        auto* node = tool.node;
        auto& parent = *node->parent;
        // Local rotation = W * P^T.
        auto& P = parent.world.rotate;
        auto& L = node->local.rotate;
        auto& W = const_cast<RE::NiMatrix3&>(p.rotate);
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                cell(L, i, j) = cell(W, i, 0) * cell(P, j, 0) + cell(W, i, 1) * cell(P, j, 1) + cell(W, i, 2) * cell(P, j, 2);
            }
        }
        // Local translation = P * (O - parent t) / parent scale.
        const float ps = parent.world.scale > 1e-4f ? parent.world.scale : 1.0f;
        const Vec3 t = scaled(worldToLocalDir(P, sub(p.origin, fromNi(parent.world.translate))), 1.0f / ps);
        node->local.translate = { t.x, t.y, t.z };
        const float s = parent.world.scale * node->local.scale;
        if (world) {
            node->world.rotate = p.rotate;
            node->world.translate = { p.origin.x, p.origin.y, p.origin.z };
            node->world.scale = s;
            updateChildrenWorld(node, 0);
        }
        return add(p.origin, localToWorldDir(p.rotate, scaled(tool.tip, s)));
    }

    // ---- The visible fingers (FRIK body bones) ----
    bool fingerBone(const bool left, const char* bone, Vec3& out) noexcept
    {
        char name[32]{};
        std::snprintf(name, sizeof(name), "%sArm_%s", left ? "L" : "R", bone);
        RE::NiTransform t;
        if (!vrlockpick::grip::bone(name, t)) {
            return false;
        }
        out = fromNi(t.translate);
        return true;
    }

    // Where the visible fingers hold a tool: between the thumb and index
    // tips for the pin, the middle of the curled fingers for the screwdriver.
    bool fingerHold(const bool left, const bool pinch, Vec3& out) noexcept
    {
        if (pinch) {
            Vec3 thumb2, thumb3;
            RE::NiPoint3 index;
            if (!fingerBone(left, "Finger12", thumb2) || !fingerBone(left, "Finger13", thumb3) || !vrlockpick::grip::indexTip(left, index)) {
                return false;
            }
            // The thumb's last joint plus most of a segment more.
            const Vec3 thumb = add(thumb3, scaled(sub(thumb3, thumb2), 0.8f));
            out = scaled(add(thumb, fromNi(index)), 0.5f);
            return true;
        }
        constexpr const char* kCurled[]{ "Finger21", "Finger22", "Finger23", "Finger31", "Finger32", "Finger33", "Finger41", "Finger42", "Finger43" };
        Vec3 sum{};
        for (const auto* bone : kCurled) {
            Vec3 p;
            if (!fingerBone(left, bone, p)) {
                return false;
            }
            sum = add(sum, p);
        }
        out = scaled(sum, 1.0f / std::size(kCurled));
        return true;
    }

    // Where the fingers hold a tool, from the bones or else the reference.
    Vec3 holdPoint(const bool left, const bool pinch, const HandPose& hand, const HoldPose& pose) noexcept
    {
        Vec3 p;
        if (fingerHold(left, pinch, p)) {
            return p;
        }
        const Vec3 hx = normalized(hand.alongFingers);
        const Vec3 hy = normalized(hand.side);
        const Vec3 hz = cross(hx, hy);
        return add(hand.position, add(scaled(hx, pose.grip.x), add(scaled(hy, pose.grip.y), scaled(hz, pose.grip.z))));
    }

    // Where the controller alone would put each wrist. The hand FRIK
    // reports as tracked follows a held hand, so it can't be used (the hand
    // chased itself and shook); the controller (wand) never does. While a
    // hand is free its wrist is recorded in the wand's space, and a held
    // hand's wrist is that record carried by the wand.
    struct WristInWand
    {
        bool have{ false };
        RE::NiMatrix3 rotate;
        Vec3 translate;
    };
    WristInWand g_wristInWand[2];

    bool wristBone(const bool left, RE::NiTransform& out) noexcept
    {
        return vrlockpick::grip::bone(left ? "LArm_Hand" : "RArm_Hand", out);
    }

    // Rows are axes in world (world = R^T v): child R = local R * parent R.
    void recordWrist(const bool left) noexcept
    {
        RE::NiTransform wand, wrist;
        if (vrlockpick::grip::glued(left) || !vrlockpick::grip::wand(left, wand) || !wristBone(left, wrist)) {
            return;
        }
        auto& record = g_wristInWand[left ? 1 : 0];
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                cell(record.rotate, i, j) = cell(wrist.rotate, i, 0) * cell(wand.rotate, j, 0) + cell(wrist.rotate, i, 1) * cell(wand.rotate, j, 1) +
                                            cell(wrist.rotate, i, 2) * cell(wand.rotate, j, 2);
            }
        }
        const float sc = wand.scale > 1e-4f ? wand.scale : 1.0f;
        record.translate = scaled(worldToLocalDir(wand.rotate, sub(fromNi(wrist.translate), fromNi(wand.translate))), 1.0f / sc);
        record.have = true;
    }

    bool controllerWrist(const bool left, RE::NiTransform& out) noexcept
    {
        const auto& record = g_wristInWand[left ? 1 : 0];
        RE::NiTransform wand;
        if (!record.have || !vrlockpick::grip::wand(left, wand)) {
            return false;
        }
        out = wand;
        auto& recorded = const_cast<RE::NiMatrix3&>(record.rotate);
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                cell(out.rotate, i, j) = cell(recorded, i, 0) * cell(wand.rotate, 0, j) + cell(recorded, i, 1) * cell(wand.rotate, 1, j) +
                                         cell(recorded, i, 2) * cell(wand.rotate, 2, j);
            }
        }
        const Vec3 t = add(fromNi(wand.translate), localToWorldDir(wand.rotate, scaled(record.translate, wand.scale)));
        out.translate = { t.x, t.y, t.z };
        out.scale = 1.0f;
        return true;
    }

    // How far the controller is from the visible hand: zero unless the hand
    // is held on a tool. Hand input adds it, so a held hand still steers.
    Vec3 handShift(const bool left) noexcept
    {
        RE::NiTransform controller, wrist;
        if (!vrlockpick::grip::glued(left) || !controllerWrist(left, controller) || !wristBone(left, wrist)) {
            return {};
        }
        return sub(fromNi(controller.translate), fromNi(wrist.translate));
    }

    // Holds the visible hand with its fingers on the tool's hold point,
    // turned as the controller is; lets go if the controller is too far,
    // and takes the tool again once it is back within most of that.
    Vec3 g_glueAt[2];

    void glueHand(const bool left, const bool docked, const Tool& tool, const bool pinch, const HoldPose& pose) noexcept
    {
        recordWrist(left);
        RE::NiTransform controller, wrist;
        HandPose hand;
        if (!g_settings.glueHands || !docked || !tool.node || !controllerWrist(left, controller) || !wristBone(left, wrist) || !readHand(left, hand)) {
            vrlockpick::grip::unglue(left);
            return;
        }
        const Vec3 fingers = sub(holdPoint(left, pinch, hand, pose), fromNi(wrist.translate));
        Vec3 at = sub(nodePointWorld(*tool.node, tool.hold), fingers);
        const Vec3 pull = sub(at, fromNi(controller.translate));
        const float reach = vrlockpick::grip::glued(left) ? g_settings.glueMaxDistance : g_settings.glueMaxDistance * 0.7f;
        if (std::sqrt(dot(pull, pull)) > reach) {
            if (vrlockpick::grip::glued(left)) {
                char message[120]{};
                std::snprintf(message, sizeof(message), "  %s hand let go of the tool: controller %.1f away", left ? "left" : "right", std::sqrt(dot(pull, pull)));
                vrlockpick::runtime::logInfo(message);
            }
            vrlockpick::grip::unglue(left);
            return;
        }
        // Smooth out what's left of the frame-to-frame noise.
        auto& last = g_glueAt[left ? 1 : 0];
        if (vrlockpick::grip::glued(left)) {
            at = add(last, scaled(sub(at, last), 0.5f));
        }
        last = at;
        RE::NiTransform target = controller;
        target.translate = { at.x, at.y, at.z };
        vrlockpick::grip::glue(left, target);
    }

    // ---- Tools held rigidly in the hands ----
    // A tool placed from the hand pose and the finger bones each frame mixed
    // poses from different moments and wobbled in the hand on its own. Now
    // its placement is measured once in the wrist's space (while the hand is
    // still and the fingers have closed) and then carried by the wrist, as
    // if fixed to it.
    struct HeldTool
    {
        bool have{ false };
        int frames{ 0 };
        Vec3 lastAlong;
        Vec3 lastSide;
        RE::NiMatrix3 rotate;  // tool rows in the wrist's space
        Vec3 translate;        // tool origin in the wrist's space
    };
    HeldTool g_heldPin;
    HeldTool g_heldScrew;

    void measureHeld(HeldTool& held, const bool left, const HandPose& hand, const Placement& p, const char* name) noexcept
    {
        ++held.frames;
        const float moved = 2.0f - dot(held.lastAlong, hand.alongFingers) - dot(held.lastSide, hand.side);
        held.lastAlong = hand.alongFingers;
        held.lastSide = hand.side;
        RE::NiTransform wrist;
        // Fingers need a moment to close; then wait for a still hand (under
        // about half a degree a frame), or take it anyway after 1.5 s.
        if (held.frames < 30 || (moved > 4e-5f && held.frames < 135) || !wristBone(left, wrist)) {
            return;
        }
        auto& Wr = wrist.rotate;
        auto& W = const_cast<RE::NiMatrix3&>(p.rotate);
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                cell(held.rotate, i, j) = cell(W, i, 0) * cell(Wr, j, 0) + cell(W, i, 1) * cell(Wr, j, 1) + cell(W, i, 2) * cell(Wr, j, 2);
            }
        }
        const float sc = wrist.scale > 1e-4f ? wrist.scale : 1.0f;
        held.translate = scaled(worldToLocalDir(Wr, sub(p.origin, fromNi(wrist.translate))), 1.0f / sc);
        held.have = true;
        char message[200]{};
        std::snprintf(message, sizeof(message), "  %s fixed in the hand after %d frames: at (%.1f, %.1f, %.1f) from the wrist", name, held.frames,
            held.translate.x, held.translate.y, held.translate.z);
        vrlockpick::runtime::logNote(message);
    }

    bool heldPlacement(const HeldTool& held, const bool left, Placement& p) noexcept
    {
        RE::NiTransform wrist;
        if (!held.have || !wristBone(left, wrist)) {
            return false;
        }
        auto& Wr = wrist.rotate;
        auto& R = const_cast<RE::NiMatrix3&>(held.rotate);
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                cell(p.rotate, i, j) = cell(R, i, 0) * cell(Wr, 0, j) + cell(R, i, 1) * cell(Wr, 1, j) + cell(R, i, 2) * cell(Wr, 2, j);
            }
        }
        p.origin = add(fromNi(wrist.translate), localToWorldDir(Wr, scaled(held.translate, wrist.scale)));
        return true;
    }

    // Places a tool still in the hand; returns its tip in world space.
    Vec3 holdTool(const Tool& tool, HeldTool& held, const bool left, const bool pinch, const HandPose& hand, const HoldPose& pose, const char* name) noexcept
    {
        Placement p;
        if (!heldPlacement(held, left, p)) {
            Vec3 fingers;
            const bool haveFingers = fingerHold(left, pinch, fingers);
            // A screwdriver lies across the fist, its shaft out past the index.
            Vec3 index, pinky, across;
            const bool haveAcross = !pinch && fingerBone(left, "Finger21", index) &&
                                    (fingerBone(left, "Finger51", pinky) || fingerBone(left, "Finger41", pinky));
            if (haveAcross) {
                across = normalized(sub(index, pinky));
            }
            p = toolInHand(tool, hand, pose, haveFingers ? &fingers : nullptr, haveAcross ? &across : nullptr);
            measureHeld(held, left, hand, p, name);
        }
        return putTool(tool, p, false);
    }

    // The pin hand's controller: a short buzz when the pin reaches the sweet
    // spot, then a faint one while it stays there.
    void sweetSpotBuzz(const RE::LockpickingMenu& menu) noexcept
    {
        auto& s = g_session;
        if (!g_settings.sweetSpotBuzz || !s.pinDocked || menu.animating) {
            s.inSweetSpot = false;
            return;
        }
        const bool inside = std::fabs(menu.pickAngle - menu.sweetSpotCenter) <= menu.sweetSpotLength * 0.5f;
        if (inside && !s.inSweetSpot) {
            s.buzzFrames = 9;  // about a tenth of a second
            vrlockpick::runtime::logInfo("Pin on the sweet spot: buzz");
        }
        s.inSweetSpot = inside;
        if (s.buzzFrames > 0) {
            --s.buzzFrames;
            vrlockpick::haptics::pulse(g_settings.leftHandPick, 3000);
        } else if (inside && g_settings.sweetSpotHoldBuzz > 0.0f) {
            vrlockpick::haptics::pulse(g_settings.leftHandPick, static_cast<unsigned short>(std::clamp(g_settings.sweetSpotHoldBuzz, 0.0f, 1.0f) * 3000.0f));
        }
    }

    // FRIK's frame end (every bone final): the held tools again, on this
    // frame's wrists, world transforms included.
    void onHandsFinal() noexcept
    {
        const auto& s = g_session;
        if (!s.active || !g_settings.toolsInHands || !s.toolsPrepared) {
            return;
        }
        Placement p;
        if (!s.pinDocked && g_pin.node && heldPlacement(g_heldPin, g_settings.leftHandPick, p)) {
            (void)putTool(g_pin, p, true);
        }
        if (!s.screwDocked && g_screw.node && heldPlacement(g_heldScrew, !g_settings.leftHandPick, p)) {
            (void)putTool(g_screw, p, true);
        }
    }

    // After the game's frame: tools not yet at the keyhole follow the hands.
    void updateToolsInHands(RE::LockpickingMenu& menu) noexcept
    {
        auto& s = g_session;
        if (!g_settings.toolsInHands || menu.animating || !s.haveBasePin) {
            return;
        }
        if (!s.toolsPrepared) {
            s.toolsPrepared = true;
            g_heldPin = HeldTool{};
            g_heldScrew = HeldTool{};
            // Hold points along the tools, from the reference.
            s.pinDocked = !prepareTool(g_pin, g_pinNode, 0.79f, "pin");
            s.screwDocked = !prepareTool(g_screw, g_screwNode, 0.79f, "screwdriver");
            vrlockpick::runtime::logNote(s.pinDocked && s.screwDocked ? "Tools stay at the lock" : "Tools in the hands: bring them to the keyhole");
        }
        Vec3 center, axis;
        if (!keyhole(center, axis)) {
            return;
        }
        HandPose hand;
        if (!s.loggedFingers && readHand(g_settings.leftHandPick, hand)) {
            Vec3 pinch, fist;
            HandPose other;
            if (fingerHold(g_settings.leftHandPick, true, pinch) && readHand(!g_settings.leftHandPick, other) &&
                fingerHold(!g_settings.leftHandPick, false, fist)) {
                s.loggedFingers = true;
                const auto inHand = [](const HandPose& h, const Vec3& p) {
                    const Vec3 hx = normalized(h.alongFingers);
                    const Vec3 hy = normalized(h.side);
                    const Vec3 d = sub(p, h.position);
                    return Vec3{ dot(d, hx), dot(d, hy), dot(d, cross(hx, hy)) };
                };
                const Vec3 a = inHand(hand, pinch);
                const Vec3 b = inHand(other, fist);
                char message[200]{};
                std::snprintf(message, sizeof(message), "  fingers in the hand frame: pinch (%.1f, %.1f, %.1f), fist (%.1f, %.1f, %.1f)", a.x, a.y, a.z, b.x, b.y, b.z);
                vrlockpick::runtime::logNote(message);
            }
        }
        if (!s.pinDocked && readHand(g_settings.leftHandPick, hand)) {
            const Vec3 tip = holdTool(g_pin, g_heldPin, g_settings.leftHandPick, true, hand, kPinHold, "pin");
            const Vec3 d = sub(tip, center);
            if (std::sqrt(dot(d, d)) < g_settings.dockDistance) {
                s.pinDocked = true;
                g_pin.node->local = g_pin.rest;
                s.haveOrbitReference = false;
                s.armPickAngle = 0.0f;
                vrlockpick::runtime::logInfo("Pin in the keyhole");
            }
        }
        if (!s.screwDocked && readHand(!g_settings.leftHandPick, hand)) {
            const Vec3 tip = holdTool(g_screw, g_heldScrew, !g_settings.leftHandPick, false, hand, kScrewHold, "screwdriver");
            const Vec3 d = sub(tip, center);
            if (std::sqrt(dot(d, d)) < g_settings.dockDistance) {
                s.screwDocked = true;
                g_screw.node->local = g_screw.rest;
                s.haveWrenchOrbitReference = false;
                s.haveTwistReference = false;
                s.haveScrewZero = false;
                vrlockpick::runtime::logInfo("Screwdriver in the keyhole");
            }
        }
    }

    // The pick angle at which the docked pin points at the pinching fingers.
    // The pin model is turned by rotY(pickAngle * pinModelSign) in its
    // parent's space, which takes its direction (x, z) to
    // (x cos - z sin, x sin + z cos): the angle is measured that way in the
    // parent's x-z plane, from the pin's direction at angle 0.
    bool pinAngleTowardHand(float& angle) noexcept
    {
        auto& s = g_session;
        HandPose hand;
        Vec3 center, axis;
        if (!g_pin.node || !g_pin.node->parent || !readHand(g_settings.leftHandPick, hand) || !keyhole(center, axis)) {
            return false;
        }
        const bool left = g_settings.leftHandPick;
        const Vec3 fingers = add(holdPoint(left, true, hand, kPinHold), handShift(left));
        Vec3 across;
        s.pinchDistance = acrossAxis(fingers, center, axis, across);
        if (s.pinchDistance < 3.0f) {
            return false;  // too close to the keyhole for a direction
        }
        const Vec3 h = worldToLocalDir(g_pin.node->parent->world.rotate, across);
        const Vec3 d = sub(g_pin.hold, g_pin.tip);
        const float degrees = std::atan2(d.x * h.z - d.z * h.x, d.x * h.x + d.z * h.z) * 180.0f / kPi;
        angle = clampPick(degrees * g_settings.pinModelSign);
        return true;
    }

    // How far the screwdriver hand has swung the handle from where the
    // screwdriver rests, clockwise as seen from the player's side.
    bool screwAngleTowardHand(float& angle) noexcept
    {
        HandPose hand;
        Vec3 center, axis;
        const bool left = !g_settings.leftHandPick;
        if (!g_screw.node || !g_pinNode || !g_pinNode->parent || !readHand(left, hand) || !keyhole(center, axis)) {
            return false;
        }
        const Vec3 grip = add(holdPoint(left, false, hand, kScrewHold), handShift(left));
        Vec3 across;
        if (acrossAxis(grip, center, axis, across) < 3.0f) {
            return false;
        }
        // Positive turns are counter-clockwise looking against this axis.
        const Vec3 view = dot(axis, sub(grip, center)) >= 0.0f ? axis : scaled(axis, -1.0f);
        const Vec3 rest = acrossOnly(localToWorldDir(g_pinNode->parent->world.rotate, g_screw.restDir), axis);
        if (dot(rest, rest) < 1e-4f) {
            return false;
        }
        angle = -orbitDegrees(rest, across, view) * g_settings.screwSign;
        return true;
    }

    // Arm control (bArmControl): pin and screwdriver.
    void armControl(RE::LockpickingMenu& menu) noexcept
    {
        auto& s = g_session;
        // Screwdriver: the lock follows the hand, a quarter circle = 90.
        // Swinging around the keyhole (default) or twisting in place.
        const bool aimScrew = g_settings.screwPointsAtHand && g_settings.toolsInHands && !g_settings.screwTwist;
        float swing = 0.0f;
        const bool aimed = aimScrew && screwAngleTowardHand(swing);
        if (aimed) {
            if (!s.haveScrewZero) {
                s.haveScrewZero = true;
                s.screwZero = swing;
            }
            s.screwAngle = wrapDegrees(swing - s.screwZero);
        }
        const bool haveScrew = aimScrew ? aimed : g_settings.screwTwist ? s.haveTwistReference : s.haveWrenchOrbitReference;
        if (g_settings.turnWithHand && s.haveWrench && g_turnLock && haveScrew && s.screwDocked) {
            const float swung = aimScrew ? std::max(s.screwAngle, 0.0f) : std::fabs(g_settings.screwTwist ? s.wrenchTwist : s.wrenchOrbit);
            const float target = swung * g_settings.screwLockDegreesPerTwist;
            if (!s.handTurning && target > g_settings.armTurnStartDegrees) {
                s.handTurning = true;
                vrlockpick::runtime::logInfo("Screwdriver twisted: lock turn on");
            } else if (s.handTurning && target < g_settings.armTurnStopDegrees) {
                s.handTurning = false;
                g_turnLock(&menu, 0.0f);
                vrlockpick::runtime::logInfo("Screwdriver back: lock turn off");
            }
            if (s.handTurning) {
                // Catch up with the hand; a tiny value just keeps the tension.
                const float behind = target - menu.lockAngle;
                const float strength = behind > 1.0f ? std::clamp(behind / 15.0f, 0.15f, 1.0f) : 0.0005f;
                g_turnLock(&menu, strength);
            }
        }

        // Pin: held while the trigger is, moving with the hand around the keyhole.
        if (g_settings.drivePick && s.haveHand && s.pinDocked) {
            const bool trigger = !g_settings.pinNeedsTrigger || triggerHeld(g_settings.leftHandPick);
            const bool canMove = !menu.turningLock && !s.handTurning;
            if (trigger && canMove && !s.pinGrabbed) {
                s.pinGrabbed = true;
                s.grabOrbit = s.handOrbit;
                s.grabPickAngle = s.armPickAngle;
                vrlockpick::runtime::logInfo("Pin taken");
            } else if (s.pinGrabbed && (!trigger || !canMove)) {
                s.pinGrabbed = false;
                vrlockpick::runtime::logInfo("Pin let go");
            }
            float aimed = 0.0f;
            if (s.pinGrabbed && g_settings.pinPointsAtHand && g_settings.toolsInHands) {
                if (pinAngleTowardHand(aimed)) {
                    s.armPickAngle = aimed;
                }
            } else if (s.pinGrabbed) {
                const float moved = wrapDegrees(s.handOrbit - s.grabOrbit) * g_settings.armPickSign * g_settings.pickDegreesPerHandDegree;
                s.armPickAngle = clampPick(s.grabPickAngle + moved);
            }
            menu.pickAngle = s.armPickAngle;
        }
    }

    // Runs before the game's AdvanceMovie, so the game moves the pin model,
    // checks the sweet spot and turns the lock with our values, not the stick's.
    void beforeAdvance(RE::LockpickingMenu& menu) noexcept
    {
        auto& s = g_session;
        if (!s.active) {
            openSession(menu);
        }
        ++s.menuFrames;

        HandPose pose;
        s.haveHand = readHand(g_settings.leftHandPick, pose);
        if (s.haveHand) {
            pose.position = add(pose.position, handShift(g_settings.leftHandPick));
            if (!s.haveReference) {
                s.reference = pose;
                s.haveReference = true;
            }
            s.handRoll = rollDegrees(s.reference, pose);
            updateOrbit(pose, s.haveOrbitReference, s.orbitReference, s.handOrbit, s.handDistance);
        }
        HandPose wrench;
        s.haveWrench = readHand(!g_settings.leftHandPick, wrench);
        if (s.haveWrench) {
            wrench.position = add(wrench.position, handShift(!g_settings.leftHandPick));
            if (!s.haveWrenchReference) {
                s.wrenchReference = wrench;
                s.haveWrenchReference = true;
            }
            s.wrenchRoll = rollDegrees(s.wrenchReference, wrench);
            updateOrbit(wrench, s.haveWrenchOrbitReference, s.wrenchOrbitReference, s.wrenchOrbit, s.wrenchDistance);
            updateTwist(wrench);
        }
        const bool arm = g_settings.armControl;
        const float pickInput = arm ? s.handOrbit * g_settings.armPickSign : s.handRoll;
        const float turnInput = std::fabs(arm ? s.wrenchOrbit : s.wrenchRoll);
        const float turnStart = arm ? g_settings.armTurnStartDegrees : g_settings.turnStartDegrees;
        const float turnStop = arm ? g_settings.armTurnStopDegrees : g_settings.turnStopDegrees;
        const float turnFull = arm ? g_settings.armTurnFullDegrees : g_settings.turnFullDegrees;

        if (menu.animating) {
            return;  // intro and pin-break animations
        }
        if (g_settings.gripFingers) {
            vrlockpick::grip::hold(g_settings.leftHandPick, true);
            vrlockpick::grip::hold(!g_settings.leftHandPick, false);
        }
        if (arm) {
            armControl(menu);
            return;
        }

        // The other hand turns the lock like a screwdriver: roll it past
        // fTurnStartDegrees to start, back under fTurnStopDegrees to let go.
        // While turned, the game's turn function gets a stick-like strength
        // every frame (more roll, faster); when the hand is not turned it is
        // left alone and the stick still works.
        if (g_settings.turnWithHand && s.haveWrench && g_turnLock) {
            const float roll = turnInput;
            if (!s.handTurning && roll > turnStart) {
                s.handTurning = true;
                vrlockpick::runtime::logInfo("Wrench hand turned: lock turn on");
            } else if (s.handTurning && roll < turnStop) {
                s.handTurning = false;
                g_turnLock(&menu, 0.0f);
                vrlockpick::runtime::logInfo("Wrench hand back: lock turn off");
            }
            if (s.handTurning) {
                const float full = std::max(turnFull, turnStart + 1.0f);
                const float strength = std::clamp(roll / full, 0.3f, 1.0f);
                g_turnLock(&menu, strength);
            }
        }

        if (g_settings.drivePick && s.haveHand) {
            // The pin can't move while the lock is under tension.
            if (menu.turningLock || s.handTurning) {
                if (!s.pinHeld) {
                    s.pinHeld = true;
                    s.heldPickAngle = clampPick(pickInput * g_settings.pickDegreesPerHandDegree);
                }
                menu.pickAngle = s.heldPickAngle;
            } else {
                s.pinHeld = false;
                menu.pickAngle = clampPick(pickInput * g_settings.pickDegreesPerHandDegree);
            }
        }
    }

    void afterAdvance(RE::LockpickingMenu& menu) noexcept
    {
        auto& s = g_session;
        const auto now = Clock::now();
        // The game turns the pin model (LockPick01) only when the stick moves,
        // not from pickAngle, so the model is turned here every frame, and
        // moved the way the game moves it (kPinOffsets) so it stays in the keyhole.
        if (g_pinNode) {
            s.gamePinAngle = pinNodeDegrees(g_pinNode);
            auto& t = g_pinNode->local.translate;
            const bool gameMoved = !s.pinWritten || std::fabs(s.gamePinAngle - s.writtenPinAngle) > 0.2f ||
                                   std::fabs(t.x - s.writtenPinPos.x) + std::fabs(t.z - s.writtenPinPos.z) > 0.01f;
            if (gameMoved && s.pinWritten && s.pinDocked && std::fabs(s.gamePinAngle - s.writtenPinAngle) > 0.2f) {
                char message[160]{};
                std::snprintf(message, sizeof(message), "    game moved pin: angle %.2f pos (%.3f, %.3f, %.3f)", s.gamePinAngle, t.x, t.y, t.z);
                vrlockpick::runtime::logInfo(message);
            }
            if (!s.haveBasePin && !menu.animating && std::fabs(s.gamePinAngle) < 0.5f) {
                s.basePinPos = t;
                s.haveBasePin = true;
            }
            if (g_settings.setPinModel && g_settings.drivePick && s.haveHand && s.haveBasePin && !menu.animating) {
                const float degrees = menu.pickAngle * g_settings.pinModelSign;
                setPinNodeDegrees(g_pinNode, degrees);
                const auto offset = pinOffset(menu.pickAngle);
                t.x = s.basePinPos.x + offset.x;
                t.y = s.basePinPos.y;
                t.z = s.basePinPos.z + offset.z;
                s.pinWritten = true;
                s.writtenPinAngle = degrees;
                s.writtenPinPos = t;
            }
        }
        updateToolsInHands(menu);
        if (g_settings.armControl && g_settings.toolsInHands && s.toolsPrepared && !menu.animating) {
            glueHand(g_settings.leftHandPick, s.pinDocked, g_pin, true, kPinHold);
            glueHand(!g_settings.leftHandPick, s.screwDocked, g_screw, false, kScrewHold);
        }
        sweetSpotBuzz(menu);
        const bool changed = menu.turningLock != s.lastTurning || menu.picksBroken != s.lastPicksBroken;
        if (changed) {
            logMenuState(menu.picksBroken != s.lastPicksBroken ? "Pick broke" : (menu.turningLock ? "Lock turn started" : "Lock turn stopped"), menu);
        }
        if (now - s.lastLog > std::chrono::milliseconds(500) &&
            (std::fabs(menu.pickAngle - s.lastPickAngle) > 0.5f || std::fabs(menu.lockAngle - s.lastLockAngle) > 0.5f)) {
            char message[320]{};
            std::snprintf(message, sizeof(message), "  pick=%.1f gamePin=%.1f lock=%.1f handRoll=%.1f wrenchRoll=%.1f orbit=%.1f/%.1f twist=%.1f screw=%.1f%s dist=%.1f/%.1f (hands %s/%s%s%s) sweet=%.1f+-%.1f",
                menu.pickAngle, s.gamePinAngle, menu.lockAngle, s.handRoll, s.wrenchRoll, s.handOrbit, s.wrenchOrbit, s.wrenchTwist, s.screwAngle, s.pinGrabbed ? " held" : "", s.handDistance, s.wrenchDistance, s.haveHand ? "ok" : "missing", s.haveWrench ? "ok" : "missing",
                vrlockpick::grip::glued(true) ? ", left on tool" : "", vrlockpick::grip::glued(false) ? ", right on tool" : "",
                menu.sweetSpotCenter, menu.sweetSpotLength * 0.5f);
            vrlockpick::runtime::logInfo(message);
            s.lastLog = now;
        }
        if (now - s.lastNodeLog > std::chrono::milliseconds(300)) {
            logTurnedNodes();
            logChangedMenuFloats(menu);
            s.lastNodeLog = now;
        }
        s.lastPickAngle = menu.pickAngle;
        s.lastLockAngle = menu.lockAngle;
        s.lastTurning = menu.turningLock;
        s.lastPicksBroken = menu.picksBroken;
    }

    // LockpickingMenu::AdvanceMovie (vtable slot 4) runs every frame the menu is up.
    using AdvanceMovie_t = void (*)(RE::LockpickingMenu*, float, std::uint64_t);
    AdvanceMovie_t g_originalAdvanceMovie{ nullptr };

    void hookedAdvanceMovie(RE::LockpickingMenu* menu, const float timeDelta, const std::uint64_t time)
    {
        if (menu) {
            try {
                beforeAdvance(*menu);
            } catch (...) {
            }
        }
        g_originalAdvanceMovie(menu, timeDelta, time);
        if (menu) {
            try {
                afterAdvance(*menu);
            } catch (...) {
            }
        }
    }

    // VR's thumbstick handler for the menu (slot 7 of its input vtable at
    // +0x10; VR has one more input slot than the flat game). The idle turn
    // stick sends x = 0 every frame, which would let go of the hand's turn,
    // so stick events are skipped while the hand is turning.
    using OnThumbstick_t = void (*)(void*, void*);
    OnThumbstick_t g_originalOnThumbstick{ nullptr };
    constexpr std::uintptr_t kOnThumbstickOffset = 0x131BA00;

    void hookedOnThumbstick(void* user, void* event)
    {
        if (g_session.active && g_session.handTurning) {
            return;
        }
        g_originalOnThumbstick(user, event);
    }

    // The vtable is taken from the live menu, so no address library entry is needed.
    void hookMenu(RE::IMenu* menu) noexcept
    {
        if (g_originalAdvanceMovie || !menu) {
            return;
        }
        auto* vtable = *reinterpret_cast<std::uintptr_t**>(menu);
        g_originalAdvanceMovie = reinterpret_cast<AdvanceMovie_t>(vtable[4]);
        REL::safe_write(reinterpret_cast<std::uintptr_t>(&vtable[4]), reinterpret_cast<std::uintptr_t>(&hookedAdvanceMovie));
        vrlockpick::runtime::logNote("Hooked LockpickingMenu::AdvanceMovie");

        auto* inputVtable = *reinterpret_cast<std::uintptr_t**>(reinterpret_cast<std::uint8_t*>(menu) + 0x10);
        if (inputVtable[7] == REL::Module::get().base() + kOnThumbstickOffset) {
            g_originalOnThumbstick = reinterpret_cast<OnThumbstick_t>(inputVtable[7]);
            REL::safe_write(reinterpret_cast<std::uintptr_t>(&inputVtable[7]), reinterpret_cast<std::uintptr_t>(&hookedOnThumbstick));
            vrlockpick::runtime::logNote("Hooked LockpickingMenu thumbstick handler");
        } else {
            vrlockpick::runtime::logWarning("LockpickingMenu thumbstick handler not where expected; the stick may interrupt the hand's turn");
        }

    }

    class MenuWatcher final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
    {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent& event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
        {
            try {
                if (event.menuName == RE::LockpickingMenu::MENU_NAME) {
                    if (event.opening) {
                        vrlockpick::runtime::logNote("LockpickingMenu open event");
                        if (auto* ui = RE::UI::GetSingleton()) {
                            const auto menu = ui->GetMenu(RE::LockpickingMenu::MENU_NAME);
                            if (!menu) {
                                vrlockpick::runtime::logWarning("LockpickingMenu not found in the menu map yet");
                            }
                            hookMenu(menu.get());
                        }
                    } else {
                        char message[160]{};
                        std::snprintf(message, sizeof(message), "Lockpicking closed after %llu menu frames (ROCK frames during it: %llu)",
                            static_cast<unsigned long long>(g_session.menuFrames),
                            static_cast<unsigned long long>(g_rockFrames - g_session.rockFramesAtOpen));
                        vrlockpick::runtime::logNote(message);
                        g_session.active = false;
                        vrlockpick::grip::release(true);
                        vrlockpick::grip::release(false);
                        vrlockpick::grip::unglue(true);
                        vrlockpick::grip::unglue(false);
                    }
                }
            } catch (...) {
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    MenuWatcher g_menuWatcher;

    bool connect(rock::api::Client& client, rock::api::QueryInterfaceV1) noexcept
    {
        if (client.acquire(static_cast<std::uint32_t>(hands::PermissionV1::Read), g_hands) != rock::api::Status::Ok) {
            g_hands = nullptr;
            vrlockpick::runtime::logWarning("ROCK hands interface unavailable; the pick can't follow the hand");
        }
        if (client.acquire(static_cast<std::uint32_t>(input::PermissionV1::Read), g_input) != rock::api::Status::Ok) {
            g_input = nullptr;
            vrlockpick::runtime::logWarning("ROCK input interface unavailable; the pin moves without the trigger");
        }
        return true;
    }

    bool start(const std::uint64_t ownerToken) noexcept
    {
        g_ownerToken = ownerToken;
        static bool ready = false;
        if (!ready) {
            readSettings();
            findTurnLock();
            vrlockpick::grip::onHandsFinal(onHandsFinal);
            if (auto* ui = RE::UI::GetSingleton()) {
                ui->RegisterSink<RE::MenuOpenCloseEvent>(&g_menuWatcher);
                vrlockpick::runtime::logNote("Watching for the lockpicking menu (build " VRLOCKPICK_BUILD ")");
            } else {
                vrlockpick::runtime::logWarning("UI singleton missing; lockpicking menu not watched");
            }
            ready = true;
        }
        return true;
    }

    void stop(const std::uint64_t) noexcept
    {
        g_ownerToken = 0;
    }

    void frame(const std::uint64_t, const rock::api::core::SnapshotV1&) noexcept
    {
        ++g_rockFrames;
    }
}

namespace vrlockpick::runtime
{
    const Definition& definition() noexcept
    {
        static const Definition value{
            .pluginName = "VRLockpick",
            .pluginVersion = 1,
            .onConnect = &connect,
            .onStart = &start,
            .onStop = &stop,
            .onFrame = &frame,
        };
        return value;
    }
}
