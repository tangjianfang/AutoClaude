#include "transcript_watcher.h"
#include "event_parser.h"
#include <windows.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstdarg>
#include <shlobj.h>

namespace {
bool DebugOn() {
    char v[16] = {};
    GetEnvironmentVariableA("AUTOCALAUDE_DEBUG", v, sizeof(v));
    return v[0] == '1' || v[0] == 'y' || v[0] == 'Y';
}
void WLog(const wchar_t* fmt, ...) {
    if (!DebugOn()) return;
    static std::wstring logPath;
    if (logPath.empty()) {
        wchar_t buf[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        std::wstring p = buf;
        logPath = p.substr(0, p.find_last_of(L"\\/") + 1) + L"autoclaude-watcher.log";
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, logPath.c_str(), L"a") != 0 || !f) return;
    va_list ap; va_start(ap, fmt);
    vfwprintf(f, fmt, ap);
    va_end(ap);
    fwprintf(f, L"\n");
    fclose(f);
}
} // namespace

namespace {

// Return system "now" as a FILETIME for mtime comparisons.
FILETIME NowAsFileTime() {
    FILETIME ft{};
    SYSTEMTIME st{};
    GetSystemTime(&st);
    SystemTimeToFileTime(&st, &ft);
    return ft;
}

// True iff `when` is more than `seconds` before `now`.
bool OlderThan(const FILETIME& now, const FILETIME& when, DWORD seconds) {
    ULARGE_INTEGER a{}, b{};
    a.LowPart = now.dwLowDateTime;  a.HighPart = now.dwHighDateTime;
    b.LowPart = when.dwLowDateTime; b.HighPart = when.dwHighDateTime;
    // FILETIME is in 100ns units; 1 second = 10,000,000 of them.
    LONGLONG diff = (LONGLONG)(a.QuadPart - b.QuadPart);
    return diff > (LONGLONG)seconds * 10000000LL;
}

std::wstring FolderName(const std::wstring& path) {
    auto slash = path.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? path : path.substr(slash + 1);
}

} // namespace

std::wstring TranscriptWatcher::ProjectsRoot() {
    wchar_t* home = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &home))) {
        std::wstring p = home;
        CoTaskMemFree(home);
        return p + L"\\.claude\\projects";
    }
    return L"";
}

void TranscriptWatcher::AnnotateProject(Track& t) {
    // Project name = immediate parent folder unless it's "subagents", in
    // which case step one more level up so the UI shows the real project
    // (e.g. "C--tjf-github-BLECode") plus an implicit "agent" prefix via
    // the file name. TrackInfo carries an isSubagent flag for the UI.
    //
    // path looks like: "...\<project>\subagents\agent-a2....jsonl"
    // or              "...\<project>\<uuid>.jsonl"
    auto lastSlash = t.path.find_last_of(L"\\/");
    if (lastSlash == std::wstring::npos) {
        t.projectName = L"";
        t.shortId = L"";
        return;
    }
    std::wstring parent = t.path.substr(0, lastSlash);
    std::wstring file = t.path.substr(lastSlash + 1);

    // Strip .jsonl extension for the short id.
    auto dot = file.find_last_of(L'.');
    std::wstring stem = (dot == std::wstring::npos) ? file : file.substr(0, dot);
    if (stem.size() > 8) {
        t.shortId = stem.substr(0, 8) + L"…";
    } else {
        t.shortId = stem;
    }

    auto secondSlash = parent.find_last_of(L"\\/");
    std::wstring folder = (secondSlash == std::wstring::npos) ? parent
                                                              : parent.substr(secondSlash + 1);
    if (folder == L"subagents") {
        t.isSubagent = true;
        // The real project lives one level up.
        if (secondSlash != std::wstring::npos) {
            std::wstring upOne = parent.substr(0, secondSlash);
            auto thirdSlash = upOne.find_last_of(L"\\/");
            t.projectName = (thirdSlash == std::wstring::npos)
                                ? upOne
                                : upOne.substr(thirdSlash + 1);
        } else {
            t.projectName = L"";
        }
    } else {
        t.isSubagent = false;
        t.projectName = folder;
    }
}

std::vector<TranscriptWatcher::Track>
TranscriptWatcher::DiscoverActive(const std::wstring& root, int windowSec) {
    std::vector<Track> out;
    if (root.empty()) return out;

    // First pass: collect (path, mtime) pairs.
    struct Entry { std::wstring path; FILETIME ft; };
    std::vector<Entry> entries;
    std::vector<std::wstring> stack;
    stack.push_back(root);
    while (!stack.empty()) {
        std::wstring cur = stack.back(); stack.pop_back();
        std::wstring pattern = cur + L"\\*";
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            // Skip hidden / system. Keep "subagents" because that's where
            // Claude Code writes per-agent transcripts.
            if (name == L".git") continue;
            std::wstring full = cur + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                stack.push_back(full);
            } else {
                const size_t L = name.size();
                if (L >= 6 && _wcsicmp(name.c_str() + (L - 6), L".jsonl") == 0) {
                    entries.push_back({full, fd.ftLastWriteTime});
                }
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    // Filter by recency.
    FILETIME now = NowAsFileTime();
    for (auto& e : entries) {
        if (windowSec <= 0 || !OlderThan(now, e.ft, (DWORD)windowSec)) {
            Track t;
            t.path = e.path;
            AnnotateProject(t);
            out.push_back(std::move(t));
        }
    }

    // Sort newest-mtime first so UI primary = index 0. Query each file's
    // mtime once (cheap for a few hundred paths).
    auto mtimeOf = [](const std::wstring& path) -> FILETIME {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
            return fad.ftLastWriteTime;
        }
        FILETIME z{};
        return z;
    };
    std::vector<FILETIME> mtimes;
    mtimes.reserve(out.size());
    for (auto& t : out) mtimes.push_back(mtimeOf(t.path));
    std::vector<size_t> idx(out.size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
    std::sort(idx.begin(), idx.end(),
              [&](size_t i, size_t j) {
                  return CompareFileTime(&mtimes[i], &mtimes[j]) > 0;
              });
    std::vector<Track> sorted;
    sorted.reserve(out.size());
    for (size_t k : idx) sorted.push_back(std::move(out[k]));
    return sorted;
}

bool TranscriptWatcher::ReadOnce(Track& t) {
    HANDLE h = CreateFileW(t.path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    bool ok = false;
    if (GetFileSizeEx(h, &size)) {
        LONGLONG sz = size.QuadPart;
        if (sz < t.offset) { t.offset = 0; t.accum.clear(); }
        if (sz > t.offset) {
            LARGE_INTEGER move; move.QuadPart = t.offset;
            SetFilePointerEx(h, move, nullptr, FILE_BEGIN);
            DWORD want = (DWORD)(sz - t.offset);
            std::string buf(want, '\0');
            DWORD got = 0;
            if (ReadFile(h, buf.data(), want, &got, nullptr) && got > 0) {
                t.accum.append(buf.data(), got);
                t.offset += got;
                size_t start = 0;
                while (true) {
                    size_t nl = t.accum.find('\n', start);
                    if (nl == std::string::npos) break;
                    std::string line = t.accum.substr(start, nl - start);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (!line.empty()) {
                        EventSummary e = ParseEvent(line);
                        auto* p = new SessionEvent{t.path, e};
                        WLog(L"[read] %s evt=%d end_turn=%d",
                             FolderName(t.path).c_str(),
                             (int)e.type, e.is_end_turn ? 1 : 0);
                        PostMessageW(hwnd_, WM_APP_EVT, 0,
                                     reinterpret_cast<LPARAM>(p));
                    }
                    start = nl + 1;
                }
                t.accum.erase(0, start);
                ok = got > 0;
            }
        } else {
            ok = true; // nothing new but the file is openable
        }
    }
    CloseHandle(h);
    return ok;
}

void TranscriptWatcher::Start(HWND hwnd, const Config& cfg) {
    hwnd_ = hwnd;
    cfg_ = cfg;
    InitializeCriticalSection(&dataLock_);
    stop_ = false;
    th_ = std::thread([this] { Run(); });
}

void TranscriptWatcher::Stop() {
    stop_ = true;
    if (th_.joinable()) th_.join();
}

TranscriptWatcher::~TranscriptWatcher() {
    Stop();
    DeleteCriticalSection(&dataLock_);
}

void TranscriptWatcher::UpdateConfig(const Config& cfg) {
    EnterCriticalSection(&dataLock_);
    cfg_ = cfg;
    LeaveCriticalSection(&dataLock_);
}

void TranscriptWatcher::Run() {
    WLog(L"[watcher] thread started (multi-session)");
    std::vector<Track> tracks;
    DWORD lastDiscover = 0;
    bool announcedEmpty = true;

    while (!stop_) {
        if (paused_) { Sleep(500); continue; }

        Config c;
        {
            EnterCriticalSection(&dataLock_);
            c = cfg_;
            LeaveCriticalSection(&dataLock_);
        }

        // Refresh the active set every 5s (or on the very first tick).
        DWORD now = GetTickCount();
        if (tracks.empty() || (now - lastDiscover) > 5000) {
            lastDiscover = now;
            std::vector<Track> fresh =
                DiscoverActive(ProjectsRoot(), c.activeWindowSec);

            // Reuse offsets/accum buffers for known paths; reset for new ones.
            for (auto& f : fresh) {
                for (auto& existing : tracks) {
                    if (_wcsicmp(existing.path.c_str(), f.path.c_str()) == 0) {
                        f.offset = existing.offset;
                        f.accum = std::move(existing.accum);
                        break;
                    }
                }
            }
            tracks = std::move(fresh);

            if (tracks.empty() && !announcedEmpty) {
                announcedEmpty = true;
                WLog(L"[discover] no active sessions");
                PostMessageW(hwnd_, WM_APP_SESSIONS, 0,
                             reinterpret_cast<LPARAM>(new std::vector<TrackInfo>));
            } else if (!tracks.empty()) {
                announcedEmpty = false;
                auto* snap = new std::vector<TrackInfo>;
                snap->reserve(tracks.size());
                for (auto& t : tracks) {
                    TrackInfo ti;
                    ti.path = t.path;
                    ti.projectName = t.projectName;
                    ti.shortId = t.shortId;
                    ti.isSubagent = t.isSubagent;
                    snap->push_back(std::move(ti));
                }
                WLog(L"[discover] %zu active session(s)", snap->size());
                PostMessageW(hwnd_, WM_APP_SESSIONS, 0,
                             reinterpret_cast<LPARAM>(snap));
            }
        }

        if (tracks.empty()) { Sleep(1000); continue; }

        // Round-robin: each track gets exactly one ReadOnce per cycle.
        // Tracks whose file fails to open (rotation, deletion) keep their
        // offset/accum across retries; they'll be removed on the next
        // discover pass if their mtime drops out of the active window.
        for (auto& t : tracks) {
            if (stop_) break;
            ReadOnce(t);
        }
        Sleep(250);
    }
}
