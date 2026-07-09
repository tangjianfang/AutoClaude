// Shared application types.
#pragma once
#include <windows.h>
#include <string>
#include <vector>

// Event category posted by the watcher thread to the UI thread.
enum class EvtType { Other, User, Assistant, System };

// What the UI thread learns about each new transcript line.
struct EventSummary {
    EvtType type = EvtType::Other;
    bool is_end_turn = false;   // assistant message.stop_reason == "end_turn"
    bool is_api_error = false;  // system subtype == "api_error"
    LONGLONG ts_ms = 0;         // wall-clock time of parse (GetTickCount64 ms)

    // Token usage from message.usage — only set on assistant events with a
    // non-zero usage block. Cumulative per call (not per line).
    unsigned int tokInput            = 0;
    unsigned int tokOutput           = 0;
    unsigned int tokCacheRead        = 0;
    unsigned int tokCacheCreation    = 0;
};

// Selected power action.
enum class Action { Shutdown, Restart, Hibernate, Lock };

// Monitor / idle / countdown states.
enum class State { Monitoring, IdleWaiting, Countdown, Done };

// Windows messages posted by the watcher thread. LPARAM always owns a heap
// pointer; the UI thread must `delete` it after consuming.
#define WM_APP_EVT     (WM_APP + 1)   // LPARAM = SessionEvent* (path + EventSummary)
#define WM_APP_SESSION (WM_APP + 2)   // LPARAM = nullptr (legacy single-session switch)
#define WM_APP_SESSIONS (WM_APP + 3)  // LPARAM = std::vector<TrackInfo>* snapshot of active tracks
#define WM_APP_LOOPSTATUS (WM_APP + 4)  // LPARAM = LoopStatus* (UI thread deletes)

// Auto-continue loop status for one session, pushed by the named-pipe sensor
// (statusline.js broadcast). The UI matches it to a session row by sessionId.
struct LoopStatus {
    std::wstring sessionId;
    std::wstring cwd;
    bool loopEnabled = false;
    int  autoTurns = 0;
};

// Watcher → UI event bundle. The path lets the UI route to the correct row
// even if the list reorders between the post and the UI receive.
struct SessionEvent {
    std::wstring path;
    EventSummary e;
};

// Lightweight summary of an active transcript file. Posted in bulk to the UI
// when the set of active sessions changes (newly discovered / aged out).
struct TrackInfo {
    std::wstring path;
    std::wstring projectName;  // e.g. "C--tjf-github-AutoClaude"
    std::wstring shortId;      // e.g. "42cfc7e0…"
    bool isSubagent = false;
};