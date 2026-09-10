#include "Direction.h"
#include <cstdlib>
#include <chrono>
#include <iostream>
#include <limits>

using DW::Direction;
using DW::InputState;
using DW::AttackDirectionState;
using DW::MapAttackDirection;
int checks = 0;
void Check(bool ok, const char* label)
{
    ++checks;
    if (!ok) { std::cerr << "FAILED: " << label << '\n'; std::exit(1); }
}
int main()
{
    using enum Direction;
    InputState s;
    Check(s.Value() == None, "initial neutral");
    for (auto d : {Up, Right, Down, Left}) {
        s.Key(d, true); Check(s.Value() == d, "WASD press");
        s.Key(d, false); Check(s.Value() == None, "WASD release");
    }
    s.Key(Up, true); s.Key(Right, true);
    Check(s.Value() == Right, "adjacent last press");
    s.Key(Up, true); Check(s.Value() == Right, "held repeat does not steal");
    s.Key(Right, false); Check(s.Value() == Up, "release falls back to held key");
    s.Key(Down, true); Check(s.Value() == Down, "opposite keys last press");
    s.Reset(); Check(s.Value() == None, "reset clears all held keys");
    s.Key(Down, false); Check(s.Value() == None, "late release after reset");
    s.Stick(0, 1); Check(s.Value() == Up, "stick up");
    s.Stick(1, 0); Check(s.Value() == Right, "stick right");
    s.Stick(0, -1); Check(s.Value() == Down, "stick down");
    s.Stick(-1, 0); Check(s.Value() == Left, "stick left");
    s.Stick(0, 0); Check(s.Value() == None, "stick recenter immediate");
    s.Stick(0.24F, 0); Check(s.Value() == None, "deadzone inclusive");
    s.Stick(0.241F, 0); Check(s.Value() == Right, "just outside deadzone");
    s.Stick(0.1F, 0.1F); Check(s.Value() == None, "radial center noise");
    s.Stick(0.18F, 0.18F); Check(s.Value() == Up, "radial diagonal activation");
    s.Reset(); s.Stick(0, 1);
    s.Stick(0.71F, 0.70F); Check(s.Value() == Up, "vertical diagonal hysteresis");
    s.Stick(0.80F, 0.60F); Check(s.Value() == Right, "cross vertical hysteresis");
    s.Stick(0.70F, 0.71F); Check(s.Value() == Right, "horizontal diagonal hysteresis");
    s.Stick(0.60F, 0.80F); Check(s.Value() == Up, "cross horizontal hysteresis");
    s.Stick(0, -1); Check(s.Value() == Down, "axis sign flip");
    s.Key(Left, true); Check(s.Value() == Left, "keyboard takes device");
    s.Stick(0, -0.95F); Check(s.Value() == Left, "steady stick does not steal");
    s.Key(Left, false); Check(s.Value() == None, "no stale other-device fallback");
    s.Stick(1, 0); Check(s.Value() == Right, "new stick direction takes device");
    s.Reset(); Check(s.Value() == None && s.rawX == 0 && s.rawY == 0, "disconnect/load/focus reset");
    s.Stick(std::numeric_limits<float>::quiet_NaN(), 0);
    Check(s.Value() == None, "NaN rejected");
    s.Stick(0, std::numeric_limits<float>::infinity());
    Check(s.Value() == None, "infinite rejected");

    Check(MapAttackDirection(None, false, false) == None, "attack mapping preserves neutral");
    Check(MapAttackDirection(Left, false, false) == Left &&
          MapAttackDirection(Right, false, false) == Right &&
          MapAttackDirection(Up, false, false) == Up &&
          MapAttackDirection(Down, false, false) == Down,
        "default attack mapping preserves all directions");
    Check(MapAttackDirection(Left, true, false) == Right &&
          MapAttackDirection(Right, true, false) == Left &&
          MapAttackDirection(Up, true, false) == Up &&
          MapAttackDirection(Down, true, false) == Down,
        "horizontal attack mapping swaps only left right");
    Check(MapAttackDirection(Left, false, true) == Left &&
          MapAttackDirection(Right, false, true) == Right &&
          MapAttackDirection(Up, false, true) == Down &&
          MapAttackDirection(Down, false, true) == Up,
        "vertical attack mapping swaps only up down");
    Check(MapAttackDirection(Left, true, true) == Right &&
          MapAttackDirection(Right, true, true) == Left &&
          MapAttackDirection(Up, true, true) == Down &&
          MapAttackDirection(Down, true, true) == Up,
        "combined attack mapping swaps both axes");

    AttackDirectionState attack;
    const auto start = std::chrono::steady_clock::time_point{};
    attack.Begin(Left, start);
    Check(attack.Active() && attack.Value() == Left, "attack segment snapshots input");
    Check(!attack.Update(None, start + std::chrono::milliseconds{50}) && attack.Value() == Left,
        "neutral does not clear attack direction");
    Check(attack.Update(Right, start + std::chrono::milliseconds{200}) && attack.Value() == Right,
        "attack direction changes at window boundary");
    Check(!attack.Update(Left, start + std::chrono::milliseconds{201}) && attack.Value() == Right,
        "attack direction freezes after window");
    attack.End();
    Check(!attack.Active() && attack.Value() == None, "attack end clears direction");
    attack.Begin(None, start + std::chrono::seconds{1});
    Check(attack.Value() == None, "new neutral segment does not inherit direction");
    Check(attack.Update(Up, start + std::chrono::milliseconds{1100}) && attack.Value() == Up,
        "neutral segment accepts direction in window");
    attack.End();
    attack.SetUpdateWindow(std::chrono::milliseconds{0});
    attack.Begin(Left, start + std::chrono::seconds{2});
    Check(!attack.Update(Right, start + std::chrono::seconds{2}) && attack.Value() == Left,
        "zero window keeps snapshot only");
    for (int deg = 0; deg < 360; ++deg) {
        const float a = deg * 3.14159265F / 180;
        const auto v = InputState::Classify(std::cos(a), std::sin(a), None);
        Check(v >= Up && v <= Left, "full circle has only four directions");
    }
    std::cout << checks << " input checks passed\n";
}
