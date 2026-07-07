#pragma once
#include "../app.h"
#include "../config.h"
#include <atomic>
#include <thread>
#include <string>
#include <vector>

class TranscriptWatcher {
public:
    void Start(HWND hwnd, const Config& cfg);
    void Stop();
    ~TranscriptWatcher();

    // Live updates from the UI thread.
    void SetPaused(bool p) { paused_ = p; }
    void UpdateConfig(const Config& cfg);

private:
    void Run();

    // One per discovered session file.
    struct Track {
        std::wstring path;            // full path to .jsonl
        std::wstring projectName;     // e.g. "C--tjf-github-AutoClaude" (immediate folder)
        std::wstring shortId;         // e.g. "42cfc7e0…" — first 8 chars of the file stem
        bool isSubagent = false;
        LONGLONG offset = 0;          // byte cursor into the file
        std::string accum;            // partial-line buffer across reads
    };

    // Discovery: walk the projects tree, collect every .jsonl whose mtime
    // is within `cfg.activeWindowSec` of now. Returns tracks sorted
    // newest-mtime first.
    static std::vector<Track> DiscoverActive(const std::wstring& root, int windowSec);
    // Parse a single file's `\\folder\\name` into projectName + shortId,
    // marking isSubagent if the containing folder is named `subagents`.
    static void AnnotateProject(Track& t);
    // Returns %USERPROFILE%\\.claude\\projects, or empty on failure.
    static std::wstring ProjectsRoot();

    // Per-tick IO for one track: opens, reads new bytes, parses lines,
    // PostMessages a SessionEvent* per non-empty line.
    bool ReadOnce(Track& t);

    HWND hwnd_ = nullptr;
    std::thread th_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> paused_{false};

    // Watcher config snapshot, swapped under dataLock_.
    Config cfg_;
    CRITICAL_SECTION dataLock_;
};