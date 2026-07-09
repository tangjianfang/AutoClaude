#pragma once
#include <windows.h>
#include <string>
#include <fstream>
#include <json.hpp>

// Paths under %USERPROFILE%\.claude\autoclaude (overridable via AUTOCLAUDE_DIR),
// shared with the Node hook. Header-only so both window.cpp and any future
// consumer can use it without a new translation unit.

inline std::wstring AcDir() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"AUTOCLAUDE_DIR", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return buf;
    wchar_t up[MAX_PATH] = {};
    GetEnvironmentVariableW(L"USERPROFILE", up, MAX_PATH);
    return std::wstring(up) + L"\\.claude\\autoclaude";
}

inline std::wstring StopFlagPath() { return AcDir() + L"\\stop-flag"; }

// Create dir and all missing parents. Returns true if the dir exists after.
inline bool EnsureDir(const std::wstring& dir) {
    if (dir.empty()) return false;
    if (CreateDirectoryW(dir.c_str(), nullptr) ||
        GetLastError() == ERROR_ALREADY_EXISTS) {
        return true;
    }
    if (GetLastError() == ERROR_PATH_NOT_FOUND) {
        auto slash = dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos && slash > 0) {
            if (EnsureDir(dir.substr(0, slash))) {
                return CreateDirectoryW(dir.c_str(), nullptr) ||
                       GetLastError() == ERROR_ALREADY_EXISTS;
            }
        }
    }
    return false;
}

inline bool WriteStopFlag() {
    if (!EnsureDir(AcDir())) return false;
    std::wstring p = StopFlagPath();
    std::ofstream f(p.c_str(), std::ios::binary | std::ios::trunc);
    return (bool)f;
}

inline std::wstring LoopConfigPath() { return AcDir() + L"\\loop-config.json"; }

// Read the loop-config.json "enabled" flag. Missing/malformed -> false.
inline bool ReadLoopEnabled() {
    try {
        std::ifstream f(LoopConfigPath(), std::ios::binary);
        if (!f) return false;
        nlohmann::json j; f >> j;
        return j.value("enabled", false);
    } catch (...) { return false; }
}

// Flip "enabled" in loop-config.json, preserving other fields. Returns new value.
inline bool ToggleLoopEnabled() {
    nlohmann::json j;
    try {
        std::ifstream f(LoopConfigPath(), std::ios::binary);
        if (f) f >> j;
    } catch (...) { j = nlohmann::json::object(); }
    if (!j.is_object()) j = nlohmann::json::object();
    bool nv = !j.value("enabled", false);
    j["enabled"] = nv;
    EnsureDir(AcDir());
    std::ofstream o(LoopConfigPath(), std::ios::binary | std::ios::trunc);
    o << j.dump(2);
    return nv;
}
