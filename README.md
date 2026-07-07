# AutoClaude

A native Win32 GUI tool (DirectUI style — single borderless window, custom GDI+ drawing)
that watches your Claude Code session's JSON event transcript and, when it judges the
task is complete, runs a cancelable countdown to a power action (shutdown / restart /
hibernate / lock).

The "current session" in `auto` mode is the **globally newest** `*.jsonl` anywhere under
`%USERPROFILE%\.claude\projects\`, recursively (including `subagents/`). This picks up
whichever Claude Code process is currently writing, regardless of your cwd. Folder mtime
is ignored — only the file mtime counts. The tool can also be pinned to a specific
project (cwd) via the `--project` flag / `projectPath` config.

## Build

```bash
# from a Git-Bash / MSYS shell on Windows:
cd C:\tjf\github\AutoClaude
cmd //c build.bat
# exe is at: build\Release\AutoClaude.exe
```

`build.bat` calls `vcvars64.bat`, runs CMake with the Visual Studio 17 2022 generator
for x64, then `cmake --build build --config Release`. Requires a Visual Studio 2022+
install with the "Desktop development with C++" workload (for `cl.exe`).

## Run

```bash
# default: dry-run safe mode, watches the newest project folder
build/Release/AutoClaude.exe

# watch a specific project (dry-run on)
build/Release/AutoClaude.exe --project "C:\tjf\github\RGBBox" --dry-run

# actually take a power action (CAUTION)
build/Release/AutoClaude.exe --live
```

`--dry-run` (default) means the countdown reaching zero does NOT trigger the real power
action — the status line just reports what would have run. Pass `--live` to actually
shutdown / restart / hibernate / lock.

## How it works

1. A worker thread tails the session JSONL: it opens the file with `FILE_SHARE_WRITE`
   (Claude holds it open), polls file size every 250 ms, reads new bytes, and parses
   only complete lines (delimited by `\n`).
2. Each parsed event is posted via `PostMessage(WM_APP_EVT)` to the UI thread with a
   small `EventSummary` struct (type, end_turn flag, api_error flag).
3. The UI thread drives a small state machine:
   - `Monitoring` — no special behavior; new events refresh the status label.
   - `IdleWaiting` — entered when an `assistant` event with `message.stop_reason ==
     "end_turn"` is observed. Any other event resets back to `Monitoring`. An
     `api_error` system event also aborts.
   - `Countdown` — entered when the idle wait elapses (default 60 s of no events after
     `end_turn`). The 1 s timer ticks down (default 60 s).
   - `Done` — reached when countdown hits 0; the chosen power action is performed.
4. A `WS_POPUP` borderless 480x420 window is drawn entirely with GDI+ (no standard
   child controls): title bar, status line, big countdown readout, four action pills
   (Shutdown / Restart / Hibernate / Lock), a Cancel button (countdown only), and a
   Pause / Resume button. Hit-testing on `WM_LBUTTONDOWN` lets you click the regions.

## Config

`config.json` is created next to the exe on first run, and auto-saved whenever you
click a different action pill. Schema:

```json
{
  "project": "auto",                                    // or "explicit"
  "projectPath": "C:\\tjf\\github\\RGBBox",              // when project=="explicit"
  "idleTimeoutSec": 60,                                  // idle wait after end_turn
  "countdownSec":   60,                                  // power countdown
  "action":        "shutdown",                           // shutdown|restart|hibernate|lock
  "dryRun":        true,                                 // false to actually take action
  "autoSwitchSession": true                              // follow newest session
}
```

`dryRun` defaults to `true` for safety — you have to flip it off (or pass `--live`)
to take a real power action.

## Debug logging

Set `AUTOCALAUDE_DEBUG=1` in the environment before launching and two log files will
be written next to the exe:

- `autoclaude.log` — startup, received events, state transitions, power actions.
- `autoclaude-watcher.log` — watcher thread discovery / tail progress.

Useful for verifying the watcher is reading the right file and for diagnosing event
detection.

## Verification performed

- **Parser** — 11 unit cases covering `assistant end_turn` / `tool_use`, `user`,
  `system api_error` / `turn_duration`, negative case (user text containing the
  literal "end_turn" must NOT trigger the flag), and metadata (`mode`) → all pass.
- **State machine** — 4 scenarios with real `GetTickCount64` timing:
  Monitoring → IdleWaiting → Countdown → Done (full happy path),
  activity during IdleWaiting resets to Monitoring,
  api_error aborts IdleWaiting,
  Cancel during Countdown returns to Monitoring.
- **Live integration** — copied a real 2276-line session, appended a synthetic
  `assistant end_turn`, observed the watcher detect it and the UI state machine
  advance IdleWaiting (3s) → Countdown (3s) → Done (dry-run, no shutdown).

## Layout

```
CMakeLists.txt                        # CMake build
build.bat                             # wraps vcvars64.bat + cmake for one-shot release builds
third_party/nlohmann_json/            # vendored header-only JSON (CMakeLists + json.hpp)
src/
  app.h                               # shared types: EventSummary, State, Action, WM_APP_* messages
  main.cpp                            # WinMain: CLI parse + dispatch
  config.{h,cpp}                      # load/save config.json (via nlohmann::json)
  monitor/
    event_parser.{h,cpp}              # type/subtype/stop_reason extraction (via nlohmann::json)
    transcript_watcher.{h,cpp}        # worker thread: session discovery + file tailing
  core/
    state_machine.{h,cpp}             # Monitoring/IdleWaiting/Countdown/Done transitions
  ui/
    window.{h,cpp}                    # WS_POPUP window, WndProc, regions, paint, hit-test
    drawing.{h,cpp}                   # GDI+ startup + flat dark panel helpers
  power/
    power_action.{h,cpp}              # shutdown/restart/hibernate/lock + SE_SHUTDOWN_NAME
```

## Safety notes

- `InitiateSystemShutdownW` is gated by `EnableShutdownPrivilege()`; it returns a
  failure silently if the privilege isn't granted. Most user tokens can request it;
  if it fails, fall back to launching `cmd.exe shutdown /s /t 0`.
- `dryRun=true` is the default. Verify the countdown reaches zero and the status
  reports the expected action before flipping it to `false`.
- The watcher reads the JSONL with `FILE_SHARE_WRITE`, so Claude can keep writing
  while we tail — this does not interfere with the live session.