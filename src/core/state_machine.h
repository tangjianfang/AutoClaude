#pragma once
#include "../app.h"

// Pure state-machine helper. Owns no threads; the UI thread drives it from
// WM_APP_EVT and WM_TIMER. Time is GetTickCount64() ms (monotonic).
struct StateMachine {
    State state = State::Monitoring;
    LONGLONG idleDeadlineMs = 0;   // when IdleWaiting elapses
    int      countdownRemaining = 0;
    LONGLONG lastEventMs = 0;
    bool     lastEventWasEndTurn = false;

    // Called for every new parsed event. `idleSec` is the configured idle timeout.
    void OnEvent(const EventSummary& e, int idleSec);

    // Called every WM_TIMER (1s). Returns the action to perform if the
    // countdown just hit zero, or `Other` (meaning: none) otherwise.
    // `countdownSec` is the configured countdown length.
    // Returns true when the countdown reached zero this tick.
    bool OnTimer(int countdownSec);

    // User clicked Cancel.
    void Cancel();

    const wchar_t* StateName() const;
};