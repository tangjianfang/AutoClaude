#include "status_pipe.h"
#include <windows.h>
#include <string>
#include <json.hpp>

namespace {
const wchar_t* kPipeName = L"\\\\.\\pipe\\autoclaude-status";

std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}
} // namespace

void StatusPipeServer::Start(HWND hwnd) {
    hwnd_ = hwnd;
    stop_.store(false);
    th_ = std::thread([this] { Run(); });
}

void StatusPipeServer::Stop() {
    stop_.store(true);
    // Wake a blocked ConnectNamedPipe by self-connecting once.
    HANDLE c = CreateFileW(kPipeName, GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (c != INVALID_HANDLE_VALUE) CloseHandle(c);
    if (th_.joinable()) th_.join();
}

StatusPipeServer::~StatusPipeServer() {
    if (th_.joinable()) Stop();
}

void StatusPipeServer::EmitFrame(const std::string& line) {
    if (line.empty()) return;
    try {
        nlohmann::json j = nlohmann::json::parse(line);
        if (!j.is_object()) return;
        auto* ls = new LoopStatus();
        ls->sessionId   = ToWide(j.value("sessionId", std::string()));
        ls->cwd         = ToWide(j.value("cwd", std::string()));
        ls->loopEnabled = j.value("loopEnabled", false);
        ls->autoTurns   = j.value("autoTurns", 0);
        if (!PostMessageW(hwnd_, WM_APP_LOOPSTATUS, 0, (LPARAM)ls)) {
            delete ls;
        }
    } catch (const nlohmann::json::exception&) {
        // Ignore malformed frames.
    }
}

void StatusPipeServer::Run() {
    while (!stop_.load()) {
        HANDLE p = CreateNamedPipeW(
            kPipeName, PIPE_ACCESS_INBOUND,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES, 0, 4096, 0, nullptr);
        if (p == INVALID_HANDLE_VALUE) { Sleep(200); continue; }

        BOOL ok = ConnectNamedPipe(p, nullptr)
                    ? TRUE
                    : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (stop_.load()) { CloseHandle(p); break; }

        if (ok) {
            std::string acc;
            char buf[1024];
            DWORD n = 0;
            while (ReadFile(p, buf, sizeof(buf), &n, nullptr) && n > 0) {
                acc.append(buf, n);
                size_t nl;
                while ((nl = acc.find('\n')) != std::string::npos) {
                    EmitFrame(acc.substr(0, nl));
                    acc.erase(0, nl + 1);
                }
            }
            // Flush a trailing line without newline (client end() without \n).
            if (!acc.empty()) EmitFrame(acc);
        }

        DisconnectNamedPipe(p);
        CloseHandle(p);
    }
}
