#include "window.h"
#include "drawing.h"
#include "../power/power_action.h"
#include <windows.h>
#include <windowsx.h>
#include <objidl.h>
#include <gdiplus.h>
#include <string>
#include <memory>
#include <vector>
#include <cstdio>

namespace {
bool DebugOn() {
    char v[16] = {};
    GetEnvironmentVariableA("AUTOCALAUDE_DEBUG", v, sizeof(v));
    return v[0] == '1' || v[0] == 'y' || v[0] == 'Y';
}
void DLog(const wchar_t* fmt, ...) {
    if (!DebugOn()) return;
    static std::wstring logPath;
    if (logPath.empty()) {
        wchar_t buf[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, buf, MAX_PATH);
        std::wstring p = buf;
        logPath = p.substr(0, p.find_last_of(L"\\/") + 1) + L"autoclaude.log";
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

constexpr int WIN_W_DEFAULT = 480;
constexpr int WIN_H_DEFAULT = 420;
constexpr int WIN_W_MIN     = 360;
constexpr int WIN_H_MIN     = 280;

struct Region {
    int x, y, w, h;
    bool Contains(int px, int py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
    Gdiplus::RectF Rect() const { return Gdiplus::RectF((float)x, (float)y, (float)w, (float)h); }
};

// Per-session row in the UI list. Mirrors a TrackInfo from the watcher plus
// per-row accumulation (last event label, ts, total events parsed).
struct SessionRow {
    std::wstring path;
    std::wstring projectName;
    std::wstring shortId;
    bool isSubagent = false;
    int eventsCount = 0;
    LONGLONG lastEventTsMs = 0;
    LONGLONG lastAssistantMs = 0;   // ts of last assistant event; used for "Claude is working" indicator
    std::wstring lastEventLabel;
};

constexpr int ROW_H = 18;
constexpr int ROW_GAP = 2;
constexpr int LIST_X = 20;
constexpr int LIST_Y = 78;

// Layout is recomputed whenever the window resizes. Title/status are pinned
// to the top, pills/buttons/footer to the bottom; the list area stretches
// in between. The grip lives in the bottom-right corner as a custom resize
// handle (WS_POPUP doesn't get a system frame).
struct Layout {
    Region title;
    Region status;
    Region list;
    Region pills[4];   // Shutdown / Restart / Hibernate / Lock
    Region cancelBtn;
    Region startBtn;
    Region closeBtn;
    Region grip;
    int  listVisibleRows = 7;     // recomputed in Recompute()
    int  winW = WIN_W_DEFAULT;
    int  winH = WIN_H_DEFAULT;

    void Recompute(int w, int h) {
        winW = w; winH = h;
        title   = {0, 0, w, 36};
        status  = {20, 44, w - 40, 26};
        closeBtn = {w - 36, 6, 30, 24};

        const int pillsH = 48;
        const int btnH   = 44;
        const int footerH = 22;
        const int gapListToPills = 16;
        const int gapPillsToBtn = 14;

        int bottom = h - footerH;
        int btnsTop = bottom - btnH;
        int pillsTop = btnsTop - gapPillsToBtn - pillsH;
        int listBottom = pillsTop - gapListToPills;
        int listTop = LIST_Y;
        int listH = std::max(60, listBottom - listTop);

        list = {LIST_X, listTop, w - 40, listH};
        int pw = (w - 40 - 3 * 10) / 4;
        for (int i = 0; i < 4; ++i)
            pills[i] = {20 + i * (pw + 10), pillsTop, pw, pillsH};
        cancelBtn = {20, btnsTop, (w - 40) / 2 - 5, btnH};
        startBtn  = {20 + (w - 40) / 2 + 5, btnsTop, (w - 40) / 2 - 5, btnH};

        grip = {w - 22, h - 22, 22, 22};
        listVisibleRows = std::max(1, listH / (ROW_H + ROW_GAP));
    }

    Layout() { Recompute(WIN_W_DEFAULT, WIN_H_DEFAULT); }
};

struct AppCtx {
    Config cfg;
    std::wstring exeDir;
    TranscriptWatcher watcher;
    StateMachine sm;   // only the primary session drives this SM
    bool paused = false;
    Layout layout;
    std::wstring doneMsg;
    std::wstring dryRunNotice;

    // Active sessions, sorted to match the watcher's order (newest mtime first).
    // The index of the "primary" session (= index 0) drives the SM; switching
    // primary resets SM to Monitoring.
    std::vector<SessionRow> sessions;
    std::wstring primaryPath;       // path of the row that drives the SM
    int scrollOffset = 0;           // first visible row index

    int FindRow(const std::wstring& path) const {
        for (size_t i = 0; i < sessions.size(); ++i)
            if (_wcsicmp(sessions[i].path.c_str(), path.c_str()) == 0)
                return (int)i;
        return -1;
    }
    void PrunePrimaryIfGone() {
        if (primaryPath.empty()) return;
        if (FindRow(primaryPath) < 0) {
            // Primary vanished — fall back to whichever is at index 0 now.
            primaryPath = sessions.empty() ? L"" : sessions[0].path;
            sm.Cancel();    // back to Monitoring, cancels any pending countdown
        }
    }

    Action Selected() const { return (Action)cfg.action; }
};

AppCtx* Ctx(HWND hwnd) {
    return reinterpret_cast<AppCtx*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

const wchar_t* ActionLong(int a) {
    switch (a) {
        case 1: return L"Restart";
        case 2: return L"Hibernate";
        case 3: return L"Lock";
        case 0:
        default:return L"Shutdown";
    }
}

void SaveCfg(HWND hwnd) {
    AppCtx* c = Ctx(hwnd);
    if (c) c->cfg.Save(c->exeDir);
}

// "Now - last" formatted compactly: "5s" / "2m" / "—".
std::wstring FormatAgo(LONGLONG ms) {
    if (ms <= 0) return L"—";
    if (ms < 1000) return L"now";
    LONGLONG sec = ms / 1000;
    if (sec < 60) return std::to_wstring((int)sec) + L"s";
    LONGLONG min = sec / 60;
    if (min < 60) return std::to_wstring((int)min) + L"m";
    return std::to_wstring((int)(min / 60)) + L"h";
}

const wchar_t* EventBadge(EvtType t, bool endTurn) {
    if (t == EvtType::Assistant) return endTurn ? L"end_turn" : L"tool_use";
    if (t == EvtType::User)      return L"user";
    if (t == EvtType::System)    return L"system";
    return L"event";
}

// Build a one-line summary for a row showing project, the most recent event
// and how long ago it was. The activity dot is drawn separately in PaintRow.
std::wstring FormatRow(const SessionRow& r) {
    LONGLONG now = (LONGLONG)GetTickCount64();
    LONGLONG sinceMs = r.lastEventTsMs ? (now - r.lastEventTsMs) : 0;
    std::wstring ago = FormatAgo(sinceMs);
    std::wstring proj = r.projectName.empty()
                          ? (r.isSubagent ? L"<subagent>" : L"<unknown>")
                          : r.projectName;
    std::wstring id = r.shortId;

    std::wstring line;
    line.reserve(96);
    line += proj + L" · " + id;
    line += L"   ";
    line += ago;
    line += L" ago · ";
    line += r.lastEventLabel.empty() ? L"waiting…" : r.lastEventLabel;
    if (r.isSubagent) line += L" · sub";
    return line;
}

// True if the row has shown "Claude doing something" (assistant event) within
// the last 30 seconds. Within 5s = bright green, otherwise muted.
bool RowActivelyProcessing(const SessionRow& r, Gdiplus::Color& outColor) {
    if (!r.lastAssistantMs) return false;
    LONGLONG age = (LONGLONG)GetTickCount64() - r.lastAssistantMs;
    if (age < 0) age = 0;
    if (age > 30 * 1000) return false;
    // Use u.success-equivalent: bright accent-ish green.
    outColor = Gdiplus::Color(255, 80, 220, 110);   // bright green
    if (age > 15 * 1000) {
        // Fading green.
        outColor = Gdiplus::Color(140, 80, 220, 110);
    }
    return true;
}

// Paint a single row.
void PaintRow(Gdiplus::Graphics& g, UIColors& u, const SessionRow& r,
              bool isPrimary, int topY, int listW) {
    Gdiplus::RectF rowRect((float)LIST_X, (float)topY, (float)listW, (float)ROW_H);
    if (isPrimary) {
        Gdiplus::SolidBrush bg(u.bgAlt);
        g.FillRectangle(&bg, rowRect);
    }

    Gdiplus::Font rFont(L"Segoe UI", 9.5f, isPrimary ? Gdiplus::FontStyleBold
                                                      : Gdiplus::FontStyleRegular);
    Gdiplus::Color txt = isPrimary ? u.text : u.muted;
    // Reserve the right ~14 px for the activity dot.
    Gdiplus::RectF textRect = rowRect;
    textRect.Width -= 14.0f;
    DrawCentered(g, FormatRow(r).c_str(), textRect, rFont, txt);

    if (isPrimary) {
        // Left bar marker.
        Gdiplus::SolidBrush bar(u.accent);
        g.FillRectangle(&bar, (float)(LIST_X - 3), (float)topY,
                        3.0f, (float)ROW_H);
    }

    // Right-edge activity dot: green if a recent assistant event arrived
    // (Claude is actually doing something); muted otherwise (user typing,
    // completed session, no parsed events).
    {
        Gdiplus::Color dotColor = u.muted;
        bool active = RowActivelyProcessing(r, dotColor);
        if (active) {
            Gdiplus::SolidBrush dot(dotColor);
            float cx = LIST_X + listW - 8.0f;
            float cy = (float)topY + ROW_H / 2.0f;
            g.FillEllipse(&dot, cx - 4.0f, cy - 4.0f, 8.0f, 8.0f);
        } else {
            // Hollow ring.
            Gdiplus::Pen ring(u.muted, 1.0f);
            float cx = LIST_X + listW - 8.0f;
            float cy = (float)topY + ROW_H / 2.0f;
            g.DrawEllipse(&ring, cx - 3.0f, cy - 3.0f, 6.0f, 6.0f);
        }
    }
}

void Paint(HWND hwnd) {
    AppCtx* c = Ctx(hwnd);
    if (!c) return;
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    // Double buffer: render to a bitmap, then blit.
    int W = c->layout.winW;
    int H = c->layout.winH;
    Gdiplus::Bitmap bmp(W, H, PixelFormat32bppARGB);
    Gdiplus::Graphics g(&bmp);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    UIColors u;

    g.Clear(u.bg);

    // Title bar
    Gdiplus::SolidBrush tb(u.bgAlt);
    g.FillRectangle(&tb, 0, 0, W, c->layout.title.h);
    Gdiplus::Font titleFont(L"Segoe UI", 12, Gdiplus::FontStyleBold);
    DrawCentered(g, L"AutoClaude — session monitor",
                 c->layout.title.Rect(), titleFont, u.text);

    // Status: state · N active · window Ms  (or just "no active sessions")
    std::wstring status;
    if (c->sessions.empty()) {
        status = c->paused ? L"PAUSED · no active sessions"
                           : L"no active sessions · waiting";
    } else {
        status = c->sm.StateName();
        status += L"  ·  ";
        status += std::to_wstring((int)c->sessions.size());
        status += L" active  ·  window ";
        status += std::to_wstring(c->cfg.activeWindowSec);
        status += L"s";
        if (c->paused) status += L"  ·  PAUSED";
        if (!c->doneMsg.empty()) status = c->doneMsg;
    }
    Gdiplus::Font sFont(L"Segoe UI", 10);
    DrawCentered(g, status.c_str(),
                 Gdiplus::RectF(20, 44, (float)(W - 40), 26), sFont, u.muted);

    // Sessions list
    // Clamp scroll offset so we don't over-scroll when the list shrinks.
    int visible = c->layout.listVisibleRows;
    int maxOffset = (int)c->sessions.size() > visible
                      ? (int)c->sessions.size() - visible
                      : 0;
    if (c->scrollOffset > maxOffset) c->scrollOffset = maxOffset;
    if (c->scrollOffset < 0) c->scrollOffset = 0;

    int listW = c->layout.list.w;
    int rowsToShow = std::min<int>((int)c->sessions.size() - c->scrollOffset, visible);
    for (int i = 0; i < rowsToShow; ++i) {
        const SessionRow& r = c->sessions[c->scrollOffset + i];
        bool isPrimary = (r.path == c->primaryPath);
        int topY = LIST_Y + i * (ROW_H + ROW_GAP);
        PaintRow(g, u, r, isPrimary, topY, listW);
    }
    if (c->sessions.empty()) {
        std::wstring empty = L"(no .jsonl files modified in the last "
                           + std::to_wstring(c->cfg.activeWindowSec)
                           + L" seconds)";
        Gdiplus::Font f(L"Segoe UI", 9, Gdiplus::FontStyleItalic);
        DrawCentered(g, empty.c_str(), c->layout.list.Rect(), f, u.muted);
    } else if ((int)c->sessions.size() > visible) {
        // Scroll hint in the bottom-right corner of the list.
        std::wstring hint = std::to_wstring(c->scrollOffset + 1) + L"-"
                          + std::to_wstring(std::min<int>(
                                c->scrollOffset + visible,
                                (int)c->sessions.size()))
                          + L" of " + std::to_wstring((int)c->sessions.size())
                          + L"  ·  scroll";
        Gdiplus::Font hFont(L"Segoe UI", 8);
        DrawCentered(g, hint.c_str(),
                     Gdiplus::RectF(20, (float)(LIST_Y + c->layout.list.h + 2),
                                    (float)listW, 14), hFont, u.muted);
    }

    // Action pills
    const wchar_t* names[4] = { L"Shutdown", L"Restart", L"Hibernate", L"Lock" };
    Gdiplus::Font pFont(L"Segoe UI", 11, Gdiplus::FontStyleBold);
    for (int i = 0; i < 4; ++i) {
        bool sel = (c->cfg.action == i);
        Gdiplus::RectF r = c->layout.pills[i].Rect();
        if (sel) {
            FillRound(g, r, u.accent, 10);
            DrawCentered(g, names[i], r, pFont, Gdiplus::Color(255, 255, 255, 255));
        } else {
            FillRound(g, r, u.bgAlt, 10);
            DrawRound(g, r, u.muted, 10, 1.5f);
            DrawCentered(g, names[i], r, pFont, u.text);
        }
    }

    // Buttons
    Gdiplus::Font bFont(L"Segoe UI", 11, Gdiplus::FontStyleBold);
    // Cancel (only meaningful during countdown)
    {
        bool active = (c->sm.state == State::Countdown);
        Gdiplus::RectF r = c->layout.cancelBtn.Rect();
        if (active) {
            FillRound(g, r, u.danger, 10);
            DrawCentered(g, L"Cancel", r, bFont, Gdiplus::Color(255, 255, 255, 255));
        } else {
            FillRound(g, r, u.bgAlt, 10);
            DrawRound(g, r, u.muted, 10, 1.5f);
            DrawCentered(g, L"Cancel", r, bFont, u.muted);
        }
    }
    // Start/Pause
    {
        Gdiplus::RectF r = c->layout.startBtn.Rect();
        const wchar_t* label = c->paused ? L"Resume" : L"Pause";
        if (c->paused) {
            FillRound(g, r, u.accent, 10);
            DrawCentered(g, label, r, bFont, Gdiplus::Color(255, 255, 255, 255));
        } else {
            FillRound(g, r, u.bgAlt, 10);
            DrawRound(g, r, u.muted, 10, 1.5f);
            DrawCentered(g, label, r, bFont, u.text);
        }
    }

    // Close X — drawn at integer coordinates with a chunkier pen so the
    // anti-aliasing has enough pixels to produce a crisp diagonal.
    {
        Gdiplus::RectF r = c->layout.closeBtn.Rect();
        Gdiplus::Pen xp(u.text, 2.0f);
        // Snap to integer pixels so GDI+ doesn't blur to half-pixel grid.
        float x1 = (float)((int)r.X + 9);
        float y1 = (float)((int)r.Y + 7);
        float x2 = (float)((int)r.X + 21);
        float y2 = (float)((int)r.Y + 19);
        g.DrawLine(&xp, x1, y1, x2, y2);
        g.DrawLine(&xp, x2, y1, x1, y2);
    }

    // Footer (dry-run + a one-line SM state readout for the primary).
    // Lives just above the resize grip; uses layout.winH as the anchor.
    Gdiplus::Font fFont(L"Segoe UI", 8.5f);
    std::wstring foot = c->cfg.dryRun ? L"DRY-RUN · " : L"LIVE · ";
    if (!c->doneMsg.empty()) {
        foot += c->doneMsg;
    } else if (c->sm.state == State::Countdown) {
        foot += std::to_wstring(c->sm.countdownRemaining) + L"s until action";
    } else if (c->sm.state == State::IdleWaiting) {
        LONGLONG now = (LONGLONG)GetTickCount64();
        LONGLONG rem = (c->sm.idleDeadlineMs - now) / 1000;
        if (rem < 0) rem = 0;
        foot += L"idle " + std::to_wstring((int)rem) + L"s";
    } else {
        foot += L"primary: " + c->primaryPath;
        // Keep the path short in the footer.
        size_t from = foot.size() > 60 ? foot.size() - 60 : 0;
        foot = (from > 0 ? L"…" : L"") + foot.substr(from);
    }
    DrawCentered(g, foot.c_str(),
                 Gdiplus::RectF(20, (float)(H - 22), (float)(W - 40), 18),
                 fFont, u.muted);

    // Resize grip — three diagonal lines tucked into the bottom-right
    // corner. Drawn last so nothing paints over it.
    {
        Gdiplus::Pen gripPen(u.muted, 1.5f);
        const Region& gr = c->layout.grip;
        int gx = gr.x;
        int gy = gr.y;
        for (int i = 0; i < 3; ++i) {
            // Each line steps up-and-left from the bottom-right corner.
            int off = i * 5;
            g.DrawLine(&gripPen,
                       (float)(gx + gr.w - 3 - off), (float)(gy + gr.h - 3),
                       (float)(gx + gr.w - 3),          (float)(gy + gr.h - 3 - off));
        }
    }

    // Blit to screen.
    Gdiplus::Graphics sg(hdc);
    sg.DrawImage(&bmp, 0, 0);

    EndPaint(hwnd, &ps);
}

void OnSessionEvent(HWND hwnd, SessionEvent* se) {
    AppCtx* c = Ctx(hwnd);
    if (!c) { delete se; return; }

    int rowIdx = c->FindRow(se->path);
    if (rowIdx >= 0) {
        SessionRow& r = c->sessions[rowIdx];
        r.eventsCount++;
        r.lastEventTsMs = se->e.ts_ms;

        const wchar_t* label = L"event";
        if (se->e.type == EvtType::Assistant) {
            r.lastAssistantMs = se->e.ts_ms;
            label = se->e.is_end_turn ? L"assistant: end_turn"
                                       : L"assistant: working";
        } else if (se->e.type == EvtType::User) {
            label = L"user message";
        } else if (se->e.type == EvtType::System) {
            label = se->e.is_api_error ? L"system: api_error"
                                         : L"system event";
        }
        r.lastEventLabel = label;
    }
    c->doneMsg.clear();

    // The state machine only listens to the primary session's events.
    if (!c->paused && rowIdx >= 0 && c->sessions[rowIdx].path == c->primaryPath) {
        c->sm.OnEvent(se->e, c->cfg.idleTimeoutSec);
    }
    DLog(L"[evt] %s type=%d end_turn=%d -> state=%d",
         se->path.c_str(),
         (int)se->e.type, se->e.is_end_turn ? 1 : 0,
         (int)c->sm.state);
    delete se;
    InvalidateRect(hwnd, nullptr, FALSE);
}

void OnSessionsUpdate(HWND hwnd, const std::vector<TrackInfo>* snap) {
    AppCtx* c = Ctx(hwnd);
    if (!c) { delete snap; return; }
    if (snap->empty()) {
        c->sessions.clear();
    } else {
        // Rebuild sessions, preserving per-row state for paths we already knew.
        std::vector<SessionRow> fresh;
        fresh.reserve(snap->size());
        for (const auto& ti : *snap) {
            int old = c->FindRow(ti.path);
            if (old >= 0) {
                fresh.push_back(c->sessions[old]);   // keep counters / labels
                fresh.back().path = ti.path;
                fresh.back().projectName = ti.projectName;
                fresh.back().shortId = ti.shortId;
                fresh.back().isSubagent = ti.isSubagent;
            } else {
                SessionRow r;
                r.path = ti.path;
                r.projectName = ti.projectName;
                r.shortId = ti.shortId;
                r.isSubagent = ti.isSubagent;
                r.lastEventLabel = L"waiting for events…";
                fresh.push_back(std::move(r));
            }
        }
        c->sessions = std::move(fresh);
    }

    // Track the primary. Index 0 is newest mtime — that's the one driving the SM.
    c->primaryPath = c->sessions.empty() ? L"" : c->sessions[0].path;
    c->PrunePrimaryIfGone();

    DLog(L"[sessions] snapshot: %zu active, primary=%s",
         c->sessions.size(), c->primaryPath.c_str());
    delete snap;
    InvalidateRect(hwnd, nullptr, FALSE);
}

void OnTimerTick(HWND hwnd) {
    AppCtx* c = Ctx(hwnd);
    if (!c) return;
    if (c->paused) return;
    if (c->sm.state == State::Countdown || c->sm.state == State::IdleWaiting) {
        bool zero = c->sm.OnTimer(c->cfg.countdownSec);
        DLog(L"[timer] state=%d countdown=%d zero=%d",
             (int)c->sm.state, c->sm.countdownRemaining, zero ? 1 : 0);
        if (zero) {
            Action a = c->Selected();
            const wchar_t* msg = PerformPowerAction(a, c->cfg.dryRun);
            DLog(L"[power] action=%d dryRun=%d msg=%s",
                 (int)a, c->cfg.dryRun ? 1 : 0, msg ? msg : L"(null)");
            if (c->cfg.dryRun) {
                c->doneMsg = std::wstring(L"DONE (dry-run) · ")
                           + ActionLong(c->cfg.action);
            } else {
                c->doneMsg = L"performed: ";
                c->doneMsg += msg;
            }
        }
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

void OnLButtonDown(HWND hwnd, int x, int y) {
    AppCtx* c = Ctx(hwnd);
    if (!c) return;
    auto& L = c->layout;

    if (L.closeBtn.Contains(x, y)) { PostMessageW(hwnd, WM_CLOSE, 0, 0); return; }

    for (int i = 0; i < 4; ++i) {
        if (L.pills[i].Contains(x, y)) {
            c->cfg.action = i;
            SaveCfg(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
    }
    if (L.cancelBtn.Contains(x, y) && c->sm.state == State::Countdown) {
        c->sm.Cancel();
        c->doneMsg.clear();
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }
    if (L.startBtn.Contains(x, y)) {
        c->paused = !c->paused;
        c->watcher.SetPaused(c->paused);
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            SetTimer(hwnd, 1, 1000, nullptr);
            return 0;
        }
        case WM_PAINT: Paint(hwnd); return 0;
        case WM_LBUTTONDOWN: OnLButtonDown(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
        case WM_MOUSEWHEEL: {
            AppCtx* c = Ctx(hwnd);
            if (!c) return 0;
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            int step = -delta / WHEEL_DELTA;   // up = -delta
            int maxOff = std::max<int>(0, (int)c->sessions.size() - c->layout.listVisibleRows);
            c->scrollOffset = std::max<int>(0,
                                std::min<int>(maxOff, c->scrollOffset + step));
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_SIZE: {
            AppCtx* c = Ctx(hwnd);
            if (!c) return 0;
            int w = LOWORD(lp);
            int h = HIWORD(lp);
            if (w <= 0 || h <= 0) return 0;
            c->layout.Recompute(w, h);
            // Clamp scroll offset against the new row count.
            int maxOff = std::max<int>(0, (int)c->sessions.size() - c->layout.listVisibleRows);
            if (c->scrollOffset > maxOff) c->scrollOffset = maxOff;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_TIMER: if (wp == 1) OnTimerTick(hwnd); return 0;
        case WM_APP_EVT:
            OnSessionEvent(hwnd, reinterpret_cast<SessionEvent*>(lp));
            return 0;
        case WM_APP_SESSIONS:
            OnSessionsUpdate(hwnd,
                reinterpret_cast<const std::vector<TrackInfo>*>(lp));
            return 0;
        case WM_NCHITTEST: {
            POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(hwnd, &p);
            AppCtx* c = Ctx(hwnd);
            if (c) {
                if (c->layout.closeBtn.Contains(p.x, p.y)) return HTCLOSE;
                if (c->layout.grip.Contains(p.x, p.y))    return HTBOTTOMRIGHT;
                if (c->layout.title.Contains(p.x, p.y))   return HTCAPTION;
            }
            return HTCLIENT;
        }
        case WM_NCCALCSIZE: {
            // With WS_THICKFRAME set on the window, the system would normally
            // reserve a non-client border (caption + sizing frame). We render
            // the whole surface ourselves in WM_PAINT, so suppress the
            // non-client area entirely by reporting that no borders exist.
            if (wp) {
                return WVR_ALIGNTOP | WVR_ALIGNLEFT
                     | WVR_ALIGNBOTTOM | WVR_ALIGNRIGHT;
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMinTrackSize = { WIN_W_MIN, WIN_H_MIN };
            mmi->ptMaxTrackSize = { LONG_MAX, LONG_MAX };
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_DESTROY:
            KillTimer(hwnd, 1);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

void RunApp(const Config& initialCfg) {
    GdiInit();
    DLog(L"[startup] AutoClaude starting, dryRun=%d project=%s window=%ds",
         initialCfg.dryRun ? 1 : 0,
         initialCfg.project.c_str(),
         initialCfg.activeWindowSec);

    auto ctx = std::make_unique<AppCtx>();
    ctx->cfg = initialCfg;
    ctx->exeDir = ExeDir();
    if (initialCfg.dryRun) ctx->dryRunNotice = L"DRY-RUN";

    const wchar_t* kClass = L"AutoClaudeClass";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    HWND hwnd = CreateWindowExW(
        WS_EX_APPWINDOW, kClass, L"AutoClaude",
        WS_POPUP | WS_THICKFRAME | WS_VISIBLE,
        (sw - WIN_W_DEFAULT) / 2, (sh - WIN_H_DEFAULT) / 2,
        WIN_W_DEFAULT, WIN_H_DEFAULT,
        nullptr, nullptr, wc.hInstance, ctx.get());
    if (!hwnd) return;

    ctx->watcher.Start(hwnd, ctx->cfg);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    ctx->watcher.Stop();
    ctx.release();
    GdiShutdown();
}
