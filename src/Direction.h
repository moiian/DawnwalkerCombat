#pragma once
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace DW
{
enum class Direction : std::int32_t { None = 0, Up = 1, Right = 2, Down = 3, Left = 4 };
enum class Device { None, Keyboard, Gamepad };

// Pure, engine-independent state machine. Tests exercise the same code as the DLL.
class InputState
{
public:
    static constexpr float Deadzone = 0.24F;
    // tan(50 degrees): retain the previous axis until ~5 degrees past the diagonal.
    static constexpr float AxisRatio = 1.191754F;

    void Reset()
    {
        keys = {};
        serial = 0;
        stick = Direction::None;
        device = Device::None;
        rawX = rawY = 0;
    }

    void Key(Direction direction, bool pressed)
    {
        const auto index = static_cast<unsigned>(direction);
        if (index == 0 || index > 4) return;
        if (pressed && keys[index] == 0) {
            keys[index] = ++serial;
            device = Device::Keyboard;
        } else if (!pressed) {
            keys[index] = 0;
        }
    }

    void Stick(float x, float y)
    {
        rawX = x;
        rawY = y;
        const auto previous = stick;
        stick = Classify(x, y, stick);
        // A steady held stick cannot steal ownership back from a keyboard press.
        // A fresh tilt or a direction change is meaningful gamepad input.
        if (stick != Direction::None && stick != previous) device = Device::Gamepad;
    }

    [[nodiscard]] Direction Value() const
    {
        if (device == Device::Gamepad) return stick;
        if (device != Device::Keyboard) return Direction::None;
        unsigned winner = 0;
        for (unsigned i = 1; i <= 4; ++i) {
            if (keys[i] > keys[winner]) winner = i;
        }
        return static_cast<Direction>(winner);
    }

    [[nodiscard]] static Direction Classify(float x, float y, Direction previous)
    {
        if (!std::isfinite(x) || !std::isfinite(y) || x*x + y*y <= Deadzone*Deadzone)
            return Direction::None;
        const float ax = std::abs(x), ay = std::abs(y);
        const bool wasHorizontal = previous == Direction::Left || previous == Direction::Right;
        const bool wasVertical = previous == Direction::Up || previous == Direction::Down;
        bool horizontal = ax > ay; // exact diagonal from neutral deterministically chooses vertical
        if (wasHorizontal) horizontal = !(ay > ax * AxisRatio);
        else if (wasVertical) horizontal = ax > ay * AxisRatio;
        return horizontal ? (x > 0 ? Direction::Right : Direction::Left) :
                            (y > 0 ? Direction::Up : Direction::Down);
    }

    Device device{Device::None};
    float rawX{0}, rawY{0}; // game-provided normalized values before OUR deadzone

private:
    std::array<std::uint64_t, 5> keys{};
    std::uint64_t serial{0};
    Direction stick{Direction::None};
};

// Per-attack-segment direction latch. It deliberately has no timer or tick:
// input events decide whether they arrive within the short selection window.
class AttackDirectionState
{
public:
    static constexpr auto kAttackDirectionWindow = std::chrono::milliseconds{200};

    void Begin(Direction a_currentInput, std::chrono::steady_clock::time_point a_now)
    {
        active = true;
        startedAt = a_now;
        direction = a_currentInput;
    }

    bool Update(Direction a_newInput, std::chrono::steady_clock::time_point a_now)
    {
        if (!active || a_newInput == Direction::None) return false;
        if (a_now - startedAt > kAttackDirectionWindow || direction == a_newInput) return false;
        direction = a_newInput;
        return true;
    }

    void End()
    {
        active = false;
        direction = Direction::None;
    }

    [[nodiscard]] bool Active() const { return active; }
    [[nodiscard]] Direction Value() const { return direction; }

private:
    bool active{false};
    std::chrono::steady_clock::time_point startedAt{};
    Direction direction{Direction::None};
};
}
