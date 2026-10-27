// VRLockpick: pick locks with your own hands in Fallout 4 VR.
// The bobby pin follows the roll of one hand and the lock turns when the
// other hand rolls like a screwdriver; the game's own rules do the rest.
#include <ROCK/Hands.h>
#include "Grip.h"
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
        float armTurnStartDegrees{ 12.0f };
        float armTurnStopDegrees{ 6.0f };
        float armTurnFullDegrees{ 35.0f };
    };
    Settings g_settings;

    const hands::ApiV1* g_hands{};
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
    // pin turns about (its parent's Y axis). NiTransform: world = R * (s * v) + t.
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
            parent.world.translate.x + sc * (cell(r, 0, 0) * b.x + cell(r, 0, 1) * b.y + cell(r, 0, 2) * b.z),
            parent.world.translate.y + sc * (cell(r, 1, 0) * b.x + cell(r, 1, 1) * b.y + cell(r, 1, 2) * b.z),
            parent.world.translate.z + sc * (cell(r, 2, 0) * b.x + cell(r, 2, 1) * b.y + cell(r, 2, 2) * b.z),
        };
        axis = { cell(r, 0, 1), cell(r, 1, 1), cell(r, 2, 1) };
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
            if (!s.haveWrenchReference) {
                s.wrenchReference = wrench;
                s.haveWrenchReference = true;
            }
            s.wrenchRoll = rollDegrees(s.wrenchReference, wrench);
            updateOrbit(wrench, s.haveWrenchOrbitReference, s.wrenchOrbitReference, s.wrenchOrbit, s.wrenchDistance);
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
            if (gameMoved && s.pinWritten) {
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
        const bool changed = menu.turningLock != s.lastTurning || menu.picksBroken != s.lastPicksBroken;
        if (changed) {
            logMenuState(menu.picksBroken != s.lastPicksBroken ? "Pick broke" : (menu.turningLock ? "Lock turn started" : "Lock turn stopped"), menu);
        }
        if (now - s.lastLog > std::chrono::milliseconds(500) &&
            (std::fabs(menu.pickAngle - s.lastPickAngle) > 0.5f || std::fabs(menu.lockAngle - s.lastLockAngle) > 0.5f)) {
            char message[256]{};
            std::snprintf(message, sizeof(message), "  pick=%.1f gamePin=%.1f lock=%.1f handRoll=%.1f wrenchRoll=%.1f orbit=%.1f/%.1f dist=%.1f/%.1f (hands %s/%s) sweet=%.1f+-%.1f",
                menu.pickAngle, s.gamePinAngle, menu.lockAngle, s.handRoll, s.wrenchRoll, s.handOrbit, s.wrenchOrbit, s.handDistance, s.wrenchDistance, s.haveHand ? "ok" : "missing", s.haveWrench ? "ok" : "missing",
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
        return true;
    }

    bool start(const std::uint64_t ownerToken) noexcept
    {
        g_ownerToken = ownerToken;
        static bool ready = false;
        if (!ready) {
            readSettings();
            findTurnLock();
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
