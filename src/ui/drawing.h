#pragma once
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

// Color palette. Tuned for a calm, cool-toned "night console" aesthetic:
// dark obsidian backdrop with a single cyan accent. The whole UI lives on
// this; treat it as the brand.
struct UIColors {
    // Backdrop gradient stops (top → bottom of the window).
    Gdiplus::Color bgTop    = Gdiplus::Color(255, 14, 16, 21);    // #0E1015
    Gdiplus::Color bgBottom = Gdiplus::Color(255, 22, 26, 34);    // #161A22

    // Surface elevations (panels get +1 elevation per step).
    Gdiplus::Color panel    = Gdiplus::Color(255, 27, 31, 40);
    Gdiplus::Color panelAlt = Gdiplus::Color(255, 33, 38, 48);
    Gdiplus::Color panelHi  = Gdiplus::Color(255, 43, 50, 62);

    // Text — three levels of emphasis.
    Gdiplus::Color text     = Gdiplus::Color(255, 240, 243, 247);
    Gdiplus::Color textDim  = Gdiplus::Color(255, 214, 222, 232);
    Gdiplus::Color muted    = Gdiplus::Color(255, 158, 166, 178);
    Gdiplus::Color dim      = Gdiplus::Color(255, 118, 126, 138);

    // Status / accents.
    Gdiplus::Color accent      = Gdiplus::Color(255, 125, 211, 252); // cyan-300
    Gdiplus::Color accentSoft  = Gdiplus::Color(255,  56,  96, 122);
    Gdiplus::Color accentDeep  = Gdiplus::Color(255,  12,  74, 110);
    Gdiplus::Color success     = Gdiplus::Color(255, 134, 239, 172); // emerald-300
    Gdiplus::Color warn        = Gdiplus::Color(255, 251, 191,  36); // amber-400
    Gdiplus::Color danger      = Gdiplus::Color(255, 248, 113, 113); // red-400

    // Structural.
    Gdiplus::Color divider  = Gdiplus::Color(255,  49,  56,  66);
    Gdiplus::Color grip     = Gdiplus::Color(255,  70,  78,  92);
    Gdiplus::Color gripHot  = Gdiplus::Color(255, 110, 130, 160);
};

void GdiInit();
void GdiShutdown();

// ----- Rect / shape helpers -----

// Fill a vertical gradient into an integer-aligned Rect.
void FillVGradient(Gdiplus::Graphics& g, const Gdiplus::Rect& r,
                   const Gdiplus::Color& top, const Gdiplus::Color& bottom);

// Fill a rounded rectangle with a vertical gradient (background → bottom).
void FillRoundGradient(Gdiplus::Graphics& g, const Gdiplus::RectF& r,
                       const Gdiplus::Color& top, const Gdiplus::Color& bottom,
                       float radius);

// Plain rounded-rect fill (no gradient).
void FillRound(Gdiplus::Graphics& g, const Gdiplus::RectF& r,
               const Gdiplus::Color& c, float radius);

// Rounded-rect outline (1px hairline, snapped to pixel grid).
void DrawRound(Gdiplus::Graphics& g, const Gdiplus::RectF& r,
               const Gdiplus::Color& c, float radius, float width);

// Crisp horizontal hairline divider at integer y.
void DrawHairline(Gdiplus::Graphics& g, int x, int y, int w,
                  const Gdiplus::Color& c);

// Faint scanline texture (1px every `period` rows, top-down through the rect).
void DrawScanlines(Gdiplus::Graphics& g, const Gdiplus::Rect& r,
                   const Gdiplus::Color& c, int period);

// Standard interactive "pill" tile: gradient fill, hairline border,
// cyan border when selected, panelHi when hovered, panel when idle.
void DrawPill(Gdiplus::Graphics& g, const Gdiplus::RectF& r,
              bool selected, bool hover);

// ----- Text -----

// Font factory (creates a font with explicit pixel-unit size; the trailing
// "UnitPixel" is what avoids the device-default unit mess when the system DPI
// isn't perfectly tuned).
Gdiplus::Font MakeFont(const wchar_t* name, float size,
                       int style = Gdiplus::FontStyleRegular);

// Centred text inside r using AntiAliasGridFit — sharper than ClearType on
// non-ClearType displays and avoids color fringing on small text.
void DrawLabel(Gdiplus::Graphics& g, const wchar_t* text,
               const Gdiplus::RectF& r,
               const wchar_t* fontName, float size, int style,
               const Gdiplus::Color& color,
               Gdiplus::StringAlignment align = Gdiplus::StringAlignmentCenter,
               Gdiplus::StringAlignment lineAlign = Gdiplus::StringAlignmentCenter);

// Left-aligned text in r (x acts as a left margin).
void DrawLabelLeft(Gdiplus::Graphics& g, const wchar_t* text,
                   const Gdiplus::RectF& r,
                   const wchar_t* fontName, float size, int style,
                   const Gdiplus::Color& color);

// ----- Path util (internal to drawing.cpp; tests in window.cpp see it) -----
void RoundPath(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& r, float d);
