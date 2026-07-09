#include "window.h"
#include "drawing.h"
#include "../power/power_action.h"
#include "../ipc/status_pipe.h"
#include "../ipc/ac_paths.h"
#include <windows.h>
#include <windowsx.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellscalingapi.h>
#include <string>
#include <memory>
#include <vector>
#include <cstdio>
#include <cmath>

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

constexpr int WIN_W_DEFAULT = 640;
constexpr int WIN_H_DEFAULT = 580;
constexpr int WIN_W_MIN     = 480;
constexpr int WIN_H_MIN     = 420;

constexpr int ROW_H  = 36;
constexpr int ROW_GAP = 4;
constexpr int LIST_X = 20;
constexpr int LIST_Y = 86;

struct Region {
    int x, y, w, h;
    bool Contains(int px, int py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
    Gdiplus::RectF RectF() const {
        return Gdiplus::RectF((float)x, (float)y, (float)w, (float)h);
    }
    Gdiplus::Rect Rect() const {
        return Gdiplus::Rect(x, y, w, h);
    }
};

struct SessionRow {
    std::wstring path;
    std::wstring projectName;
    std::wstring shortId;
    bool isSubagent = false;
    int eventsCount = 0;
    LONGLONG lastEventTsMs = 0;
    LONGLONG lastAssistantMs = 0;
    std::wstring lastEventLabel;

    // Cumulative token usage since the watcher discovered this session.
    // Per-event values come from message.usage; we add to running totals
    // so the row can show "this session has used N tokens so far".
    unsigned long long tokInputTotal         = 0;
    unsigned long long tokOutputTotal        = 0;
    unsigned long long tokCacheReadTotal     = 0;
    unsigned long long tokCacheCreationTotal = 0;

    // Auto-continue loop status (from the named-pipe sensor).
    int  autoTurns = 0;
    bool loopEnabled = false;
};

struct Layout {
    Region title;
    Region status;
    Region list;
    Region pills[4];
    Region cancelBtn;
    Region stopLoopBtn;
    Region startBtn;
    Region closeBtn;
    Region helpBtn;
    Region grip;
    int  listVisibleRows = 8;
    int  winW = WIN_W_DEFAULT;
    int  winH = WIN_H_DEFAULT;

    void Recompute(int w, int h) {
        winW = w; winH = h;
        title    = {0, 0, w, 42};
        status   = {20, 50, w - 40, 28};
        // Close X lives at the right edge; help "?" sits to its left so the
        // title strip keeps both buttons discoverable.
        helpBtn  = {w - 80, 6, 36, 32};
        closeBtn = {w - 44, 6, 36, 32};

        const int pillsH = 40;
        const int btnH   = 46;
        const int footerH = 28;
        const int gapListToPills = 16;
        const int gapPillsToBtn  = 12;

        int bottom    = h - footerH;
        int btnsTop   = bottom - btnH;
        int pillsTop  = btnsTop - gapPillsToBtn - pillsH;
        int listBottom = pillsTop - gapListToPills;
        int listTop    = LIST_Y;
        int listH      = std::max(100, listBottom - listTop);

        list = {LIST_X, listTop, w - 40, listH};

        int innerW = w - 40 - 3 * 10;
        int pw = innerW / 4;
        for (int i = 0; i < 4; ++i)
            pills[i] = {20 + i * (pw + 10), pillsTop, pw, pillsH};

        // Bottom row: three equal buttons — Cancel / Stop Loop / Pause.
        int triGap = 8;
        int triW   = ((w - 40) - 2 * triGap) / 3;
        cancelBtn   = {20,                          btnsTop, triW, btnH};
        stopLoopBtn = {20 + triW + triGap,          btnsTop, triW, btnH};
        startBtn    = {20 + 2 * (triW + triGap),    btnsTop, triW, btnH};

        grip = {w - 24, h - 24, 24, 24};
        listVisibleRows = std::max(1, listH / (ROW_H + ROW_GAP));
    }

    Layout() { Recompute(WIN_W_DEFAULT, WIN_H_DEFAULT); }
};

struct AppCtx {
    Config cfg;
    std::wstring exeDir;
    TranscriptWatcher watcher;
    StatusPipeServer pipe;
    StateMachine sm;
    bool paused = false;
    Layout layout;
    std::wstring doneMsg;

    std::vector<SessionRow> sessions;
    std::wstring primaryPath;
    int scrollOffset = 0;

    // Hover tracking — updated on WM_MOUSEMOVE, cleared on WM_MOUSELEAVE.
    bool hoverClose = false;
    bool hoverGrip  = false;
    bool hoverCancel = false;
    bool hoverStopLoop = false;
    bool hoverStart = false;
    bool hoverHelp  = false;
    int  hoverPill  = -1;     // 0..3, -1 = none
    bool trackingMouseLeave = false;

    // Help overlay — when true, the main UI paints dimmed and a centred
    // help card is drawn on top. Any click closes it (ESC also closes).
    bool helpOpen = false;

    // Animation tick count for the breathing active-dot.
    ULONGLONG pulseStartMs = 0;

    int FindRow(const std::wstring& path) const {
        for (size_t i = 0; i < sessions.size(); ++i)
            if (_wcsicmp(sessions[i].path.c_str(), path.c_str()) == 0)
                return (int)i;
        return -1;
    }
    void PrunePrimaryIfGone() {
        if (primaryPath.empty()) return;
        if (FindRow(primaryPath) < 0) {
            primaryPath = sessions.empty() ? L"" : sessions[0].path;
            sm.Cancel();
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

std::wstring FormatAgo(LONGLONG ms) {
    if (ms <= 0) return L"—";
    if (ms < 1000) return L"now";
    LONGLONG sec = ms / 1000;
    if (sec < 60) return std::to_wstring((int)sec) + L"s";
    LONGLONG min = sec / 60;
    if (min < 60) return std::to_wstring((int)min) + L"m";
    return std::to_wstring((int)(min / 60)) + L"h";
}

// Same as FormatAgo but always shows seconds — used by the pulse indicator.
std::wstring FormatAgoFine(LONGLONG ms) {
    if (ms < 0) ms = 0;
    if (ms < 1000) return L"now";
    return std::to_wstring((int)(ms / 1000)) + L"s";
}

// Compact human-readable token count: "942", "1.2k", "1.5M", "2.1G".
// Used for the per-session "↓in ↑out" footer line.
std::wstring FormatTokens(unsigned long long n) {
    if (n < 1000ull)         return std::to_wstring((unsigned long long)n);
    const wchar_t* units[] = { L"k", L"M", L"G" };
    double val = (double)n;
    int u = 0;
    while (val >= 1000.0 && u < 2) { val /= 1000.0; ++u; }
    wchar_t buf[32];
    if (val >= 100.0) swprintf(buf, 32, L"%.0f%ls", val, units[u]);
    else if (val >= 10.0) swprintf(buf, 32, L"%.1f%ls", val, units[u]);
    else swprintf(buf, 32, L"%.2f%ls", val, units[u]);
    return buf;
}

// Color of the leading indicator for a given SM state.
Gdiplus::Color StateColor(const UIColors& u, State s, bool paused) {
    if (paused)           return u.muted;
    switch (s) {
        case State::Monitoring:  return u.accent;     // cyan
        case State::IdleWaiting: return u.warn;       // amber
        case State::Countdown:   return u.warn;       // amber
        case State::Done:        return u.danger;     // red
    }
    return u.muted;
}

const wchar_t* StateText(State s, bool paused) {
    if (paused)             return L"PAUSED";
    switch (s) {
        case State::Monitoring:  return L"MONITORING";
        case State::IdleWaiting: return L"IDLE WAITING";
        case State::Countdown:   return L"COUNTDOWN";
        case State::Done:        return L"DONE";
    }
    return L"?";
}

// Breathing pulse factor — sin wave 0..1, period 1.6s.
float Pulse(ULONGLONG nowMs, ULONGLONG startMs) {
    LONGLONG dt = (LONGLONG)nowMs - (LONGLONG)startMs;
    if (dt < 0) dt = 0;
    float t = (float)(dt % 1600) / 1600.0f;
    return 0.5f - 0.5f * std::cos(t * 6.2831853f);
}

// Decide how hot (full-bright vs dim) a row's leading bar should be based on
// whether Claude is actively working (recent assistant event).
int RowHotLevel(const SessionRow& r) {
    if (!r.lastAssistantMs) return 0;
    LONGLONG age = (LONGLONG)GetTickCount64() - r.lastAssistantMs;
    if (age < 0)     return 0;
    if (age > 30000) return 0;
    if (age > 15000) return 1;
    return 2;
}

Gdiplus::Color RowHotColor(const UIColors& u, int level) {
    if (level >= 2) return u.success;
    if (level >= 1) return u.warn;
    return u.dim;
}

// ---------- Drawing helpers (window-local) ----------

void DrawPillBackground(Gdiplus::Graphics& g, const Region& r,
                        const UIColors& u,
                        Gdiplus::Color border, bool filled = false) {
    Gdiplus::RectF rf = r.RectF();
    if (filled) {
        FillRound(g, rf, border, 10.0f);
    } else {
        FillRoundGradient(g, rf, u.panel, u.bgBottom, 10.0f);
        DrawRound(g, rf, border, 10.0f, 1.0f);
    }
}

void DrawCloseX(Gdiplus::Graphics& g, const Region& r,
                const UIColors& u, bool hover) {
    if (hover) {
        Gdiplus::RectF rf = r.RectF();
        FillRoundGradient(g, rf,
            Gdiplus::Color(255, 200,  60,  70),
            Gdiplus::Color(255, 140,  40,  48),
            8.0f);
        DrawRound(g, rf, u.danger, 8.0f, 1.0f);
    }
    Gdiplus::Color xc = hover ? u.text : u.textDim;
    int cx = r.x + r.w / 2;
    int cy = r.y + r.h / 2;
    int h  = 5;
    Gdiplus::Pen xp(xc, 1.5f);
    xp.SetAlignment(Gdiplus::PenAlignmentInset);
    g.DrawLine(&xp,
        Gdiplus::PointF((float)(cx - h), (float)(cy - h)),
        Gdiplus::PointF((float)(cx + h + 1), (float)(cy + h + 1)));
    g.DrawLine(&xp,
        Gdiplus::PointF((float)(cx + h + 1), (float)(cy - h)),
        Gdiplus::PointF((float)(cx - h), (float)(cy + h + 1)));
}

void DrawGrip(Gdiplus::Graphics& g, const Region& r,
              const UIColors& u, bool hover) {
    Gdiplus::Color c = hover ? u.gripHot : u.grip;
    Gdiplus::Pen p(c, 1.5f);
    p.SetAlignment(Gdiplus::PenAlignmentInset);
    int gx = r.x, gy = r.y;
    for (int i = 0; i < 3; ++i) {
        int off = i * 5;
        g.DrawLine(&p,
            Gdiplus::PointF((float)(gx + r.w - 4 - off), (float)(gy + r.h - 4)),
            Gdiplus::PointF((float)(gx + r.w - 4),       (float)(gy + r.h - 4 - off)));
    }
}

// Help "?" button — same visual weight as close X. Idle: panel bg + dim "?";
// hover: panelHi bg + bright "?" with an accent border for affordance.
void DrawHelpIcon(Gdiplus::Graphics& g, const Region& r,
                  const UIColors& u, bool hover) {
    Gdiplus::RectF rf = r.RectF();
    if (hover) {
        FillRoundGradient(g, rf, u.panelHi,
                          Gdiplus::Color(255, 30, 36, 46), 8.0f);
        DrawRound(g, rf, u.accent, 8.0f, 1.0f);
    } else {
        FillRoundGradient(g, rf, u.panel, u.bgBottom, 8.0f);
        DrawRound(g, rf, u.divider, 8.0f, 1.0f);
    }
    Gdiplus::Color qc = hover ? u.accentSoft : u.textDim;
    DrawLabel(g, L"?", rf,
              L"Segoe UI", 16.0f, Gdiplus::FontStyleBold, qc);
}

// Centred modal-style help card. Backdrop dims everything beneath at
// ~70% alpha so the underlying UI is still faintly visible but the card
// reads as the focus of attention. Click anywhere or press ESC to dismiss.
void PaintHelpOverlay(Gdiplus::Graphics& g, UIColors& u, int W, int H) {
    // Dimming backdrop.
    Gdiplus::SolidBrush back(Gdiplus::Color(180,  8, 10, 16));
    g.FillRectangle(&back, 0, 0, W, H);

    const int cardW = 480;
    const int cardH = 360;
    const int cardX = (W - cardW) / 2;
    const int cardY = (H - cardH) / 2;
    Gdiplus::RectF cardRect((float)cardX, (float)cardY,
                            (float)cardW, (float)cardH);
    FillRoundGradient(g, cardRect, u.panelAlt, u.bgBottom, 12.0f);
    DrawRound(g, cardRect, u.grip, 12.0f, 1.0f);

    // Title.
    DrawLabel(g, L"AutoClaude  —  Quick reference",
              Gdiplus::RectF((float)(cardX + 24), (float)(cardY + 12),
                             (float)(cardW - 48), 28.0f),
              L"Segoe UI", 16.0f, Gdiplus::FontStyleBold, u.text,
              Gdiplus::StringAlignmentNear, Gdiplus::StringAlignmentCenter);
    DrawHairline(g, cardX + 24, cardY + 50, cardW - 48, u.divider);

    int bodyX = cardX + 24;
    int bodyW = cardW - 48;
    int bodyY = cardY + 62;

    auto Section = [&](const wchar_t* heading,
                       const std::vector<const wchar_t*>& lines) {
        // 2px accent bar to the left of the heading.
        Gdiplus::SolidBrush accB(u.accent);
        g.FillRectangle(&accB, (float)bodyX, (float)(bodyY + 4),
                        2.0f, 12.0f);
        DrawLabelLeft(g, heading,
                      Gdiplus::RectF((float)(bodyX + 8), (float)bodyY,
                                     (float)(bodyW - 8), 16.0f),
                      L"Segoe UI", 12.0f,
                      Gdiplus::FontStyleBold, u.text);
        bodyY += 18;
        for (auto ln : lines) {
            DrawLabelLeft(g, ln,
                          Gdiplus::RectF((float)(bodyX + 12), (float)bodyY,
                                         (float)(bodyW - 12), 14.0f),
                          L"Segoe UI", 10.5f,
                          Gdiplus::FontStyleRegular, u.textDim);
            bodyY += 14;
        }
        bodyY += 4;
    };

    Section(L"Status dot (top row)", {
        L"cyan  = monitoring ·  amber = idle / countdown",
        L"red   = done     ·  grey  = paused",
    });
    Section(L"Action pills (Shutdown / Restart / Hibernate / Lock)", {
        L"Pick the action. The cyan-bordered pill is the active one.",
        L"Fires only on the primary session (newest .jsonl).",
    });
    Section(L"Cancel button", {
        L"Aborts an armed countdown. Red = armed; grey = nothing to cancel.",
    });
    Section(L"Pause / Resume", {
        L"Halts all monitoring. Default mode is DRY-RUN; nothing actually",
        L"shuts down unless DRY-RUN is off.",
    });
    Section(L"Session rows", {
        L"↳ marks subagent sessions. Bottom line shows last event,",
        L"event count, and cumulative token usage (↓ in  ↑ out).",
    });

    // Footer dismiss hint.
    DrawLabel(g, L"click anywhere to dismiss  ·  ESC",
              Gdiplus::RectF((float)cardX, (float)(cardY + cardH - 26),
                             (float)cardW, 18.0f),
              L"Cascadia Mono", 10.5f,
              Gdiplus::FontStyleRegular, u.muted);
}

// One session row, two lines:
//   top:    ↳ projectName            ago
//   bottom: last event label · N events · ↓X ↑Y
// The leading 3px bar is the activity indicator (cyan for primary, hot
// color for active non-primary, dim otherwise).
void PaintRow(Gdiplus::Graphics& g, UIColors& u, const SessionRow& r,
              bool isPrimary, int topY, int listW, int maxAutoTurns) {
    Gdiplus::RectF rowRect((float)LIST_X, (float)topY,
                           (float)listW, (float)ROW_H);
    int radius = 6;

    if (isPrimary) {
        FillRoundGradient(g, rowRect, u.panelAlt,
                          Gdiplus::Color(255, 27, 31, 40), (float)radius);
        DrawRound(g, rowRect, u.divider, (float)radius, 1.0f);
    } else {
        FillRound(g, rowRect, u.panel, (float)radius);
    }

    // Leading 3px activity bar (full row height).
    int hot = RowHotLevel(r);
    Gdiplus::Color barColor = isPrimary ? u.accent : RowHotColor(u, hot);
    Gdiplus::SolidBrush bar(barColor);
    g.FillRectangle(&bar, (float)LIST_X + 4, (float)topY + 4,
                    3.0f, (float)ROW_H - 8);

    // ---- Top line: project name (left) + ago (right) ----
    Gdiplus::RectF topRect(rowRect.X + 12, (float)(topY + 1),
                           rowRect.Width - 80, 17.0f);
    std::wstring proj;
    proj.reserve(96);
    if (r.isSubagent) proj += L"↳  ";
    proj += r.projectName.empty()
                ? (r.isSubagent ? L"<subagent>" : L"<unknown>")
                : r.projectName;
    DrawLabelLeft(g, proj.c_str(), topRect,
                 L"Segoe UI", 12.5f,
                 isPrimary ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular,
                 isPrimary ? u.text : u.textDim);

    std::wstring ago = FormatAgoFine(
        r.lastEventTsMs ? (LONGLONG)GetTickCount64() - r.lastEventTsMs : 0);
    DrawLabel(g, ago.c_str(),
              Gdiplus::RectF(rowRect.X + rowRect.Width - 72, (float)(topY + 1),
                             56, 17.0f),
              L"Cascadia Mono", 11.0f,
              Gdiplus::FontStyleRegular, u.muted);

    // ---- Bottom line: last event · N events · ↓in ↑out ----
    Gdiplus::RectF botRect(rowRect.X + 12, (float)(topY + 18),
                           rowRect.Width - 24, 16.0f);
    std::wstring bot;
    bot.reserve(160);
    bot += r.lastEventLabel.empty() ? L"waiting…" : r.lastEventLabel;
    bot += L"  ·  ";
    bot += std::to_wstring(r.eventsCount);
    bot += L" events";
    if (r.tokInputTotal || r.tokOutputTotal) {
        bot += L"  ·  ↓";
        bot += FormatTokens(r.tokInputTotal);
        bot += L"  ↑";
        bot += FormatTokens(r.tokOutputTotal);
    }
    if (r.loopEnabled) {
        bot += L"  ·  loop ";
        bot += std::to_wstring(r.autoTurns);
        bot += L"/";
        bot += std::to_wstring(maxAutoTurns);
    }
    DrawLabelLeft(g, bot.c_str(), botRect,
                  L"Segoe UI", 10.5f,
                  Gdiplus::FontStyleRegular, u.muted);

    // Primary row gets a tiny cyan dot on the right of the top line as an
    // extra affordance (the gradient bg already differentiates it but the
    // dot makes scanning the list faster).
    if (isPrimary) {
        Gdiplus::SolidBrush dotB(u.accent);
        float dDot = 6.0f;
        float dcx = rowRect.X + rowRect.Width - 10;
        float dcy = topY + 9.5f;
        g.FillEllipse(&dotB, dcx - dDot/2, dcy - dDot/2, dDot, dDot);
    }
}

void Paint(HWND hwnd) {
    AppCtx* c = Ctx(hwnd);
    if (!c) return;
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    int W = c->layout.winW, H = c->layout.winH;
    Gdiplus::Bitmap bmp(W, H, PixelFormat32bppARGB);
    Gdiplus::Graphics g(&bmp);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQuality);
    UIColors u;

    // ---- Backdrop ----
    FillVGradient(g, Gdiplus::Rect(0, 0, W, H), u.bgTop, u.bgBottom);
    // 1px scanlines every 3px, very faint, gives texture without noise.
    DrawScanlines(g, Gdiplus::Rect(0, 0, W, H),
                  Gdiplus::Color(14, 255, 255, 255), 3);

    // ---- Title strip ----
    Region titleR = c->layout.title;
    FillVGradient(g, titleR.Rect(), u.panelAlt,
                  Gdiplus::Color(255, 22, 26, 34));
    // Left edge accent.
    Gdiplus::SolidBrush accB(u.accent);
    g.FillRectangle(&accB, 0, 0, 4, titleR.h);
    DrawHairline(g, 0, titleR.h - 1, W, u.divider);
    DrawLabel(g, L"AUTOCLAUDE",
             Gdiplus::RectF((float)titleR.x + 14, (float)titleR.y,
                            (float)titleR.w - 110, (float)titleR.h),
             L"Segoe UI", 16.0f, Gdiplus::FontStyleBold,
             u.text,
             Gdiplus::StringAlignmentNear,
             Gdiplus::StringAlignmentCenter);
    DrawLabel(g, L"session monitor",
             Gdiplus::RectF((float)titleR.x + 140, (float)titleR.y,
                            200, (float)titleR.h),
             L"Segoe UI", 11.0f, Gdiplus::FontStyleRegular,
             u.muted,
             Gdiplus::StringAlignmentNear,
             Gdiplus::StringAlignmentCenter);

    // ---- Close X + Help "?" (help is to the left of close) ----
    DrawCloseX(g, c->layout.closeBtn, u, c->hoverClose);
    DrawHelpIcon(g, c->layout.helpBtn, u, c->hoverHelp);

    // ---- Status row: colored dot + state badge + counters ----
    Region statR = c->layout.status;
    ULONGLONG now = GetTickCount64();
    bool isPulsingState = !c->paused &&
        (c->sm.state == State::Monitoring ||
         c->sm.state == State::IdleWaiting ||
         c->sm.state == State::Countdown);
    float pulse = isPulsingState ? Pulse(now, c->pulseStartMs) : 0.0f;

    Gdiplus::Color dotColor = StateColor(u, c->sm.state, c->paused);
    if (isPulsingState) {
        int boost = (int)(pulse * 90.0f);
        dotColor = Gdiplus::Color(
            255,
            (BYTE)std::min(255, (int)dotColor.GetR() + boost/3),
            (BYTE)std::min(255, (int)dotColor.GetG() + boost/3),
            (BYTE)std::min(255, (int)dotColor.GetB() + boost/3));
    }

    // Leading state dot.
    Gdiplus::SolidBrush dotB(dotColor);
    float dotD = 11.0f;
    g.FillEllipse(&dotB, (float)statR.x, (float)statR.y + (statR.h - dotD) / 2,
                  dotD, dotD);

    // State name + counters.
    std::wstring left = StateText(c->sm.state, c->paused);
    std::wstring right;
    if (!c->doneMsg.empty()) {
        right = c->doneMsg;
    } else if (c->sessions.empty()) {
        right = L"no active sessions · waiting";
    } else {
        right = std::to_wstring((int)c->sessions.size()) + L" active · window "
              + std::to_wstring(c->cfg.activeWindowSec) + L"s";
        if (c->paused) right += L" · PAUSED";
    }
    DrawLabel(g, left.c_str(),
             Gdiplus::RectF((float)statR.x + 22, (float)statR.y,
                            250, (float)statR.h),
             L"Segoe UI", 13.0f, Gdiplus::FontStyleBold, u.text,
             Gdiplus::StringAlignmentNear,
             Gdiplus::StringAlignmentCenter);
    DrawLabel(g, right.c_str(),
             Gdiplus::RectF((float)statR.x + 260, (float)statR.y,
                            (float)statR.w - 280, (float)statR.h),
             L"Segoe UI", 12.0f, Gdiplus::FontStyleRegular, u.muted,
             Gdiplus::StringAlignmentNear,
             Gdiplus::StringAlignmentCenter);

    // Countdown bar (only when state == Countdown).
    if (c->sm.state == State::Countdown && !c->paused) {
        int barX = statR.x + 18;
        int barY = statR.y + statR.h - 5;
        int barW = statR.w - 36;
        int barH = 3;
        float frac = (float)c->sm.countdownRemaining /
                     (float)std::max(1, c->cfg.countdownSec);
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        Gdiplus::SolidBrush track(u.divider);
        g.FillRectangle(&track, (float)barX, (float)barY,
                        (float)barW, (float)barH);
        Gdiplus::SolidBrush fill(u.warn);
        g.FillRectangle(&fill, (float)barX, (float)barY,
                        (float)(barW * frac), (float)barH);
    }

    // ---- Sessions list ----
    int visible = c->layout.listVisibleRows;
    int maxOffset = (int)c->sessions.size() > visible
                      ? (int)c->sessions.size() - visible : 0;
    if (c->scrollOffset > maxOffset) c->scrollOffset = maxOffset;
    if (c->scrollOffset < 0) c->scrollOffset = 0;

    int listW = c->layout.list.w;
    int rowsToShow = std::min<int>((int)c->sessions.size() - c->scrollOffset,
                                    visible);
    for (int i = 0; i < rowsToShow; ++i) {
        const SessionRow& r = c->sessions[c->scrollOffset + i];
        bool isPrimary = (r.path == c->primaryPath);
        int topY = LIST_Y + i * (ROW_H + ROW_GAP);
        PaintRow(g, u, r, isPrimary, topY, listW, c->cfg.maxAutoTurns);
    }
    if (c->sessions.empty()) {
        std::wstring empty = L"no .jsonl files modified in the last "
                           + std::to_wstring(c->cfg.activeWindowSec)
                           + L"s";
        DrawLabel(g, empty.c_str(),
                 Gdiplus::RectF((float)c->layout.list.x,
                                (float)c->layout.list.y,
                                (float)c->layout.list.w,
                                (float)c->layout.list.h),
                 L"Segoe UI", 13.0f,
                 Gdiplus::FontStyleItalic, u.muted);
    } else if ((int)c->sessions.size() > visible) {
        int endIdx = std::min<int>(c->scrollOffset + visible,
                                    (int)c->sessions.size());
        std::wstring hint = std::to_wstring(c->scrollOffset + 1) + L"–"
                          + std::to_wstring(endIdx) + L"  of  "
                          + std::to_wstring((int)c->sessions.size());
        DrawLabel(g, hint.c_str(),
                 Gdiplus::RectF((float)(c->layout.list.x),
                                (float)(LIST_Y + c->layout.list.h + 4),
                                (float)listW, 14),
                 L"Cascadia Mono", 10.0f,
                 Gdiplus::FontStyleRegular, u.muted);
    }

    // ---- Action pills ----
    const wchar_t* names[4] = { L"Shutdown", L"Restart", L"Hibernate", L"Lock" };
    for (int i = 0; i < 4; ++i) {
        bool sel = (c->cfg.action == i);
        bool hot = (c->hoverPill == i);
        Region r = c->layout.pills[i];
        Gdiplus::RectF rf = r.RectF();
        Gdiplus::Color border = sel ? u.accent
                          : hot    ? u.gripHot
                                   : u.divider;
        Gdiplus::Color fillT  = sel ? u.accentDeep
                          : hot    ? u.panelHi
                                   : u.panel;
        Gdiplus::Color fillB  = sel ? Gdiplus::Color(255,  8, 50, 82)
                          : hot    ? Gdiplus::Color(255, 35, 41, 52)
                                   : u.bgBottom;
        FillRoundGradient(g, rf, fillT, fillB, 10.0f);
        DrawRound(g, rf, border, 10.0f, sel ? 1.5f : 1.0f);
        Gdiplus::Color tColor = sel ? u.text
                          : hot    ? u.textDim
                                   : u.textDim;
        int style = sel ? Gdiplus::FontStyleBold : Gdiplus::FontStyleBold;
        DrawLabel(g, names[i], rf,
                 L"Segoe UI", 13.0f, style, tColor);
    }

    // ---- Cancel + Start/Pause buttons ----
    // Cancel: only meaningful during countdown; danger color when armed.
    {
        Region r = c->layout.cancelBtn;
        bool armed = (c->sm.state == State::Countdown) && !c->paused;
        bool hot = c->hoverCancel;
        Gdiplus::RectF rf = r.RectF();
        if (armed) {
            FillRoundGradient(g, rf,
                Gdiplus::Color(255, 200, 80, 80),
                Gdiplus::Color(255, 160, 50, 50),
                10.0f);
            DrawRound(g, rf, u.danger, 10.0f, 1.5f);
        } else {
            Gdiplus::Color fillT = hot ? u.panelHi : u.panel;
            FillRoundGradient(g, rf, fillT, u.bgBottom, 10.0f);
            DrawRound(g, rf, hot ? u.gripHot : u.divider, 10.0f, 1.0f);
        }
        // 2-line label: "Cancel" / "abort countdown"
        Gdiplus::Color cT = armed ? u.text : u.text;
        Gdiplus::Color cC = armed ? Gdiplus::Color(255, 255, 230, 230)
                                  : u.dim;
        Gdiplus::RectF topRect(rf.X, rf.Y + 4, rf.Width, rf.Height / 2 - 2);
        Gdiplus::RectF subRect(rf.X, rf.Y + rf.Height / 2 + 2,
                               rf.Width, rf.Height / 2 - 4);
        DrawLabel(g, L"Cancel", topRect,
                  L"Segoe UI", 13.0f, Gdiplus::FontStyleBold, cT);
        DrawLabel(g, armed ? L"scheduled action" : L"abort countdown",
                  subRect,
                  L"Segoe UI", 10.0f, Gdiplus::FontStyleRegular, cC);
    }
    // Stop Loop: writes stop-flag so the Stop hook halts auto-continue next turn.
    {
        Region r = c->layout.stopLoopBtn;
        bool hot = c->hoverStopLoop;
        bool active = false;
        for (const auto& s : c->sessions) if (s.loopEnabled) { active = true; break; }
        Gdiplus::RectF rf = r.RectF();
        Gdiplus::Color fillT = hot ? u.panelHi : u.panel;
        FillRoundGradient(g, rf, fillT, u.bgBottom, 10.0f);
        DrawRound(g, rf, active ? u.warn : (hot ? u.gripHot : u.divider),
                  10.0f, active ? 1.5f : 1.0f);
        Gdiplus::Color tColor = active ? u.text : u.textDim;
        Gdiplus::RectF topRect(rf.X, rf.Y + 4, rf.Width, rf.Height / 2 - 2);
        Gdiplus::RectF subRect(rf.X, rf.Y + rf.Height / 2 + 2,
                               rf.Width, rf.Height / 2 - 4);
        DrawLabel(g, L"Stop Loop", topRect,
                  L"Segoe UI", 13.0f, Gdiplus::FontStyleBold, tColor);
        DrawLabel(g, L"halt auto-continue", subRect,
                  L"Segoe UI", 10.0f, Gdiplus::FontStyleRegular, u.dim);
    }
    // Pause / Resume
    {
        Region r = c->layout.startBtn;
        bool hot = c->hoverStart;
        Gdiplus::RectF rf = r.RectF();
        if (c->paused) {
            FillRoundGradient(g, rf, u.accentDeep,
                              Gdiplus::Color(255,  8, 50, 82), 10.0f);
            DrawRound(g, rf, u.accent, 10.0f, 1.5f);
            DrawLabel(g, L"Resume", rf,
                     L"Segoe UI", 13.0f,
                     Gdiplus::FontStyleBold, u.text);
            DrawLabel(g, L"halt monitoring", rf,
                     L"Segoe UI", 10.0f,
                     Gdiplus::FontStyleRegular,
                     Gdiplus::Color(255, 220, 240, 255));
        } else {
            Gdiplus::Color fillT = hot ? u.panelHi : u.panel;
            FillRoundGradient(g, rf, fillT, u.bgBottom, 10.0f);
            DrawRound(g, rf, hot ? u.gripHot : u.divider, 10.0f, 1.0f);
            DrawLabel(g, L"Pause",
                     Gdiplus::RectF(rf.X, rf.Y + 4, rf.Width,
                                    rf.Height / 2 - 2),
                     L"Segoe UI", 13.0f,
                     Gdiplus::FontStyleBold, u.text);
            DrawLabel(g, L"halt monitoring",
                     Gdiplus::RectF(rf.X, rf.Y + rf.Height / 2 + 2,
                                    rf.Width, rf.Height / 2 - 4),
                     L"Segoe UI", 10.0f,
                     Gdiplus::FontStyleRegular, u.dim);
        }
    }

    // ---- Footer ----
    int footerY = H - 22;
    std::wstring foot = c->cfg.dryRun ? L"DRY-RUN" : L"LIVE";
    if (!c->doneMsg.empty()) {
        // Done message shown in footer instead of "primary:" path tail.
    } else if (c->sm.state == State::Countdown) {
        foot = std::to_wstring(c->sm.countdownRemaining) + L"s";
        foot += c->cfg.dryRun ? L"  ·  DRY-RUN" : L"  ·  LIVE";
    } else if (c->sm.state == State::IdleWaiting) {
        LONGLONG rem = (c->sm.idleDeadlineMs - (LONGLONG)now) / 1000;
        if (rem < 0) rem = 0;
        foot = std::to_wstring((int)rem) + L"s idle";
        foot += c->cfg.dryRun ? L"  ·  DRY-RUN" : L"  ·  LIVE";
    } else {
        foot += L"  ·  primary: " + c->primaryPath;
        size_t from = foot.size() > 60 ? foot.size() - 60 : 0;
        foot = (from > 0 ? L"…" : L"") + foot.substr(from);
    }
    DrawHairline(g, 0, footerY - 2, W, u.divider);
    DrawLabelLeft(g, foot.c_str(),
                 Gdiplus::RectF(20, (float)footerY, (float)(W - 40), 18),
                 L"Cascadia Mono", 10.5f,
                 Gdiplus::FontStyleRegular, u.muted);

    // ---- Grip ----
    DrawGrip(g, c->layout.grip, u, c->hoverGrip);

    // ---- Help overlay (modal; drawn last so it sits on top of everything) ----
    if (c->helpOpen) {
        PaintHelpOverlay(g, u, W, H);
    }

    // Blit.
    Gdiplus::Graphics sg(hdc);
    sg.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
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
            label = se->e.is_end_turn ? L"assistant end_turn"
                                       : L"assistant working";
        } else if (se->e.type == EvtType::User) {
            label = L"user message";
        } else if (se->e.type == EvtType::System) {
            label = se->e.is_api_error ? L"system api_error"
                                         : L"system event";
        }
        r.lastEventLabel = label;

        // Accumulate token usage. message.usage only appears on assistant
        // events with response bodies, so this only adds on those.
        r.tokInputTotal         += se->e.tokInput;
        r.tokOutputTotal        += se->e.tokOutput;
        r.tokCacheReadTotal     += se->e.tokCacheRead;
        r.tokCacheCreationTotal += se->e.tokCacheCreation;
    }
    c->doneMsg.clear();

    if (!c->paused && rowIdx >= 0 &&
        c->sessions[rowIdx].path == c->primaryPath) {
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
        std::vector<SessionRow> fresh;
        fresh.reserve(snap->size());
        for (const auto& ti : *snap) {
            int old = c->FindRow(ti.path);
            if (old >= 0) {
                fresh.push_back(c->sessions[old]);
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
                r.lastEventLabel = L"waiting…";
                fresh.push_back(std::move(r));
            }
        }
        c->sessions = std::move(fresh);
    }
    c->primaryPath = c->sessions.empty() ? L"" : c->sessions[0].path;
    c->PrunePrimaryIfGone();
    DLog(L"[sessions] snapshot: %zu active, primary=%s",
         c->sessions.size(), c->primaryPath.c_str());
    delete snap;
    InvalidateRect(hwnd, nullptr, FALSE);
}

void OnLoopStatus(HWND hwnd, LoopStatus* ls) {
    AppCtx* c = Ctx(hwnd);
    if (!c) { delete ls; return; }
    // Match the frame's sessionId against a row by path substring (the row's
    // path stem starts with the sessionId's first chars).
    if (!ls->sessionId.empty()) {
        for (auto& r : c->sessions) {
            if (r.path.find(ls->sessionId) != std::wstring::npos) {
                r.autoTurns = ls->autoTurns;
                r.loopEnabled = ls->loopEnabled;
                break;
            }
        }
    }
    delete ls;
    InvalidateRect(hwnd, nullptr, FALSE);
}

void OnTimerTick(HWND hwnd) {
    AppCtx* c = Ctx(hwnd);
    if (!c) return;

    if (!c->paused) {
        if (c->sm.state == State::Countdown ||
            c->sm.state == State::IdleWaiting) {
            bool zero = c->sm.OnTimer(c->cfg.countdownSec);
            DLog(L"[timer] state=%d countdown=%d zero=%d",
                 (int)c->sm.state, c->sm.countdownRemaining, zero ? 1 : 0);
            if (zero) {
                Action a = c->Selected();
                const wchar_t* msg = PerformPowerAction(a, c->cfg.dryRun);
                DLog(L"[power] action=%d dryRun=%d msg=%s",
                     (int)a, c->cfg.dryRun ? 1 : 0, msg ? msg : L"(null)");
                if (c->cfg.dryRun) {
                    c->doneMsg = std::wstring(L"DONE (dry-run) ")
                               + ActionLong(c->cfg.action);
                } else {
                    c->doneMsg = std::wstring(L"performed: ")
                               + (msg ? msg : L"");
                }
            }
        }
    }
    // Repaint every tick: drives the pulse animation + countdown progress.
    InvalidateRect(hwnd, nullptr, FALSE);
}

void OnMouseMove(HWND hwnd, int x, int y) {
    AppCtx* c = Ctx(hwnd);
    if (!c) return;
    bool hClose  = c->layout.closeBtn.Contains(x, y);
    bool hGrip   = c->layout.grip.Contains(x, y);
    bool hCancel = c->layout.cancelBtn.Contains(x, y);
    bool hStopLoop = c->layout.stopLoopBtn.Contains(x, y);
    bool hStart  = c->layout.startBtn.Contains(x, y);
    bool hHelp   = c->layout.helpBtn.Contains(x, y);
    int  hPill   = -1;
    for (int i = 0; i < 4; ++i)
        if (c->layout.pills[i].Contains(x, y)) { hPill = i; break; }
    bool dirty = (hClose  != c->hoverClose)  ||
                 (hGrip   != c->hoverGrip)   ||
                 (hCancel != c->hoverCancel) ||
                 (hStopLoop != c->hoverStopLoop) ||
                 (hStart  != c->hoverStart)  ||
                 (hHelp   != c->hoverHelp)   ||
                 (hPill   != c->hoverPill);
    if (dirty) {
        c->hoverClose  = hClose;
        c->hoverGrip   = hGrip;
        c->hoverCancel = hCancel;
        c->hoverStopLoop = hStopLoop;
        c->hoverStart  = hStart;
        c->hoverHelp   = hHelp;
        c->hoverPill   = hPill;
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    if (!c->trackingMouseLeave) {
        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd;
        TrackMouseEvent(&tme);
        c->trackingMouseLeave = true;
    }
}

void OnMouseLeave(HWND hwnd) {
    AppCtx* c = Ctx(hwnd);
    if (!c) return;
    bool dirty = c->hoverClose || c->hoverGrip ||
                 c->hoverCancel || c->hoverStopLoop || c->hoverStart ||
                 c->hoverHelp || c->hoverPill >= 0;
    c->hoverClose = c->hoverGrip = c->hoverCancel = c->hoverStopLoop =
        c->hoverStart = c->hoverHelp = false;
    c->hoverPill = -1;
    c->trackingMouseLeave = false;
    if (dirty) InvalidateRect(hwnd, nullptr, FALSE);
}

void OnLButtonDown(HWND hwnd, int x, int y) {
    AppCtx* c = Ctx(hwnd);
    if (!c) return;
    auto& L = c->layout;

    // Help overlay absorbs any click — it always dismisses regardless of
    // where the click lands (inside or outside the card). This is the
    // simplest possible "modal" pattern for a single-window popup app.
    if (c->helpOpen) {
        c->helpOpen = false;
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }

    if (L.closeBtn.Contains(x, y)) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return;
    }
    if (L.helpBtn.Contains(x, y)) {
        c->helpOpen = true;
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }
    for (int i = 0; i < 4; ++i) {
        if (L.pills[i].Contains(x, y)) {
            c->cfg.action = i;
            SaveCfg(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return;
        }
    }
    if (L.cancelBtn.Contains(x, y) &&
        c->sm.state == State::Countdown && !c->paused) {
        c->sm.Cancel();
        c->doneMsg.clear();
        InvalidateRect(hwnd, nullptr, FALSE);
        return;
    }
    if (L.stopLoopBtn.Contains(x, y)) {
        WriteStopFlag();
        c->doneMsg = L"stop-flag written · loop halts next turn";
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
            SetTimer(hwnd, 1, 250, nullptr);    // 4Hz for pulse + countdown bar
            AppCtx* c = Ctx(hwnd);
            if (c) c->pulseStartMs = GetTickCount64();
            return 0;
        }
        case WM_PAINT: Paint(hwnd); return 0;
        case WM_LBUTTONDOWN:
            OnLButtonDown(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        case WM_MOUSEMOVE:
            OnMouseMove(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            return 0;
        case WM_MOUSELEAVE:
            OnMouseLeave(hwnd);
            return 0;
        case WM_SETCURSOR: {
            if (LOWORD(lp) == HTCLIENT) {
                AppCtx* c = Ctx(hwnd);
                // IDC_* are macros that expand to LPWSTR; the cursor load
                // happens at SetCursor time.
                LPCWSTR id = IDC_ARROW;
                if (c) {
                    if (c->hoverGrip)                              id = IDC_SIZENWSE;
                    else if (c->hoverClose || c->hoverHelp ||
                             c->hoverPill >= 0 ||
                             c->hoverCancel || c->hoverStopLoop ||
                             c->hoverStart)                          id = IDC_HAND;
                }
                SetCursor(LoadCursor(nullptr, id));
                return TRUE;
            }
            return FALSE;
        }
        case WM_MOUSEWHEEL: {
            AppCtx* c = Ctx(hwnd);
            if (!c) return 0;
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            int step = -delta / WHEEL_DELTA;
            int maxOff = std::max<int>(0,
                (int)c->sessions.size() - c->layout.listVisibleRows);
            c->scrollOffset = std::max<int>(0,
                std::min<int>(maxOff, c->scrollOffset + step));
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_SIZE: {
            AppCtx* c = Ctx(hwnd);
            if (!c) return 0;
            int w = LOWORD(lp), h = HIWORD(lp);
            if (w <= 0 || h <= 0) return 0;
            c->layout.Recompute(w, h);
            int maxOff = std::max<int>(0,
                (int)c->sessions.size() - c->layout.listVisibleRows);
            if (c->scrollOffset > maxOff) c->scrollOffset = maxOff;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_TIMER:
            if (wp == 1) OnTimerTick(hwnd);
            return 0;
        case WM_KEYDOWN: {
            AppCtx* c = Ctx(hwnd);
            if (!c) return 0;
            // ESC dismisses the help overlay; F1 toggles it (universal help
            // key). Both are no-ops when the overlay is closed for F1 only
            // — ESC is unconditional close.
            if (wp == VK_ESCAPE && c->helpOpen) {
                c->helpOpen = false;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (wp == VK_F1) {
                c->helpOpen = !c->helpOpen;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            return 0;
        }
        case WM_APP_EVT:
            OnSessionEvent(hwnd, reinterpret_cast<SessionEvent*>(lp));
            return 0;
        case WM_APP_SESSIONS:
            OnSessionsUpdate(hwnd,
                reinterpret_cast<const std::vector<TrackInfo>*>(lp));
            return 0;
        case WM_APP_LOOPSTATUS:
            OnLoopStatus(hwnd, reinterpret_cast<LoopStatus*>(lp));
            return 0;
        case WM_NCHITTEST: {
            // Default for the entire client area is HTCLIENT — this routes
            // mouse clicks through WM_LBUTTONDOWN so OnLButtonDown can route
            // them to whatever they hit (close, help, pills, buttons).
            // Returning HTCLOSE here would route to WM_NCLBUTTONDOWN, where
            // nothing closes the window (DefWindowProc does NOT auto-close
            // on HTCLOSE) — that's why the close X was unresponsive.
            POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(hwnd, &p);
            AppCtx* c = Ctx(hwnd);
            if (c) {
                // Order matters: close + help are visually inside the
                // title strip, so test them before title. If we returned
                // HTCAPTION for either, clicks would start a window drag
                // instead of triggering the button.
                if (c->layout.closeBtn.Contains(p.x, p.y)) return HTCLIENT;
                if (c->layout.helpBtn.Contains(p.x, p.y))  return HTCLIENT;
                if (c->layout.grip.Contains(p.x, p.y))    return HTBOTTOMRIGHT;
                if (c->layout.title.Contains(p.x, p.y))   return HTCAPTION;
            }
            return HTCLIENT;
        }
        case WM_NCCALCSIZE: {
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

    // Opt in to system DPI awareness so GDI+ doesn't render the whole
    // surface as a small bitmap that gets scaled up on HiDPI displays
    // (the cause of the apparent fuzziness on 4K monitors).
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);

    auto ctx = std::make_unique<AppCtx>();
    ctx->cfg = initialCfg;
    ctx->exeDir = ExeDir();

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
    ctx->pipe.Start(hwnd);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    ctx->watcher.Stop();
    ctx->pipe.Stop();
    ctx.release();
    GdiShutdown();
}
