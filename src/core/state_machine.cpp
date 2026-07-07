#include "state_machine.h"

void StateMachine::OnEvent(const EventSummary& e, int idleSec) {
    lastEventMs = e.ts_ms;
    lastEventWasEndTurn = e.is_end_turn;

    if (e.is_api_error) {
        // Interrupted/errored — treat as not-complete, abort any idle wait.
        state = State::Monitoring;
        idleDeadlineMs = 0;
        return;
    }

    if (e.is_end_turn) {
        // Claude finished its turn; arm the idle wait.
        state = State::IdleWaiting;
        idleDeadlineMs = e.ts_ms + (LONGLONG)idleSec * 1000;
        return;
    }

    // Any other activity: if we were waiting idle, the user/Claude resumed.
    if (state == State::IdleWaiting) {
        state = State::Monitoring;
        idleDeadlineMs = 0;
    }
}

bool StateMachine::OnTimer(int countdownSec) {
    LONGLONG now = (LONGLONG)GetTickCount64();

    if (state == State::IdleWaiting && now >= idleDeadlineMs) {
        state = State::Countdown;
        countdownRemaining = countdownSec;
    }

    if (state == State::Countdown) {
        --countdownRemaining;
        if (countdownRemaining <= 0) {
            state = State::Done;
            countdownRemaining = 0;
            return true;  // perform the power action now
        }
    }
    return false;
}

void StateMachine::Cancel() {
    state = State::Monitoring;
    idleDeadlineMs = 0;
    countdownRemaining = 0;
}

const wchar_t* StateMachine::StateName() const {
    switch (state) {
        case State::Monitoring:  return L"Monitoring";
        case State::IdleWaiting:  return L"Idle waiting";
        case State::Countdown:    return L"Countdown";
        case State::Done:         return L"Done";
    }
    return L"?";
}