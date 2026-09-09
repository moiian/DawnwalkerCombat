#include "Direction.h"
#include <RE/M/Main.h>
#include <RE/M/MovementHandler.h>
#include <RE/P/PlayerControlsData.h>
#include <RE/R/Renderer.h>
#include <RE/T/ThumbstickEvent.h>
#include <Windows.h>
#include <CommCtrl.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

using namespace std::chrono_literals;
namespace
{
constexpr REL::Version TargetRuntime{1, 6, 1170, 0};
constexpr const char* Variable = "DW_InputDirection";
constexpr UINT_PTR FocusSubclassId = 0x44574301; // "DWC" + implementation revision
using Clock = std::chrono::steady_clock;

void WriteLogPathBreadcrumb(const std::filesystem::path* a_logDirectory) noexcept
{
    try {
        wchar_t executable[MAX_PATH]{};
        const auto length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
        if (length == 0 || length >= MAX_PATH) return;

        std::ofstream breadcrumb{std::filesystem::path{executable}.parent_path() /
                "DawnwalkerCombat.log-path.txt",
            std::ios::trunc};
        if (!breadcrumb) return;
        breadcrumb << "DawnwalkerCombat diagnostic breadcrumb\n";
        if (a_logDirectory) {
            breadcrumb << "Resolved SKSE log file: "
                       << (*a_logDirectory / "DawnwalkerCombat.log").string() << '\n';
        } else {
            breadcrumb << "SKSE::log::log_directory() was unavailable before logger initialization.\n";
        }
    } catch (...) {
        // Breadcrumb failure must never prevent SKSE plugin loading.
    }
}

HWND FindSkyrimWindow()
{
    if (const auto main = RE::Main::GetSingleton(); main && main->wnd) {
        const auto window = reinterpret_cast<HWND>(main->wnd);
        if (IsWindow(window)) return window;
    }
    if (const auto renderer = RE::BSGraphics::Renderer::GetSingleton(); renderer) {
        const auto window = reinterpret_cast<HWND>(renderer->data.renderWindows[0].hWnd);
        if (IsWindow(window)) return window;
    }
    // Only a last-resort fallback. The process and window-thread checks below
    // reject a title match belonging to a different process or thread.
    return FindWindowW(nullptr, L"Skyrim Special Edition");
}

bool MenuBlocksInput()
{
    const auto ui = RE::UI::GetSingleton();
    if (!ui) return true;
    if (ui->GameIsPaused() || ui->IsApplicationMenuOpen() || ui->IsModalMenuOpen()) return true;
    for (const auto& menu : ui->menuStack) {
        if (menu && menu->menuFlags.any(RE::UI_MENU_FLAGS::kUsesMenuContext,
                RE::UI_MENU_FLAGS::kUsesCursor, RE::UI_MENU_FLAGS::kInventoryItemMenu)) return true;
    }
    return ui->IsMenuOpen("Dialogue Menu") || ui->IsMenuOpen("Loading Menu") ||
           ui->IsMenuOpen("Main Menu") || ui->IsMenuOpen("Console");
}

DW::Direction KeyboardDirection(std::uint32_t scanCode)
{
    // Physical DirectInput scan codes, independent of translated user-event names.
    switch (scanCode) {
    case 0x11: return DW::Direction::Up;    // W
    case 0x20: return DW::Direction::Right; // D
    case 0x1F: return DW::Direction::Down;  // S
    case 0x1E: return DW::Direction::Left;  // A
    default: return DW::Direction::None;
    }
}

const char* DeviceName(DW::Device device)
{
    switch (device) {
    case DW::Device::Keyboard: return "keyboard";
    case DW::Device::Gamepad: return "gamepad";
    default: return "none";
    }
}

class MovementHook final
{
public:
    static void Install()
    {
        if (installed) return;

        REL::Relocation<std::uintptr_t> vtable{RE::VTABLE_MovementHandler[0]};
        originalThumbstick = reinterpret_cast<ThumbstickFn>(vtable.write_vfunc(2, ProcessThumbstick));
        originalButton = reinterpret_cast<ButtonFn>(vtable.write_vfunc(4, ProcessButton));
        installed = true;
        spdlog::info("MovementHandler hook active; blocking suppresses player moveInputVec only");
    }

private:
    using ThumbstickFn = void (*)(RE::MovementHandler*, RE::ThumbstickEvent*, RE::PlayerControlsData*);
    using ButtonFn = void (*)(RE::MovementHandler*, RE::ButtonEvent*, RE::PlayerControlsData*);

    static void ProcessThumbstick(RE::MovementHandler* a_handler, RE::ThumbstickEvent* a_event,
        RE::PlayerControlsData* a_data)
    {
        originalThumbstick(a_handler, a_event, a_data);
        SuppressPlayerLocomotion(a_data);
    }

    static void ProcessButton(RE::MovementHandler* a_handler, RE::ButtonEvent* a_event,
        RE::PlayerControlsData* a_data)
    {
        originalButton(a_handler, a_event, a_data);
        SuppressPlayerLocomotion(a_data);
    }

    static void SuppressPlayerLocomotion(RE::PlayerControlsData* a_data)
    {
        const auto player = RE::PlayerCharacter::GetSingleton();
        // MovementHandler is the player input handler. Raw input has already been
        // observed by Controller::ProcessEvent, and vanilla has already processed
        // this event, so this leaves DW_InputDirection, camera input, and all
        // non-movement button handling intact.
        const bool suppress = a_data && player && player->IsBlocking();
        if (suppress) a_data->moveInputVec = {0.0F, 0.0F};
        if (suppress != suppressionActive) {
            suppressionActive = suppress;
            spdlog::info("movement suppression={}", suppress ? "active" : "inactive");
        }
    }

    inline static ThumbstickFn originalThumbstick;
    inline static ButtonFn originalButton;
    inline static bool installed{false};
    inline static bool suppressionActive{false};
};

class AttackToBlockCancel final : public RE::BSTEventSink<RE::BSAnimationGraphEvent>
{
public:
    static AttackToBlockCancel& Get()
    {
        static auto instance = new AttackToBlockCancel;
        return *instance;
    }

    void Register()
    {
        const auto player = RE::PlayerCharacter::GetSingleton();
        if (!player || registered) return;

        player->RemoveAnimationGraphEventSink(this);
        registered = player->AddAnimationGraphEventSink(this);
        if (registered) spdlog::info("attack-to-block animation event sink active");
        else spdlog::warn("attack-to-block animation event sink registration failed");
    }

    void Reset(const char* a_reason)
    {
        if (!active) return;
        active = false;
        cancelMotionLogged = false;
        spdlog::info("attack-to-block cancel active=false reason={}", a_reason);
    }

    [[nodiscard]] bool Active() const { return active; }

    void ObserveMotion(const RE::NiPoint3* a_translation)
    {
        const auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        if (motionSamplesPending != 0) {
            LogMotion("attack-to-block event-window motion", a_translation, active, player,
                lastTraceTag.c_str(), traceSequence);
            --motionSamplesPending;
        }

        if (active) {
            if (!cancelMotionLogged) {
                LogMotion("attack-to-block motion", a_translation, true, player, "lifecycle", traceSequence);
                cancelMotionLogged = true;
            }
            return;
        }

        const bool blocking = player->IsBlocking();
        if (!blocking) {
            moveToBlockMotionLogged = false;
            return;
        }
        if (!moveToBlockMotionLogged && HasHorizontalMotion(a_translation)) {
            LogMotion("move-to-block diagnostic", a_translation, false, player, "move-to-block", 0);
            moveToBlockMotionLogged = true;
        }
    }

    RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent* a_event,
        RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override
    {
        if (!a_event) return RE::BSEventNotifyControl::kContinue;

        const auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) return RE::BSEventNotifyControl::kContinue;

        const std::string_view tag{a_event->tag.c_str()};
        const bool attackStop = tag == "attackStop";
        if (!traceActive && !attackStop && tag.find("attack") != std::string_view::npos) {
            traceActive = true;
            ++traceSequence;
        }
        if (!traceActive && !RelevantTag(tag)) return RE::BSEventNotifyControl::kContinue;

        bool graphAttacking = false;
        player->GetGraphVariableBool("IsAttacking", graphAttacking);
        const bool blocking = player->IsBlocking();
        if (tag == "blockStartOut" && graphAttacking) {
            active = true;
            cancelMotionLogged = false;
        } else if (attackStop) {
            active = false;
            cancelMotionLogged = false;
        }

        lastTraceTag.assign(tag);
        motionSamplesPending = 1;
        spdlog::info("attack-to-block event sequence={} tag={} graph_attacking={} "
                     "actor_attacking=unavailable blocking={} active={} suppression={}",
            traceSequence, tag, graphAttacking, blocking, active, active);
        if (attackStop) traceActive = false;
        return RE::BSEventNotifyControl::kContinue;
    }

private:
    static bool RelevantTag(std::string_view a_tag)
    {
        return a_tag.find("attack") != std::string_view::npos ||
               a_tag.find("block") != std::string_view::npos ||
               a_tag.find("cancel") != std::string_view::npos ||
               a_tag.find("BFCO") != std::string_view::npos;
    }

    static bool HasHorizontalMotion(const RE::NiPoint3* a_translation)
    {
        return a_translation && (a_translation->x != 0.0F || a_translation->y != 0.0F);
    }

    void LogMotion(const char* a_label, const RE::NiPoint3* a_translation,
        bool a_suppression, RE::PlayerCharacter* a_player, const char* a_afterTag,
        std::uint32_t a_sequence)
    {
        const float x = a_translation ? a_translation->x : 0.0F;
        const float y = a_translation ? a_translation->y : 0.0F;
        bool graphAttacking = false;
        if (a_player) a_player->GetGraphVariableBool("IsAttacking", graphAttacking);
        const bool blocking = a_player && a_player->IsBlocking();
        spdlog::info("{} sequence={} after_tag={} translation_x={:.5f} translation_y={:.5f} "
                     "graph_attacking={} actor_attacking=unavailable blocking={} active={} suppression={}",
            a_label, a_sequence, a_afterTag, x, y, graphAttacking, blocking, active, a_suppression);
    }

    bool registered{false};
    bool active{false};
    bool cancelMotionLogged{false};
    bool moveToBlockMotionLogged{false};
    bool traceActive{false};
    std::uint32_t traceSequence{0};
    std::uint8_t motionSamplesPending{0};
    std::string lastTraceTag;
};

class AnimationMotionHook final
{
public:
    static void Install()
    {
        if (installed) return;

        // Skyrim 1.6.1170: MovementTweenerAgentAnimationDriven motion update
        // calls ProcessMotionData at this relocation/variant-offset call site.
        REL::Relocation<std::uintptr_t> target{
            RELOCATION_ID(41160, 42246), REL::VariantOffset(0x111, 0xFF, 0x111)};
        original = SKSE::GetTrampoline().write_call<5>(target.address(), ProcessMotionData);
        installed = true;
        AttackToBlockCancel::Get().Register();
        spdlog::info("animation motion hook active: ProcessMotionData call-site");
    }

    static void RegisterEvents() { AttackToBlockCancel::Get().Register(); }
    static void Reset(const char* a_reason) { AttackToBlockCancel::Get().Reset(a_reason); }

private:
    static bool ProcessMotionData(RE::TESObjectREFR* a_reference, float a_deltaTime,
        RE::NiPoint3* a_translation, RE::NiPoint3* a_rotation, bool* a_result)
    {
        const bool result = original(a_reference, a_deltaTime, a_translation, a_rotation, a_result);
        if (!a_reference || !a_reference->IsPlayerRef()) return result;

        auto& cancel = AttackToBlockCancel::Get();
        cancel.ObserveMotion(a_translation);
        if (a_translation && cancel.Active()) {
            a_translation->x = 0.0F;
            a_translation->y = 0.0F;
        }
        return result;
    }

    inline static REL::Relocation<decltype(ProcessMotionData)> original;
    inline static bool installed{false};
};

class Controller final : public RE::BSTEventSink<RE::InputEvent*>,
                         public RE::BSTEventSink<RE::MenuOpenCloseEvent>
{
public:
    static Controller& Get() { static auto instance = new Controller; return *instance; }

    void StartInput()
    {
        if (inputSinkRegistered) return;
        const auto input = RE::BSInputDeviceManager::GetSingleton();
        if (!input) {
            spdlog::error("Input device manager was unavailable at kInputLoaded");
            return;
        }
        input->AddEventSink(this);
        inputSinkRegistered = true;
        spdlog::info("Input sink active; W/A/S/D and left stick; deadzone={} axis_ratio={}",
            DW::InputState::Deadzone, DW::InputState::AxisRatio);
    }

    void StartRuntime()
    {
        if (runtimeStarted) return;
        const auto ui = RE::UI::GetSingleton();
        const auto tasks = SKSE::GetTaskInterface();
        if (!ui || !tasks) {
            spdlog::error("UI or task interface was unavailable at kDataLoaded");
            return;
        }
        ui->AddEventSink<RE::MenuOpenCloseEvent>(this);
        runtimeStarted = true;
        MovementHook::Install();
        AnimationMotionHook::Install();
        InstallFocusSubclass();
        spdlog::info("Menu sink active; focus reset uses WM_ACTIVATEAPP window subclass");
    }

    void BeginLoad()
    {
        Reset("pre-load"); // clear the old graph before it is replaced
        AnimationMotionHook::Reset("pre-load");
        loading = true;
        blocked = true;
    }

    void FinishLoad()
    {
        loading = false;
        graphStatus = -1;
        nextGraphCheck = {};
        Reset("new/post-load");
        AnimationMotionHook::Reset("new/post-load");
        AnimationMotionHook::RegisterEvents();
        blocked = true;
        if (runtimeStarted) InstallFocusSubclass();
    }

    RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* events,
        RE::BSTEventSource<RE::InputEvent*>*) override
    {
        if (!events || InputBlocked()) return RE::BSEventNotifyControl::kContinue;
        const auto before = state.Value();
        for (auto event = *events; event; event = event->next) {
            if (event->GetDevice() == RE::INPUT_DEVICE::kGamepad &&
                event->GetEventType() == RE::INPUT_EVENT_TYPE::kDeviceConnect) {
                const auto connection = static_cast<const RE::DeviceConnectEvent*>(event);
                if (!connection->connected) {
                    Reset("gamepad-disconnect-event");
                    break;
                }
            } else if (const auto button = event->AsButtonEvent();
                       button && event->GetDevice() == RE::INPUT_DEVICE::kKeyboard) {
                const auto direction = KeyboardDirection(button->GetIDCode());
                // Held repeats after a menu/focus/load reset must not restore a key.
                if (button->IsDown()) state.Key(direction, true);
                else if (!button->IsPressed()) state.Key(direction, false);
            } else if (event->GetDevice() == RE::INPUT_DEVICE::kGamepad &&
                       event->GetEventType() == RE::INPUT_EVENT_TYPE::kThumbstick) {
                // CommonLibSSE-NG v3.7.0 has no AsThumbstickEvent helper.
                // kThumbstick is the engine's type discriminator for this cast.
                const auto stick = static_cast<const RE::ThumbstickEvent*>(event);
                if (stick->IsLeft()) state.Stick(stick->xValue, stick->yValue);
            }
        }
        if (state.Value() != before) QueuePublish();
        LogInput();
        return RE::BSEventNotifyControl::kContinue;
    }

    RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* event,
        RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
    {
        if (event && event->opening) {
            const auto menu = RE::UI::GetSingleton()->GetMenu(event->menuName.c_str());
            if (menu && menu->menuFlags.any(RE::UI_MENU_FLAGS::kUsesMenuContext,
                    RE::UI_MENU_FLAGS::kUsesCursor, RE::UI_MENU_FLAGS::kPausesGame,
                    RE::UI_MENU_FLAGS::kModal, RE::UI_MENU_FLAGS::kInventoryItemMenu)) {
                Reset("menu-open");
                blocked = true;
            }
        }
        return RE::BSEventNotifyControl::kContinue;
    }

private:
    [[nodiscard]] bool InputBlocked()
    {
        const bool nowBlocked = loading || MenuBlocksInput();
        if (nowBlocked && !blocked) Reset("menu/loading");
        blocked = nowBlocked;
        return nowBlocked;
    }

    void QueuePublish()
    {
        if (loading || publishQueued.exchange(true)) return;
        SKSE::GetTaskInterface()->AddTask([this] {
            publishQueued.store(false);
            Publish();
        });
    }

    static LRESULT CALLBACK FocusSubclassProc(HWND window, UINT message, WPARAM wParam,
        LPARAM lParam, UINT_PTR, DWORD_PTR reference)
    {
        if (message == WM_ACTIVATEAPP && wParam == FALSE) {
            // The window callback deliberately does not touch RE/game objects.
            // It only queues a game-thread reset of the local state and graph value.
            if (const auto controller = reinterpret_cast<Controller*>(reference)) {
                controller->QueueFocusReset();
            }
        }
        return DefSubclassProc(window, message, wParam, lParam);
    }

    void InstallFocusSubclass()
    {
        if (focusSubclassInstalled || focusInstallQueued.exchange(true)) return;
        const auto tasks = SKSE::GetTaskInterface();
        if (!tasks) {
            focusInstallQueued.store(false);
            WarnFocusHook("task interface unavailable");
            return;
        }
        tasks->AddTask([this] {
            focusInstallQueued.store(false);
            if (focusSubclassInstalled) return;

            const auto window = FindSkyrimWindow();
            if (!window) {
                WarnFocusHook("Skyrim window unavailable");
                return;
            }
            DWORD process = 0;
            const auto windowThread = GetWindowThreadProcessId(window, &process);
            if (process != GetCurrentProcessId()) {
                WarnFocusHook("window belongs to another process");
                return;
            }
            // SetWindowSubclass must be called from the window-owning thread.
            if (windowThread != GetCurrentThreadId()) {
                WarnFocusHook("game task is not on the Skyrim window thread");
                return;
            }
            if (!SetWindowSubclass(window, FocusSubclassProc, FocusSubclassId,
                    reinterpret_cast<DWORD_PTR>(this))) {
                WarnFocusHook("SetWindowSubclass failed");
                return;
            }
            focusWindow = window;
            focusSubclassInstalled = true;
            spdlog::info("focus subclass active: WM_ACTIVATEAPP clears input state");
        });
    }

    void RemoveFocusSubclass()
    {
        if (!focusSubclassInstalled) return;
        if (focusWindow && IsWindow(focusWindow) &&
            GetWindowThreadProcessId(focusWindow, nullptr) == GetCurrentThreadId()) {
            if (!RemoveWindowSubclass(focusWindow, FocusSubclassProc, FocusSubclassId)) {
                WarnFocusHook("RemoveWindowSubclass failed");
            }
        }
        focusWindow = nullptr;
        focusSubclassInstalled = false;
    }

    void QueueFocusReset()
    {
        if (focusResetQueued.exchange(true)) return;
        const auto tasks = SKSE::GetTaskInterface();
        if (!tasks) {
            focusResetQueued.store(false);
            return;
        }
        tasks->AddTask([this] {
            focusResetQueued.store(false);
            Reset("focus-lost");
        });
    }

    void WarnFocusHook(const char* reason)
    {
        if (!focusWarningLogged.exchange(true)) {
            spdlog::warn("focus subclass inactive: {}; input plugin continues without focus reset", reason);
        }
    }

    void Reset(const char* reason)
    {
        const bool hadInput = state.Value() != DW::Direction::None || state.device != DW::Device::None;
        state.Reset();
        Publish();
        if (hadInput) spdlog::info("reset={} device=none raw_x=0 raw_y=0 direction=0", reason);
    }

    void Publish()
    {
        if (loading) return;
        const auto player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->Is3DLoaded()) return;
        const auto now = Clock::now();
        if (graphStatus == 0 && now < nextGraphCheck) return;
        nextGraphCheck = now + 500ms;
        const auto desired = static_cast<std::int32_t>(state.Value());
        std::int32_t current = -1;
        const RE::BSFixedString name(Variable);
        bool ok = player->GetGraphVariableInt(name, current);
        if (ok && (current != desired || graphStatus != 1)) {
            ok = player->SetGraphVariableInt(name, desired) &&
                 player->GetGraphVariableInt(name, current) && current == desired;
        }
        const int newStatus = ok ? 1 : 0;
        if (newStatus != graphStatus) {
            if (ok) spdlog::info("graph variable verified: {}={} (read/write)", Variable, desired);
            else spdlog::warn("{} unavailable/write failed; enable compatible Behavior Data Injector + DawnwalkerCombat_BDI.json. Input logs alone do NOT prove OAR works.", Variable);
            graphStatus = newStatus;
        }
    }

    void LogInput()
    {
        const auto now = Clock::now();
        const auto direction = state.Value();
        const bool changed = direction != loggedDirection || state.device != loggedDevice;
        const bool axisMoved = std::abs(state.rawX - loggedX) >= 0.10F ||
                               std::abs(state.rawY - loggedY) >= 0.10F;
        if (now < nextLog || (!changed && !(axisMoved && now >= nextAxisLog))) return;
        spdlog::info("device={} raw_x={:.3f} raw_y={:.3f} direction={}", DeviceName(state.device),
            state.rawX, state.rawY, static_cast<int>(direction));
        loggedDirection = direction;
        loggedDevice = state.device;
        loggedX = state.rawX;
        loggedY = state.rawY;
        nextLog = now + 100ms;
        nextAxisLog = now + 500ms;
    }

    DW::InputState state;
    std::atomic_bool publishQueued{false}, focusResetQueued{false}, focusInstallQueued{false},
        focusWarningLogged{false};
    HWND focusWindow{nullptr};
    bool inputSinkRegistered{false}, runtimeStarted{false}, focusSubclassInstalled{false};
    bool loading{true}, blocked{true};
    int graphStatus{-1};
    Clock::time_point nextGraphCheck{}, nextLog{}, nextAxisLog{};
    DW::Direction loggedDirection{DW::Direction::None};
    DW::Device loggedDevice{DW::Device::None};
    float loggedX{0}, loggedY{0};
};

void OnMessage(SKSE::MessagingInterface::Message* message)
{
    switch (message->type) {
    case SKSE::MessagingInterface::kInputLoaded: Controller::Get().StartInput(); break;
    case SKSE::MessagingInterface::kDataLoaded: Controller::Get().StartRuntime(); break;
    case SKSE::MessagingInterface::kPreLoadGame: Controller::Get().BeginLoad(); break;
    case SKSE::MessagingInterface::kPostLoadGame:
    case SKSE::MessagingInterface::kNewGame: Controller::Get().FinishLoad(); break;
    default: break;
    }
}
}

SKSEPluginInfo(
    .Version = {0, 1, 0, 0},
    .Name = "DawnwalkerCombat",
    .Author = "moiian",
    .RuntimeCompatibility = SKSE::PluginDeclaration::RuntimeCompatibility{TargetRuntime}
);

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    const auto directory = SKSE::log::log_directory();
    WriteLogPathBreadcrumb(directory ? &*directory : nullptr);
    if (!directory) return false;
    try {
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
            (*directory / "DawnwalkerCombat.log").string(), true);
        auto logger = std::make_shared<spdlog::logger>("DawnwalkerCombat", std::move(sink));
        spdlog::set_default_logger(std::move(logger));
        spdlog::set_pattern("[%H:%M:%S.%e] [%l] %v");
        spdlog::flush_on(spdlog::level::info);
        if (skse->RuntimeVersion() != TargetRuntime) {
            spdlog::error("Only Skyrim 1.6.1170 is supported by this milestone");
            return false;
        }
        SKSE::Init(skse);
        SKSE::AllocTrampoline(14);
        if (!SKSE::GetTaskInterface() || !SKSE::GetMessagingInterface()->RegisterListener(OnMessage)) return false;
        spdlog::info("DawnwalkerCombat 0.1.0 loaded for Skyrim 1.6.1170; raw axes are engine-normalized before DW deadzone, not physical ADC values");
        return true;
    } catch (const std::exception&) {
        return false;
    }
}
