#pragma once
#include <string>

struct Config {
    // "auto" means watch whichever project folder has the newest activity.
    std::wstring project = L"auto";
    // Explicit project cwd to slugify when project != "auto".
    std::wstring projectPath;
    int  idleTimeoutSec   = 60;
    int  countdownSec     = 60;
    int  activeWindowSec  = 60;    // how recent an mtime must be to count as "active"
    int  action           = 0;     // Action::Shutdown
    bool dryRun           = true;
    bool autoSwitchSession = true;
    // Auto-continue loop: cap shown as the "loop N/M" denominator in the UI.
    // The authoritative loop config lives in ~/.claude/autoclaude/loop-config.json;
    // this is a display-only mirror.
    int  maxAutoTurns     = 10;

    // Load from <exeDir>\config.json; missing file -> defaults (no error).
    void Load(const std::wstring& exeDir);
    // Save to <exeDir>\config.json.
    void Save(const std::wstring& exeDir) const;
};

// Resolve the directory holding the .exe (for config.json placement).
std::wstring ExeDir();