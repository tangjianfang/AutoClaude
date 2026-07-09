#pragma once
#include "../app.h"
#include <atomic>
#include <thread>

// Named-pipe server that receives one-line JSON status frames broadcast by the
// statusline sensor and posts them to the UI thread as WM_APP_LOOPSTATUS.
// One-way (inbound). Best-effort: parse failures on a line are ignored.
class StatusPipeServer {
public:
    void Start(HWND hwnd);
    void Stop();
    ~StatusPipeServer();

private:
    void Run();
    void EmitFrame(const std::string& line);

    HWND hwnd_ = nullptr;
    std::thread th_;
    std::atomic<bool> stop_{false};
};
