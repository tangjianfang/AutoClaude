#include "drawing.h"

static ULONG_PTR gdiToken = 0;

void GdiInit() {
    Gdiplus::GdiplusStartupInput si;
    Gdiplus::GdiplusStartup(&gdiToken, &si, nullptr);
}
void GdiShutdown() {
    if (gdiToken) { Gdiplus::GdiplusShutdown(gdiToken); gdiToken = 0; }
}

static void RoundPath(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& r, float d) {
    float x = r.X, y = r.Y, w = r.Width, h = r.Height;
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
}

void FillRound(Gdiplus::Graphics& g, const Gdiplus::RectF& r,
               const Gdiplus::Color& c, float radius) {
    Gdiplus::GraphicsPath path;
    float d = radius * 2;
    if (d > r.Width) d = r.Width;
    if (d > r.Height) d = r.Height;
    RoundPath(path, r, d);
    Gdiplus::SolidBrush b(c);
    g.FillPath(&b, &path);
}

void DrawRound(Gdiplus::Graphics& g, const Gdiplus::RectF& r,
               const Gdiplus::Color& c, float radius, float width) {
    Gdiplus::GraphicsPath path;
    float d = radius * 2;
    if (d > r.Width) d = r.Width;
    if (d > r.Height) d = r.Height;
    RoundPath(path, r, d);
    Gdiplus::Pen p(c, width);
    g.DrawPath(&p, &path);
}

void DrawCentered(Gdiplus::Graphics& g, const wchar_t* text,
                  const Gdiplus::RectF& r, const Gdiplus::Font& font,
                  const Gdiplus::Color& color) {
    Gdiplus::StringFormat sf;
    sf.SetAlignment(Gdiplus::StringAlignmentCenter);
    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    Gdiplus::SolidBrush b(color);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
    g.DrawString(text, -1, &font, r, &sf, &b);
}