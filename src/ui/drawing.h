#pragma once
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

// Color palette. Tuned for a "luminous slate" aesthetic: the background
// is dark enough to be unobtrusive but bright enough to read in a
// well-lit room, with a vivid cyan accent that pops against the slate.
struct UIColors {
    // Backdrop gradient stops (slate, lifted enough to be readable in
    // daylight instead of "obsidian dark").
    Gdiplus::Color bgTop    = Gdiplus::Color(255,  44,  52,  68);   // #2C3444
    Gdiplus::Color bgBottom = Gdiplus::Color(255,  58,  68,  88);   // #3A4458

    // Surface elevations (each step is visibly raised against the bg).
    Gdiplus::Color panel    = Gdiplus::Color(255,  78,  88, 110);   // #4E586E
    Gdiplus::Color panelAlt = Gdiplus::Color(255,  92, 102, 124);   // #5C667C
    Gdiplus::Color panelHi  = Gdiplus::Color(255, 110, 122, 148);   // #6E7A94

    // Text — three levels of emphasis (much brighter than v5).
    Gdiplus::Color text     = Gdiplus::Color(255, 248, 250, 252);   // #F8FAFC
    Gdiplus::Color textDim  = Gdiplus::Color(255, 218, 226, 238);   // #DAE2EE
    Gdiplus::Color muted    = Gdiplus::Color(255, 178, 188, 204);   // #B2BCCC
    Gdiplus::Color dim      = Gdiplus::Color(255, 142, 152, 170);   // #8E98AA

    // Status / accents (saturated, vivid).
    Gdiplus::Color accent      = Gdiplus::Color(255,  56, 189, 248); // #38BDF8 (sky-400)
    Gdiplus::Color accentSoft  = Gdiplus::Color(255, 125, 211, 252); // #7DD3FC (cyan-300, glow)
    Gdiplus::Color accentDeep  = Gdiplus::Color(255,  14, 116, 144); // #0E7490 (cyan-700)
    Gdiplus::Color success     = Gdiplus::Color(255,  52, 211, 153); // #34D399 (emerald-400)
    Gdiplus::Color warn        = Gdiplus::Color(255, 250, 204,  21); // #FACC15 (amber-400)
    Gdiplus::Color danger      = Gdiplus::Color(255, 251, 113, 133); // #FB7185 (rose-400)

    // Structural.
    Gdiplus::Color divider  = Gdiplus::Color(255,  92, 104, 128);   // #5C6880
    Gdiplus::Color grip     = Gdiplus::Color(255, 124, 138, 164);   // #7C8AA4
    Gdiplus::Color gripHot  = Gdiplus::Color(255, 162, 178, 210);
};

void GdiInit();
void GdiShutdown();

// ----- Rect / shape helpers -----

void FillVGradient(Gdiplus::Graphics& g, const Gdiplus::Rect& r,
                   const Gdiplus::Color& top, const Gdiplus::Color& bottom);

void FillRoundGradient(Gdiplus::Graphics& g, const Gdiplus::RectF& r,
                       const Gdiplus::Color& top, const Gdiplus::Color& bottom,
                       float radius);

void FillRound(Gdiplus::Graphics& g, const Gdiplus::RectF& r,
               const Gdiplus::Color& c, float radius);

void DrawRound(Gdiplus::Graphics& g, const Gdiplus::RectF& r,
               const Gdiplus::Color& c, float radius, float width);

void DrawHairline(Gdiplus::Graphics& g, int x, int y, int w,
                  const Gdiplus::Color& c);

void DrawScanlines(Gdiplus::Graphics& g, const Gdiplus::Rect& r,
                   const Gdiplus::Color& c, int period);

void DrawPill(Gdiplus::Graphics& g, const Gdiplus::RectF& r,
              bool selected, bool hover);

Gdiplus::Font MakeFont(const wchar_t* name, float size,
                       int style = Gdiplus::FontStyleRegular);

void DrawLabel(Gdiplus::Graphics& g, const wchar_t* text,
               const Gdiplus::RectF& r,
               const wchar_t* fontName, float size, int style,
               const Gdiplus::Color& color,
               Gdiplus::StringAlignment align = Gdiplus::StringAlignmentCenter,
               Gdiplus::StringAlignment lineAlign = Gdiplus::StringAlignmentCenter);

void DrawLabelLeft(Gdiplus::Graphics& g, const wchar_t* text,
                   const Gdiplus::RectF& r,
                   const wchar_t* fontName, float size, int style,
                   const Gdiplus::Color& color);

void RoundPath(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& r, float d);
