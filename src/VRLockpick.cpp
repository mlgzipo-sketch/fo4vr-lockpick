// VRLockpick: pick locks with your own hands in Fallout 4 VR.
// The bobby pin follows the roll of one hand and the lock turns when the
// other hand rolls like a screwdriver; the game's own rules do the rest.
#include <ROCK/Hands.h>
#include "Runtime.h"

#include <RE/Fallout.h>
#include <REL/Relocation.h>
#include <REX/W32/OLE32.h>
#include <REX/W32/SHELL32.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
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
        // Lock turning goes through the menu's own input handler: a turn
        // event is sent as if the game's turn button were held. It is the
        // one the game used last time (learned, saved in VRLockpick_turn.txt)
        // or, until then, a button event with this name.
        std::string turnEvent{ "RotateLock" };
        bool sendTurnEvents{ true };
        bool forceTurningFlag{ false };
        bool dumpGameCode{ true };
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
                auto text = line.substr(equals + 1);
                while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
                    text.pop_back();
                }
                while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
                    text.erase(text.begin());
                }
                const float value = std::strtof(text.c_str(), nullptr);
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
                } else if (key == "sTurnEvent") {
                    if (!text.empty()) {
                        g_settings.turnEvent = text;
                    }
                } else if (key == "bSendTurnEvents") {
                    g_settings.sendTurnEvents = value != 0.0f;
                } else if (key == "bDumpGameCode") {
                    g_settings.dumpGameCode = value != 0.0f;
                } else if (key == "bForceTurningFlag") {
                    g_settings.forceTurningFlag = value != 0.0f;
                } else if (key == "fPinModelSign") {
                    g_settings.pinModelSign = value;
                } else if (key == "fTurnStopDegrees") {
                    g_settings.turnStopDegrees = value;
                }
            }
        } catch (...) {
        }
        char message[384]{};
        std::snprintf(message, sizeof(message), "Settings: bDrivePick=%d bLeftHandPick=%d fPickDegreesPerHandDegree=%.2f fMaxPickAngle=%.1f bTurnWithHand=%d fTurnStartDegrees=%.1f fTurnStopDegrees=%.1f bSendTurnEvents=%d sTurnEvent=%s bForceTurningFlag=%d",
            g_settings.drivePick ? 1 : 0, g_settings.leftHandPick ? 1 : 0, g_settings.pickDegreesPerHandDegree, g_settings.maxPickAngle,
            g_settings.turnWithHand ? 1 : 0, g_settings.turnStartDegrees, g_settings.turnStopDegrees, g_settings.sendTurnEvents ? 1 : 0,
            g_settings.turnEvent.c_str(), g_settings.forceTurningFlag ? 1 : 0);
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
        bool handTurning{ false };
        float turnHeldSecs{ 0.0f };
        bool sentTurn{ false };
        Clock::time_point lastRealPress{};
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

    // ---- Lock turn through the menu's input handler ----
    // Setting turningLock alone doesn't turn the lock: the game computes the
    // damage angle and starts the turn only when its turn input arrives
    // (test 6). So the screwdriver hand sends that input, as if held.

    struct TurnInput
    {
        bool valid{ false };
        bool thumbstick{ false };
        std::int32_t device{ static_cast<std::int32_t>(RE::INPUT_DEVICE::kGamepad) };
        std::int32_t deviceID{ 0 };
        std::int32_t idCode{ -1 };
        std::string name;
        float x{ 0.0f };
        float y{ 0.0f };
    };
    TurnInput g_turnInput;      // learned from the game's own turn, if any
    TurnInput g_lastRealPress;  // the latest real press, learned when a turn follows it
    Clock::time_point g_lastRealPressTime{};
    constexpr const char* kTurnFile = "Data/F4SE/Plugins/VRLockpick_turn.txt";

    void describeTurnInput(const char* label, const TurnInput& input) noexcept
    {
        char message[256]{};
        std::snprintf(message, sizeof(message), "%s: %s '%s' id=0x%X device=%d/%d xy=(%.2f, %.2f)", label,
            input.thumbstick ? "thumbstick" : "button", input.name.c_str(), static_cast<unsigned>(input.idCode),
            input.device, input.deviceID, input.x, input.y);
        vrlockpick::runtime::logNote(message);
    }

    void saveTurnInput() noexcept
    {
        try {
            std::ofstream file(kTurnFile, std::ios::trunc);
            file << (g_turnInput.thumbstick ? 1 : 0) << ' ' << g_turnInput.device << ' ' << g_turnInput.deviceID << ' '
                 << g_turnInput.idCode << ' ' << g_turnInput.x << ' ' << g_turnInput.y << ' ' << g_turnInput.name << '\n';
        } catch (...) {
        }
    }

    void loadTurnInput() noexcept
    {
        try {
            std::ifstream file(kTurnFile);
            int thumbstick = 0;
            TurnInput input;
            if (file >> thumbstick >> input.device >> input.deviceID >> input.idCode >> input.x >> input.y >> input.name && !input.name.empty()) {
                input.thumbstick = thumbstick != 0;
                input.valid = true;
                g_turnInput = input;
                describeTurnInput("Turn input from the last game", g_turnInput);
            }
        } catch (...) {
        }
    }

    // A ButtonEvent/ThumbstickEvent the game can read. Its vtable is ours, so
    // nothing depends on the game's event vtables.
    struct FakeInputEvent
    {
        const void* const* vtable{ nullptr };
        std::int32_t device{ 0 };
        std::int32_t deviceID{ 0 };
        std::int32_t eventType{ 0 };
        std::int32_t pad14{ 0 };
        void* next{ nullptr };
        std::uint32_t timeCode{ static_cast<std::uint32_t>(-1) };
        std::int32_t handled{ 0 };
        RE::BSFixedString userEvent;  // 28
        std::int32_t idCode{ -1 };    // 30
        bool disabled{ false };       // 34
        float value0{ 0.0f };         // 38: value / xValue
        float value1{ 0.0f };         // 3C: heldDownSecs / yValue
        std::int32_t value2{ 0 };     // 40: prevDir
        std::int32_t value3{ 0 };     // 44: currDir
    };
    static_assert(sizeof(RE::BSFixedString) == 8);
    static_assert(offsetof(FakeInputEvent, userEvent) == 0x28);
    static_assert(offsetof(FakeInputEvent, value0) == 0x38);
    static_assert(sizeof(FakeInputEvent) == 0x48);

    // The exe on disk is encrypted (Steam), so the game's code is read from
    // memory once and saved next to the log, to find how VR lockpicking reads
    // the turn stick. Only the code section; written only if not there yet.
    void dumpGameCode() noexcept
    {
        try {
            wchar_t* documents{ nullptr };
            const auto result = REX::W32::SHGetKnownFolderPath(
                REX::W32::FOLDERID_Documents, REX::W32::KF_FLAG_DEFAULT, nullptr, std::addressof(documents));
            std::unique_ptr<wchar_t[], decltype(&REX::W32::CoTaskMemFree)> documentsPath(documents, REX::W32::CoTaskMemFree);
            if (!documentsPath || result != 0) {
                return;
            }
            const auto path = std::filesystem::path(documentsPath.get()) / "My Games" / "Fallout4VR" / "F4SE" / "VRLockpick_code.bin";
            if (std::filesystem::exists(path)) {
                return;
            }
            const auto& module = REL::Module::get();
            const auto text = module.segment(REL::Segment::text);
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file.write(static_cast<const char*>(text.pointer()), static_cast<std::streamsize>(text.size()));
            char message[200]{};
            std::snprintf(message, sizeof(message), "Saved the game's code section for analysis: rva 0x%zX, 0x%zX bytes", text.offset(), text.size());
            vrlockpick::runtime::logNote(message);
        } catch (...) {
        }
    }

    void* fakeDestructor(void* self, std::uint32_t) noexcept { return self; }
    std::uintptr_t fakeNothing(const void*) noexcept { return 0; }
    bool fakeHasIDCode(const void*) noexcept { return true; }
    const RE::BSFixedString* fakeUserEvent(const FakeInputEvent* self) noexcept { return &self->userEvent; }
    const void* const kFakeVtable[8]{
        reinterpret_cast<const void*>(&fakeDestructor),
        reinterpret_cast<const void*>(&fakeNothing),
        reinterpret_cast<const void*>(&fakeHasIDCode),
        reinterpret_cast<const void*>(&fakeUserEvent),
        reinterpret_cast<const void*>(&fakeNothing),
        reinterpret_cast<const void*>(&fakeNothing),
        reinterpret_cast<const void*>(&fakeNothing),
        reinterpret_cast<const void*>(&fakeNothing),
    };

    using ShouldHandle_t = bool (*)(RE::BSInputEventUser*, const RE::InputEvent*);
    ShouldHandle_t g_originalShouldHandle{ nullptr };
    using OnButton_t = void (*)(RE::BSInputEventUser*, const RE::ButtonEvent*);
    using OnThumbstick_t = void (*)(RE::BSInputEventUser*, const RE::ThumbstickEvent*);
    OnButton_t g_originalOnButton{ nullptr };
    OnThumbstick_t g_originalOnThumbstick{ nullptr };
    RE::BSInputEventUser* g_inputUser{ nullptr };
    bool g_warnedNoInput{ false };

    RE::LockpickingMenu* menuFromInputUser(RE::BSInputEventUser* user) noexcept
    {
        return static_cast<RE::LockpickingMenu*>(static_cast<RE::IMenu*>(user));
    }

    // Sends the turn input: down=true while the hand is turned (heldSecs
    // grows like a held button), down=false once to let go.
    void sendTurn(const bool down, const float heldSecs) noexcept
    {
        if (!g_inputUser || !g_originalOnButton || !g_originalOnThumbstick) {
            if (!g_warnedNoInput) {
                vrlockpick::runtime::logWarning("Menu input not hooked; the hand can't send the turn input");
                g_warnedNoInput = true;
            }
            return;
        }
        FakeInputEvent event;
        event.vtable = kFakeVtable;
        const bool learned = g_turnInput.valid;
        event.device = g_turnInput.device;
        event.deviceID = g_turnInput.deviceID;
        event.idCode = learned ? g_turnInput.idCode : -1;
        event.userEvent = learned ? g_turnInput.name.c_str() : g_settings.turnEvent.c_str();
        if (learned && g_turnInput.thumbstick) {
            event.eventType = static_cast<std::int32_t>(RE::INPUT_EVENT_TYPE::kThumbstick);
            event.value0 = down ? g_turnInput.x : 0.0f;
            event.value1 = down ? g_turnInput.y : 0.0f;
            g_originalOnThumbstick(g_inputUser, reinterpret_cast<const RE::ThumbstickEvent*>(&event));
        } else {
            event.eventType = static_cast<std::int32_t>(RE::INPUT_EVENT_TYPE::kButton);
            event.value0 = down ? 1.0f : 0.0f;
            event.value1 = heldSecs;
            g_originalOnButton(g_inputUser, reinterpret_cast<const RE::ButtonEvent*>(&event));
        }
    }

    void learnTurnInput(const TurnInput& input) noexcept
    {
        const bool same = g_turnInput.valid && g_turnInput.thumbstick == input.thumbstick &&
                          g_turnInput.name == input.name && g_turnInput.idCode == input.idCode;
        if (!same) {
            g_turnInput = input;
            saveTurnInput();
            describeTurnInput("Learned the game's turn input", g_turnInput);
        }
    }

    // Remembers a real press; if the game started a turn while handling it,
    // that press is the turn input.
    void rememberRealPress(const RE::IDEvent& event, const bool thumbstick, const float x, const float y, const bool startedTurn) noexcept
    {
        TurnInput input;
        input.valid = true;
        input.thumbstick = thumbstick;
        input.device = static_cast<std::int32_t>(event.device.underlying());
        input.deviceID = event.deviceID;
        input.idCode = event.idCode;
        const auto name = event.QRawUserEvent();
        input.name.assign(name.data(), name.size());
        input.x = x;
        input.y = y;
        if (input.name.empty()) {
            return;
        }
        if (startedTurn) {
            learnTurnInput(input);
        }
        // The left stick moves the pin all the time; only other inputs are
        // candidates for a turn the game starts a frame later.
        if (!thumbstick || event.idCode == RE::ThumbstickEvent::kRight) {
            g_lastRealPress = input;
            g_lastRealPressTime = Clock::now();
        }
    }

    // Logs which input reaches the menu at all (test 7: no event handler ran).
    bool hookedShouldHandle(RE::BSInputEventUser* user, const RE::InputEvent* event)
    {
        const bool result = g_originalShouldHandle(user, event);
        try {
            static int logged = 0;
            if (event && logged < 60) {
                ++logged;
                // Button, cursor and thumbstick events carry a user event name
                // (no virtual call: VR's vtables may not match the headers).
                const auto type = event->eventType.get();
                const bool hasId = type == RE::INPUT_EVENT_TYPE::kButton || type == RE::INPUT_EVENT_TYPE::kCursorMove ||
                                   type == RE::INPUT_EVENT_TYPE::kThumbstick;
                const auto* id = hasId ? static_cast<const RE::IDEvent*>(event) : nullptr;
                std::string name;
                if (id) {
                    const auto raw = id->QRawUserEvent();
                    name.assign(raw.data(), raw.size());
                }
                char message[200]{};
                std::snprintf(message, sizeof(message), "  input offered: type=%d '%s' id=0x%X device=%d -> %s",
                    static_cast<int>(event->eventType.underlying()), name.c_str(), id ? static_cast<unsigned>(id->idCode) : 0u,
                    static_cast<int>(event->device.underlying()), result ? "handled" : "ignored");
                vrlockpick::runtime::logInfo(message);
            }
        } catch (...) {
        }
        return result;
    }

    void hookedOnButton(RE::BSInputEventUser* user, const RE::ButtonEvent* event)
    {
        auto* menu = menuFromInputUser(user);
        const bool turningBefore = menu->turningLock;
        g_originalOnButton(user, event);
        try {
            if (event && (event->value == 0.0f || event->heldDownSecs == 0.0f)) {
                const auto name = event->QRawUserEvent();
                char message[256]{};
                std::snprintf(message, sizeof(message), "  input: button '%.*s' id=0x%X device=%d/%d value=%.2f held=%.2f turning %d->%d",
                    static_cast<int>(name.size()), name.data(), static_cast<unsigned>(event->idCode),
                    static_cast<int>(event->device.underlying()), event->deviceID, event->value, event->heldDownSecs,
                    turningBefore ? 1 : 0, menu->turningLock ? 1 : 0);
                vrlockpick::runtime::logInfo(message);
                if (event->value != 0.0f) {
                    rememberRealPress(*event, false, 0.0f, 0.0f, !turningBefore && menu->turningLock);
                }
            }
        } catch (...) {
        }
    }

    void hookedOnThumbstick(RE::BSInputEventUser* user, const RE::ThumbstickEvent* event)
    {
        auto* menu = menuFromInputUser(user);
        const bool turningBefore = menu->turningLock;
        g_originalOnThumbstick(user, event);
        try {
            if (event) {
                static bool wasPushed[2]{};
                const int stick = event->idCode == RE::ThumbstickEvent::kRight ? 1 : 0;
                const bool pushed = event->xValue * event->xValue + event->yValue * event->yValue > 0.25f;
                if (pushed != wasPushed[stick]) {
                    const auto name = event->QRawUserEvent();
                    char message[256]{};
                    std::snprintf(message, sizeof(message), "  input: thumbstick '%.*s' id=0x%X device=%d/%d xy=(%.2f, %.2f) turning %d->%d",
                        static_cast<int>(name.size()), name.data(), static_cast<unsigned>(event->idCode),
                        static_cast<int>(event->device.underlying()), event->deviceID, event->xValue, event->yValue,
                        turningBefore ? 1 : 0, menu->turningLock ? 1 : 0);
                    vrlockpick::runtime::logInfo(message);
                    wasPushed[stick] = pushed;
                }
                if (pushed) {
                    rememberRealPress(*event, true, event->xValue, event->yValue, !turningBefore && menu->turningLock);
                }
            }
        } catch (...) {
        }
    }

    // MSVC RTTI: vtable[-1] is the Complete Object Locator, which names the
    // class and the subobject's offset; checked before any slot is patched.
    bool vtableIs(const void* object, const std::uint32_t offset, const char* className) noexcept
    {
        const auto* vtable = *reinterpret_cast<const std::uintptr_t* const*>(object);
        const auto* locator = reinterpret_cast<const std::uint32_t*>(vtable[-1]);
        if (!locator || locator[0] != 1 || locator[1] != offset) {
            return false;
        }
        const auto imageBase = reinterpret_cast<std::uintptr_t>(locator) - locator[5];
        const char* name = reinterpret_cast<const char*>(imageBase + locator[3] + 0x10);
        char message[200]{};
        std::snprintf(message, sizeof(message), "  RTTI at +0x%X: %.120s", offset, name);
        vrlockpick::runtime::logNote(message);
        return std::strstr(name, className) != nullptr;
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

    // Runs before the game's AdvanceMovie, so the game moves the pin model,
    // checks the sweet spot and turns the lock with our values, not the stick's.
    void beforeAdvance(RE::LockpickingMenu& menu, const float timeDelta) noexcept
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
        }
        HandPose wrench;
        s.haveWrench = readHand(!g_settings.leftHandPick, wrench);
        if (s.haveWrench) {
            if (!s.haveWrenchReference) {
                s.wrenchReference = wrench;
                s.haveWrenchReference = true;
            }
            s.wrenchRoll = rollDegrees(s.wrenchReference, wrench);
        }

        if (menu.animating) {
            return;  // intro and pin-break animations
        }

        // The other hand turns the lock like a screwdriver: roll it past
        // fTurnStartDegrees to start, back under fTurnStopDegrees to let go.
        // While it is turned the game's turn input is sent every frame, as
        // if the button were held; when it is not, the real button still works.
        if (g_settings.turnWithHand && s.haveWrench) {
            const float roll = std::fabs(s.wrenchRoll);
            if (!s.handTurning && roll > g_settings.turnStartDegrees) {
                s.handTurning = true;
                s.turnHeldSecs = 0.0f;
                vrlockpick::runtime::logInfo(s.wrenchRoll > 0.0f ? "Wrench hand turned (+): lock turn on" : "Wrench hand turned (-): lock turn on");
                if (g_settings.sendTurnEvents) {
                    sendTurn(true, 0.0f);
                    s.sentTurn = true;
                }
            } else if (s.handTurning && roll < g_settings.turnStopDegrees) {
                s.handTurning = false;
                vrlockpick::runtime::logInfo("Wrench hand back: lock turn off");
                if (s.sentTurn) {
                    sendTurn(false, s.turnHeldSecs);
                    s.sentTurn = false;
                }
                if (g_settings.forceTurningFlag) {
                    menu.turningLock = false;
                }
            } else if (s.handTurning) {
                s.turnHeldSecs += timeDelta > 0.0f ? timeDelta : 0.011f;
                if (s.sentTurn) {
                    sendTurn(true, s.turnHeldSecs);
                }
            }
            if (s.handTurning && g_settings.forceTurningFlag) {
                menu.turningLock = true;
            }
        }

        if (g_settings.drivePick && s.haveHand) {
            // The pin can't move while the lock is under tension.
            if (menu.turningLock || s.handTurning) {
                if (!s.pinHeld) {
                    s.pinHeld = true;
                    s.heldPickAngle = clampPick(s.handRoll * g_settings.pickDegreesPerHandDegree);
                }
                menu.pickAngle = s.heldPickAngle;
            } else {
                s.pinHeld = false;
                menu.pickAngle = clampPick(s.handRoll * g_settings.pickDegreesPerHandDegree);
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
        // The game started a turn of its own: remember the input that did it,
        // so the hand sends exactly that from now on.
        if (menu.turningLock && !s.lastTurning && !s.sentTurn && g_lastRealPress.valid &&
            now - g_lastRealPressTime < std::chrono::milliseconds(500)) {
            learnTurnInput(g_lastRealPress);
        }
        const bool changed = menu.turningLock != s.lastTurning || menu.picksBroken != s.lastPicksBroken;
        if (changed) {
            logMenuState(menu.picksBroken != s.lastPicksBroken ? "Pick broke" : (menu.turningLock ? "Lock turn started" : "Lock turn stopped"), menu);
        }
        if (now - s.lastLog > std::chrono::milliseconds(500) &&
            (std::fabs(menu.pickAngle - s.lastPickAngle) > 0.5f || std::fabs(menu.lockAngle - s.lastLockAngle) > 0.5f)) {
            char message[256]{};
            std::snprintf(message, sizeof(message), "  pick=%.1f gamePin=%.1f lock=%.1f handRoll=%.1f wrenchRoll=%.1f (hands %s/%s) sweet=%.1f+-%.1f",
                menu.pickAngle, s.gamePinAngle, menu.lockAngle, s.handRoll, s.wrenchRoll, s.haveHand ? "ok" : "missing", s.haveWrench ? "ok" : "missing",
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
                beforeAdvance(*menu, timeDelta);
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

        // The menu's input handler (BSInputEventUser part): logged, used to
        // learn the turn input, and called to send it. Patched only if RTTI
        // confirms both vtables belong to LockpickingMenu.
        auto* user = static_cast<RE::BSInputEventUser*>(menu);
        const auto offset = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(user) - reinterpret_cast<std::uintptr_t>(menu));
        if (!vtableIs(menu, 0, "LockpickingMenu") || !vtableIs(user, offset, "LockpickingMenu")) {
            vrlockpick::runtime::logWarning("LockpickingMenu input vtable not confirmed by RTTI; turn input not hooked");
            return;
        }
        auto* inputVtable = *reinterpret_cast<std::uintptr_t**>(user);
        g_originalShouldHandle = reinterpret_cast<ShouldHandle_t>(inputVtable[1]);
        REL::safe_write(reinterpret_cast<std::uintptr_t>(&inputVtable[1]), reinterpret_cast<std::uintptr_t>(&hookedShouldHandle));
        g_originalOnThumbstick = reinterpret_cast<OnThumbstick_t>(inputVtable[4]);
        g_originalOnButton = reinterpret_cast<OnButton_t>(inputVtable[8]);
        REL::safe_write(reinterpret_cast<std::uintptr_t>(&inputVtable[4]), reinterpret_cast<std::uintptr_t>(&hookedOnThumbstick));
        REL::safe_write(reinterpret_cast<std::uintptr_t>(&inputVtable[8]), reinterpret_cast<std::uintptr_t>(&hookedOnButton));
        vrlockpick::runtime::logNote("Hooked LockpickingMenu input (thumbstick, button)");

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
                            if (menu) {
                                g_inputUser = static_cast<RE::BSInputEventUser*>(menu.get());
                            }
                        }
                    } else {
                        char message[160]{};
                        std::snprintf(message, sizeof(message), "Lockpicking closed after %llu menu frames (ROCK frames during it: %llu)",
                            static_cast<unsigned long long>(g_session.menuFrames),
                            static_cast<unsigned long long>(g_rockFrames - g_session.rockFramesAtOpen));
                        vrlockpick::runtime::logNote(message);
                        g_session.active = false;
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
            loadTurnInput();
            if (g_settings.dumpGameCode) {
                dumpGameCode();
            }
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
