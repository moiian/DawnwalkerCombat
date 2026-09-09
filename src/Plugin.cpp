#include "Direction.h"
#include <RE/T/ThumbstickEvent.h>
#include <Windows.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <atomic>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;
namespace
{
constexpr REL::Version TargetRuntime{1, 6, 1170, 0};
constexpr const char* Variable = "DW_InputDirection";
using Clock = std::chrono::steady_clock;

bool HasFocus()
{
    DWORD process = 0;
    const auto foreground = GetForegroundWindow();
    return foreground && GetWindowThreadProcessId(foreground, &process) && process == GetCurrentProcessId();
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

class Controller final : public RE::BSTEventSink<RE::InputEvent*>,
                         public RE::BSTEventSink<RE::MenuOpenCloseEvent>
{
public:
    // Process lifetime: SKSE does not dynamically unload plugins. Avoid a joining
    // thread destructor under the Windows loader lock at process exit.
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
        // The worker only detects foreground transitions. Every RE object and
        // animation-graph write remains in the game-thread task it submits.
        std::thread([this, tasks, focused = HasFocus()]() mutable {
            while (true) {
                const bool nowFocused = HasFocus();
                if (focused && !nowFocused && !focusResetQueued.exchange(true)) {
                    tasks->AddTask([this] {
                        focusResetQueued.store(false);
                        Reset("focus-lost");
                    });
                }
                focused = nowFocused;
                std::this_thread::sleep_for(100ms);
            }
        }).detach();
        spdlog::info("Menu sink and focus-edge watcher active");
    }

    void BeginLoad()
    {
        Reset("pre-load"); // clear the old graph before it is replaced
        loading = true;
        blocked = true;
    }

    void FinishLoad()
    {
        loading = false;
        graphStatus = -1;
        nextGraphCheck = {};
        Reset("new/post-load");
        blocked = true;
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
        const bool nowBlocked = loading || !HasFocus() || MenuBlocksInput();
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
    std::atomic_bool publishQueued{false}, focusResetQueued{false};
    bool inputSinkRegistered{false}, runtimeStarted{false};
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
        if (!SKSE::GetTaskInterface() || !SKSE::GetMessagingInterface()->RegisterListener(OnMessage)) return false;
        spdlog::info("DawnwalkerCombat 0.1.0 loaded for Skyrim 1.6.1170; raw axes are engine-normalized before DW deadzone, not physical ADC values");
        return true;
    } catch (const std::exception&) {
        return false;
    }
}
