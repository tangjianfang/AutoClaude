#pragma once
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

struct UIColors {
    Gdiplus::Color bg     = Gdiplus::Color(255, 24, 24, 28);
    Gdiplus::Color bgAlt  = Gdiplus::Color(255, 32, 32, 38);
    Gdiplus::Color text   = Gdiplus::Color(255, 235, 235, 235);
    Gdiplus::Color muted  = Gdiplus::Color(255, 150, 150, 160);
    Gdiplus::Color accent = Gdiplus::Color(255, 0, 120, 212);
    Gdiplus::Color danger = Gdiplus::Color(255, 232, 72, 72);
};

void GdiInit();
void GdiShutdown();

// Fill a rounded-rect path.
void FillRound(Gdiplus::Graphics& g, const Gdiplus::RectF& r,
               const Gdiplus::Color& c, float radius);
void DrawRound(Gdiplus::Graphics& g, const Gdiplus::RectF& r,
               const Gdiplus::Color& c, float radius, float width);

// Centered text inside r.
void DrawCentered(Gdiplus::Graphics& g, const wchar_t* text,
                  const Gdiplus::RectF& r, const Gdiplus::Font& font,
                  const Gdiplus::Color& color);