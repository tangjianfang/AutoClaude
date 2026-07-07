#include "drawing.h"

static ULONG_PTR gdiToken = 0;

void GdiInit() {
    Gdiplus::GdiplusStartupInput si;
    Gdiplus::GdiplusStartup(&gdiToken, &si, nullptr);
}
void GdiShutdown() {
    if (gdiToken) { Gdiplus::GdiplusShutdown(gdiToken); gdiToken = 0; }
}

Gdiplus::Font MakeFont(const wchar_t* name, float size, int style) {
    // UnitPixel keeps the font size stable regardless of the host's default
    // DPI. Without it, GDI+ multiplies by the display's "DpiY/96" and your
    // 9.5px font suddenly becomes 14px on a HiDPI screen.
    return Gdiplus::Font(name, size, style, Gdiplus::UnitPixel);
}

void FillVGradient(Gdiplus::Graphics& g, const Gdiplus::Rect& r,
                   const Gdiplus::Color& top, const Gdiplus::Color& bottom) {
    Gdiplus::LinearGradientBrush br(r, top, bottom,
                                    Gdiplus::LinearGradientModeVertical);
    g.FillRectangle(&br, r);
}

void RoundPath(Gdiplus::GraphicsPath& path,
               const Gdiplus::RectF& r, float d) {
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    path.CloseFigure();
}

void FillRound(Gdiplus::Graphics& g, const Gdiplus::RectF& r,
               const Gdiplus::Color& c, float radius) {
    Gdiplus::GraphicsPath path;
    float d = radius * 2;
    if (d > r.Width)  d = r.Width;
    if (d > r.Height) d = r.Height;
    RoundPath(path, r, d);
    Gdiplus::SolidBrush b(c);
    g.FillPath(&b, &path);
}

void FillRoundGradient(Gdiplus::Graphics& g, const Gdiplus::RectF& r,
                       const Gdiplus::Color& top, const Gdiplus::Color& bottom,
                       float radius) {
    Gdiplus::GraphicsPath path;
    float d = radius * 2;
    if (d > r.Width)  d = r.Width;
    if (d > r.Height) d = r.Height;
    RoundPath(path, r, d);
    Gdiplus::Rect bounds((INT)r.X, (INT)r.Y,
                         (INT)(r.Width + 0.5f), (INT)(r.Height + 0.5f));
    Gdiplus::LinearGradientBrush br(bounds, top, bottom,
                                    Gdiplus::LinearGradientModeVertical);
    g.FillPath(&br, &path);
}

void DrawRound(Gdiplus::Graphics& g, const Gdiplus::RectF& r,
               const Gdiplus::Color& c, float radius, float width) {
    Gdiplus::GraphicsPath path;
    float d = radius * 2;
    if (d > r.Width)  d = r.Width;
    if (d > r.Height) d = r.Height;
    RoundPath(path, r, d);
    Gdiplus::Pen p(c, width);
    // Snap the pen alignment so 1px strokes land squarely on a pixel row.
    p.SetAlignment(Gdiplus::PenAlignmentInset);
    g.DrawPath(&p, &path);
}

void DrawHairline(Gdiplus::Graphics& g, int x, int y, int w,
                  const Gdiplus::Color& c) {
    Gdiplus::Pen p(c, 1.0f);
    p.SetAlignment(Gdiplus::PenAlignmentInset);
    g.DrawLine(&p,
        Gdiplus::PointF((float)x,         (float)y),
        Gdiplus::PointF((float)(x + w),   (float)y));
}

void DrawScanlines(Gdiplus::Graphics& g, const Gdiplus::Rect& r,
                   const Gdiplus::Color& c, int period) {
    if (period <= 0) return;
    Gdiplus::Pen p(c, 1.0f);
    p.SetAlignment(Gdiplus::PenAlignmentInset);
    for (int y = r.Y; y < r.GetBottom(); y += period) {
        g.DrawLine(&p,
            Gdiplus::PointF((float)r.X,           (float)y),
            Gdiplus::PointF((float)r.GetRight(),  (float)y));
    }
}

void DrawPill(Gdiplus::Graphics& g, const Gdiplus::RectF& r,
              bool selected, bool hover) {
    UIColors u;
    Gdiplus::Color fillTop = selected ? u.accentDeep
                       : hover   ? u.panelHi
                                 : u.panel;
    Gdiplus::Color fillBot = selected ? Gdiplus::Color(255,  8, 50,  82)
                       : hover   ? Gdiplus::Color(255, 35, 41,  52)
                                 : Gdiplus::Color(255, 22, 26,  34);
    FillRoundGradient(g, r, fillTop, fillBot, 10.0f);

    Gdiplus::Color border = selected ? u.accent
                       : hover     ? u.gripHot
                                   : u.divider;
    DrawRound(g, r, border, 10.0f, selected ? 1.5f : 1.0f);
}

void DrawLabel(Gdiplus::Graphics& g, const wchar_t* text,
               const Gdiplus::RectF& r,
               const wchar_t* fontName, float size, int style,
               const Gdiplus::Color& color,
               Gdiplus::StringAlignment align,
               Gdiplus::StringAlignment lineAlign) {
    Gdiplus::Font font = MakeFont(fontName, size, style);
    Gdiplus::StringFormat sf;
    sf.SetAlignment(align);
    sf.SetLineAlignment(lineAlign);
    // AntiAliasGridFit: anti-aliased glyphs with horizontal hinting. Looks
    // sharper than ClearTypeGridFit on displays without RGB subpixel
    // configuration and avoids the cyan/magenta fringes ClearType produces
    // on a saturated dark UI.
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    Gdiplus::SolidBrush b(color);
    g.DrawString(text, -1, &font, r, &sf, &b);
}

void DrawLabelLeft(Gdiplus::Graphics& g, const wchar_t* text,
                   const Gdiplus::RectF& r,
                   const wchar_t* fontName, float size, int style,
                   const Gdiplus::Color& color) {
    Gdiplus::Font font = MakeFont(fontName, size, style);
    Gdiplus::StringFormat sf;
    sf.SetAlignment(Gdiplus::StringAlignmentNear);
    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    Gdiplus::SolidBrush b(color);
    g.DrawString(text, -1, &font, r, &sf, &b);
}
