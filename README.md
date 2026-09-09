# DawnwalkerCombat — milestone 1 (0.1.0)

Private SKSE input-direction prototype for **Skyrim SE/AE Steam 1.6.1170**.
This is an input/animation-variable milestone, not a complete combat system.
No block detection, stamina, attack-direction lock, movement lock or NPC AI.

## Output and rules

The player graph integer `DW_InputDirection` is `0` neutral, `1` up, `2` right,
`3` down, `4` left. The package's BDI JSON creates this variable; the DLL updates
it and checks graph read/write results. A DLL alone cannot create an arbitrary
new graph variable through `SetGraphVariableInt`.

- Physical W/D/S/A keys map to 1/2/3/4. Among held direction keys, the most recent
  **press** wins (including opposite pairs). Repeated hold events do not count as
  presses. Releasing the winner selects the remaining most recent held key;
  releasing all keys writes 0 immediately on the input event.
- Only the left stick is read. Raw x/y in the log are Skyrim's normalized event
  values before this plugin's deadzone, not hardware ADC values. Skyrim/Steam
  Input may already have processed the axes.
- Radial deadzone is 0.24. Outside it, the dominant absolute axis selects a
  cardinal direction. Exact diagonal from neutral selects the vertical axis.
  Retain the previous axis until the other axis exceeds it by a ratio of
  1.191754 (approximately 5 degrees past 45 degrees). No diagonal output exists.
  Returning inside the deadzone writes 0 immediately; no neutral delay.
- A fresh keyboard press or a fresh stick direction takes device ownership.
  A steady held stick cannot repeatedly steal ownership from a keyboard press.
  Releasing the active device does not resurrect stale input on the other device.
  Inputs must be delivered by Skyrim; Steam Input keyboard emulation is logged
  as keyboard. This plugin does not enable simultaneous input in the engine.
- Input menus (including unpaused inventory/dialogue menus), lost foreground
  focus, pre/post-load/new-game, and gamepad disconnection reset all direction
  state. Held keyboard repeat events do not restore a key after reset; release
  and press again. A new stick event after returning to gameplay can select its
  current direction. HUD overlays that do not consume input do not block it.
- Input sinks register at SKSE `kInputLoaded`. At `kDataLoaded`, a task on the
  Skyrim window thread installs a `WM_ACTIVATEAPP` subclass. On focus loss its
  callback queues one game-thread reset; it never polls Windows input or touches
  game objects directly. Installation verifies both process and window thread ID.
  Direction writes queue only when the output changes; they are not driven by a
  fixed task loop.
- Logs are state-based: at most 10 input lines/sec during fast changes, at most
  2 raw-axis-only lines/sec, no static per-frame spam. Reset and graph availability
  transitions are separately logged. Logs are not a full event trace.

## Runtime requirements

1. Skyrim 1.6.1170 and its matching SKSE64 build (2.2.6).
2. Address Library for SKSE Plugins, AE package containing 1.6.1170 data.
3. Behavior Data Injector with the Universal Support version compatible with
   1.6.1170. When both BDI mods are installed, Universal Support must win the DLL
   conflict. The user's Frostbound installation already contains both mod folders;
   enabled profile/load order and actual runtime loading still require checking.
4. OAR for animation conditions; BFCO/animations remain the user's existing setup.

The DLL deliberately rejects other runtimes. No ESP or Papyrus scripts needed.
No behavior regeneration is required for this BDI variable.

## Cloud build (no local C++ environment needed)

Push to `main` or open **Actions → Windows DLL → Run workflow**. Windows Server
2022 builds an x64 Release DLL, runs the shared C++ direction tests and packages
the mod. Download the **DawnwalkerCombat-0.1.0-MO2** artifact from the successful
run, extract GitHub's outer artifact ZIP, then install the inner
`DawnwalkerCombat-0.1.0-MO2.zip` in MO2.

The CMake preset requires CMake 3.28+, consumes CommonLibSSE-NG tag v3.7.0 and vcpkg tag 2023.10.19,
with a static MSVC runtime. The repository contains no game files or credentials.
Artifacts expire after 30 days: rerun the workflow for a fresh copy. A successful
build proves compilation and portable tests, not in-game compatibility.

## Installed layout (MO2 mod root is the virtual Data root)

```text
DawnwalkerCombat - Input/
  SKSE/Plugins/DawnwalkerCombat.dll
  SKSE/Plugins/BehaviorDataInjector/DawnwalkerCombat_BDI.json
  docs/DawnwalkerCombat/...
```

Keep this a separate enabled MO2 mod for easy rollback. This archive does not
replace animations or change existing OAR conditions. Install the separately
prepared OAR config overlay only after verifying the graph variable in game.

## Minimum game test

1. Launch SKSE through MO2 and load a disposable test save in a safe area.
2. Check `Documents/My Games/Skyrim Special Edition/SKSE/DawnwalkerCombat.log`.
   Require `0.1.0 loaded`, `Input sinks active`, and `graph variable verified`.
   `unavailable/write failed` means fix BDI/config before changing OAR conditions.
3. Tap W/D/S/A: expect 1/2/3/4, release all: 0. Hold W, press D: 2; release D
   while W stays held: 1. Hold D, press W: 1. All released: 0.
4. Test gamepad in gameplay, with Skyrim gamepad input enabled. Left stick cardinal
   directions must log 1/2/3/4. Sweep across diagonals slowly: no 5+ output or
   rapid toggling near the boundary. Recenter: 0. Right stick alone: no change.
5. Hold a direction then open inventory/pause/dialogue, Alt-Tab, load a save,
   or disconnect the controller. Each must clear input. Return/reconnect and
   confirm fresh input works. Inspect the log after closing the game if needed.
6. For an independent BDI check, select the player with `prid 14` in the console,
   then use BDI's `DGV DW_InputDirection i`. Opening the console intentionally
   resets input, so seeing **0 in the console is expected**, not a direction test.
7. Only after the above passes, migrate OAR `CompareValues`: Value A graph
   variable `DW_InputDirection`, type `Int`, comparison `==`, Value B 1/2/3/4
   (0 for neutral). Preserve all non-direction conditions, priority and blend
   settings. Stances stay interruptible; BFCO attacks stay non-interruptible.

The first game run, controller mapping, window-focus reset, and actual OAR
transition timing must be validated on the user's mod stack. Cloud CI cannot
simulate Skyrim. Focus loss uses a game-window `WM_ACTIVATEAPP` subclass; the
callback queues a game-thread reset and never polls Windows input. Roll back by
disabling the input mod and the optional OAR overlay.

## Source layout / license

`src/Direction.h` holds the portable state machine; `src/Plugin.cpp` adapts Skyrim
input, menus and SKSE lifecycle events. `tests/DirectionTests.cpp` covers direction
rules, release semantics, mixed input, deadzone, hysteresis and invalid samples.
The project is MIT licensed; see LICENSE.md and THIRD_PARTY_NOTICES.md. DMK
(Direction-Movement) is an MIT-authorized reference for input and focus handling;
its assets and full source tree are not included.
