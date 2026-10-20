// Mouse Trail for Windows — a smooth trail behind the mouse cursor.
// Single-file Win32 + GDI+ app: tray icon, transparent click-through
// overlay across all monitors. No dependencies, no installer.
//
// Windows port of https://github.com/changymon/MouseTrail

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <commdlg.h>
#include <vector>
#include <cmath>

using namespace Gdiplus;

// ---------------------------------------------------------------- settings

enum TrailStyle { STYLE_LINE = 0, STYLE_COMET = 1, STYLE_COUNT = 2 };

struct Settings {
    int      style      = STYLE_LINE;
    COLORREF color      = RGB(255, 0, 0);
    int      thickness  = 6;      // px
    int      durationMs = 600;    // trail fade time
    bool     welcomed   = false;
};
static Settings g_settings;

static const wchar_t* kRegKey = L"Software\\MouseTrail";

static DWORD RegGetDW(const wchar_t* name, DWORD def) {
    DWORD value = def, size = sizeof(value), type = 0;
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_READ, &key) == ERROR_SUCCESS) {
        if (RegQueryValueExW(key, name, nullptr, &type, (LPBYTE)&value, &size) != ERROR_SUCCESS
            || type != REG_DWORD) {
            value = def;
        }
        RegCloseKey(key);
    }
    return value;
}

static void RegSetDW(const wchar_t* name, DWORD value) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_WRITE,
                        nullptr, &key, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE*)&value, sizeof(value));
        RegCloseKey(key);
    }
}

static void LoadSettings() {
    g_settings.style      = (int)RegGetDW(L"Style", STYLE_LINE);
    if (g_settings.style < 0 || g_settings.style >= STYLE_COUNT) g_settings.style = STYLE_LINE;
    //? New value name so a color saved by older versions doesn't override the red default.
    g_settings.color      = (COLORREF)RegGetDW(L"Color", RGB(255, 0, 0));
    g_settings.thickness  = (int)RegGetDW(L"Thickness", 6);
    g_settings.durationMs = (int)RegGetDW(L"DurationMs", 600);
    g_settings.welcomed   = RegGetDW(L"Welcomed", 0) != 0;
}

static void SaveSettings() {
    RegSetDW(L"Style", g_settings.style);
    RegSetDW(L"Color", g_settings.color);
    RegSetDW(L"Thickness", g_settings.thickness);
    RegSetDW(L"DurationMs", g_settings.durationMs);
    RegSetDW(L"Welcomed", g_settings.welcomed ? 1 : 0);
}

// ------------------------------------------------------------- trail model

struct TrailPt { float x, y; double t; };

static std::vector<TrailPt> g_points;
static const float    kMinPointSpacing = 4.0f;   // px between fixed trail points
static MOUSEMOVEPOINT g_lastMovePt = {};    // newest mouse-history entry already in g_points (raw form)
static bool           g_haveMovePt = false;

static double Now() {
    static LARGE_INTEGER freq = {};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)freq.QuadPart;
}

/**
 * Appends a trail point unless it repeats the last point's position.
 * Shared by the mouse-history path and the GetCursorPos fallback.
 * @param x screen x in px
 * @param y screen y in px
 * @param t timestamp on the Now() clock; clamped so g_points stays time-ordered
 */
static void PushTrailPoint(float x, float y, double t) {
    TrailPt p = { x, y, t };
    size_t n = g_points.size();
    if (n > 0) {
        TrailPt& last = g_points.back();
        float dx = x - last.x, dy = y - last.y;
        if (dx * dx + dy * dy <= 0.25f) return;
        if (t < last.t) p.t = last.t;

        //? High polling rates move in 1-2 px integer steps, which the spline traced as a
        //? staircase. The newest point slides with the cursor until it is kMinPointSpacing
        //? away from the one before it, and only then becomes fixed.
        if (n >= 2) {
            const TrailPt& prev = g_points[n - 2];
            float px = last.x - prev.x, py = last.y - prev.y;
            if (px * px + py * py < kMinPointSpacing * kMinPointSpacing) {
                last = p;
                return;
            }
        }
    }
    g_points.push_back(p);
}

/**
 * Appends every mouse position recorded by Windows since the last tick to g_points,
 * and updates g_lastMovePt / g_haveMovePt to remember where it stopped.
 * The timer only fires at ~64 Hz, so sampling GetCursorPos alone turned fast curves into polygons.
 * @param cursor current cursor position; used alone when the history is unavailable
 * @param now current time on the Now() clock
 */
static void AppendMovePoints(POINT cursor, double now) {
    MOUSEMOVEPOINT query = {};
    query.x = cursor.x & 0xFFFF;
    query.y = cursor.y & 0xFFFF;
    MOUSEMOVEPOINT history[64];
    int count = GetMouseMovePointsEx(sizeof(query), &query, history, 64, GMMP_USE_DISPLAY_POINTS);
    if (count <= 0) {
        //? Cursor moved by SetCursorPos or the history lookup failed: plain sampling.
        g_haveMovePt = false;
        PushTrailPoint((float)cursor.x, (float)cursor.y, now);
        return;
    }

    //? history[0] is the newest entry; find the one we stopped at last tick.
    int fresh = 1;
    if (g_haveMovePt) {
        fresh = count;
        for (int i = 0; i < count; i++) {
            if (history[i].x == g_lastMovePt.x && history[i].y == g_lastMovePt.y
                && history[i].time == g_lastMovePt.time) {
                fresh = i;
                break;
            }
        }
    }

    DWORD tickNow = GetTickCount();
    for (int i = fresh - 1; i >= 0; i--) {
        int x = history[i].x, y = history[i].y;
        if (x > 32767) x -= 65536;          // negative coords on multi-monitor setups
        if (y > 32767) y -= 65536;
        LONG ageMs = (LONG)(tickNow - history[i].time);
        if (ageMs < 0) ageMs = 0;
        PushTrailPoint((float)x, (float)y, now - ageMs / 1000.0);
    }
    g_lastMovePt = history[0];
    g_haveMovePt = true;
}

static void TickModel() {
    double now = Now();
    POINT cursor;
    GetCursorPos(&cursor);
    AppendMovePoints(cursor, now);

    double lifetime = g_settings.durationMs / 1000.0;
    size_t keep = 0;
    for (size_t i = 0; i < g_points.size(); i++) {
        if (now - g_points[i].t <= lifetime) g_points[keep++] = g_points[i];
    }
    g_points.resize(keep);
}

// ------------------------------------------------------------------ colors

static BYTE AlphaByte(double a) {
    if (a < 0) a = 0;
    if (a > 1) a = 1;
    return (BYTE)(a * 255 + 0.5);
}

// Trail color with fade/whitening applied.
static Color TrailColor(double alpha, float whiten) {
    BYTE r = GetRValue(g_settings.color);
    BYTE g = GetGValue(g_settings.color);
    BYTE b = GetBValue(g_settings.color);
    if (whiten > 0) {
        r = (BYTE)(r + (255 - r) * whiten);
        g = (BYTE)(g + (255 - g) * whiten);
        b = (BYTE)(b + (255 - b) * whiten);
    }
    return Color(AlphaByte(alpha), r, g, b);
}

// ----------------------------------------------------------------- overlay

static HWND      g_overlay = nullptr;
static HWND      g_trayWnd = nullptr;
static HDC       g_memDC = nullptr;
static HBITMAP   g_dib = nullptr, g_dibOld = nullptr;
static void*     g_dibBits = nullptr;
static Bitmap*   g_bitmap = nullptr;
static Graphics* g_graphics = nullptr;
static int       g_vx, g_vy, g_vw, g_vh;   // virtual screen bounds
static bool      g_lastFrameEmpty = true;
static RECT      g_drawnRect = {};         // surface area holding last frame's pixels
static RECT      g_frameRect = {};         // surface area drawn so far this frame
static bool      g_fullUpdate = true;      // next frame must push the whole surface

/**
 * Grows g_frameRect to cover the given area, clipped to the surface.
 * Lets RenderFrame clear and push only the pixels the trail touched.
 * @param x0 left edge, inclusive
 * @param y0 top edge, inclusive
 * @param x1 right edge, exclusive
 * @param y1 bottom edge, exclusive
 */
static void MarkDrawn(int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_vw) x1 = g_vw;
    if (y1 > g_vh) y1 = g_vh;
    if (x1 <= x0 || y1 <= y0) return;
    RECT area = { x0, y0, x1, y1 };
    UnionRect(&g_frameRect, &g_frameRect, &area);
}

static void DestroySurface() {
    delete g_graphics; g_graphics = nullptr;
    delete g_bitmap;   g_bitmap = nullptr;
    if (g_memDC) {
        SelectObject(g_memDC, g_dibOld);
        DeleteObject(g_dib);
        DeleteDC(g_memDC);
        g_memDC = nullptr; g_dib = nullptr; g_dibBits = nullptr;
    }
}

static bool CreateSurface() {
    DestroySurface();
    g_vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g_vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    g_vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g_vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (g_vw <= 0 || g_vh <= 0) return false;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = g_vw;
    bmi.bmiHeader.biHeight = -g_vh;       // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    g_memDC = CreateCompatibleDC(nullptr);
    g_dib = CreateDIBSection(g_memDC, &bmi, DIB_RGB_COLORS, &g_dibBits, nullptr, 0);
    if (!g_dib) { DeleteDC(g_memDC); g_memDC = nullptr; return false; }
    g_dibOld = (HBITMAP)SelectObject(g_memDC, g_dib);
    ZeroMemory(g_dibBits, (size_t)g_vw * g_vh * 4);
    SetRectEmpty(&g_drawnRect);
    g_fullUpdate = true;

    g_bitmap = new Bitmap(g_vw, g_vh, g_vw * 4, PixelFormat32bppPARGB, (BYTE*)g_dibBits);
    g_graphics = new Graphics(g_bitmap);
    g_graphics->SetSmoothingMode(SmoothingModeAntiAlias);

    SetWindowPos(g_overlay, HWND_TOPMOST, g_vx, g_vy, g_vw, g_vh,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return true;
}

static PointF Local(float x, float y) {
    return PointF(x - (float)g_vx, y - (float)g_vy);
}

// --------------------------------------------------------- trail raster

static const float kSmoothSpacing = 3.0f;  // px between resampled spline points
static const int   kSmoothPasses  = 2;     // 1-2-1 averaging passes before the spline

static std::vector<TrailPt> g_smooth;      // g_points resampled along the spline
static std::vector<DWORD>   g_scratch;     // premultiplied BGRA layer; all-zero between passes
static std::vector<int>     g_spanMin, g_spanMax;

/**
 * Evaluates one coordinate of a Catmull-Rom segment (Barry-Goldman form) between p1 and p2.
 * Non-uniform knots let the caller use centripetal spacing, which avoids loops and overshoot.
 * @param p0 coordinate of the point before the segment
 * @param p1 coordinate of the segment start
 * @param p2 coordinate of the segment end
 * @param p3 coordinate of the point after the segment
 * @param t0 knot of p0
 * @param t1 knot of p1
 * @param t2 knot of p2
 * @param t3 knot of p3
 * @param t evaluation knot, in [t1, t2]
 * @return the interpolated coordinate
 */
static float CatmullRom(float p0, float p1, float p2, float p3,
                        float t0, float t1, float t2, float t3, float t) {
    float a1 = ((t1 - t) * p0 + (t - t0) * p1) / (t1 - t0);
    float a2 = ((t2 - t) * p1 + (t - t1) * p2) / (t2 - t1);
    float a3 = ((t3 - t) * p2 + (t - t2) * p3) / (t3 - t2);
    float b1 = ((t2 - t) * a1 + (t - t0) * a2) / (t2 - t0);
    float b2 = ((t3 - t) * a2 + (t - t1) * a3) / (t3 - t1);
    return ((t2 - t) * b1 + (t - t1) * b2) / (t2 - t1);
}

/**
 * Centripetal knot distance between two points.
 * Clamped above zero so coincident points can't divide by zero in CatmullRom.
 * @param a first point
 * @param b second point
 * @return sqrt of the distance between a and b
 */
static float KnotStep(const TrailPt& a, const TrailPt& b) {
    float dx = b.x - a.x, dy = b.y - a.y;
    float step = powf(dx * dx + dy * dy, 0.25f);
    return step < 1e-3f ? 1e-3f : step;
}

/**
 * Copies g_points into `out` and averages each inner point with its neighbours (1-2-1 kernel).
 * Rounds off the pixel-step bumps left in the raw mouse path; the first and last point stay
 * fixed so the trail still starts exactly at the cursor.
 * @param out cleared, then filled with the smoothed copy of g_points
 * @param passes number of smoothing passes; more passes give a rounder but less exact path
 */
static void SmoothControlPoints(std::vector<TrailPt>& out, int passes) {
    out = g_points;
    size_t n = out.size();
    for (int pass = 0; pass < passes; pass++) {
        TrailPt before = out.empty() ? TrailPt{} : out[0];
        for (size_t i = 1; i + 1 < n; i++) {
            TrailPt current = out[i];
            out[i].x = (before.x + 2 * current.x + out[i + 1].x) * 0.25f;
            out[i].y = (before.y + 2 * current.y + out[i + 1].y) * 0.25f;
            before = current;
        }
    }
}

/**
 * Resamples the smoothed g_points along a centripetal Catmull-Rom spline.
 * Straight lines between raw samples showed up as corners on fast curves.
 * @param out cleared, then filled with points about `spacing` px apart; times are interpolated linearly
 * @param spacing target distance between output points, in px
 */
static void BuildSmoothTrail(std::vector<TrailPt>& out, float spacing) {
    static std::vector<TrailPt> control;
    SmoothControlPoints(control, kSmoothPasses);
    out.clear();
    size_t n = control.size();
    if (n < 2) {
        out = control;
        return;
    }
    for (size_t i = 0; i + 1 < n; i++) {
        const TrailPt& p1 = control[i];
        const TrailPt& p2 = control[i + 1];
        //? Mirror the neighbours at both ends so the curve still reaches the first and last point.
        TrailPt p0 = i > 0 ? control[i - 1] : TrailPt{ 2 * p1.x - p2.x, 2 * p1.y - p2.y, p1.t };
        TrailPt p3 = i + 2 < n ? control[i + 2] : TrailPt{ 2 * p2.x - p1.x, 2 * p2.y - p1.y, p2.t };

        float t0 = 0;
        float t1 = t0 + KnotStep(p0, p1);
        float t2 = t1 + KnotStep(p1, p2);
        float t3 = t2 + KnotStep(p2, p3);

        float dx = p2.x - p1.x, dy = p2.y - p1.y;
        int steps = (int)ceilf(sqrtf(dx * dx + dy * dy) / spacing);
        if (steps < 1) steps = 1;
        if (steps > 64) steps = 64;
        for (int s = 0; s < steps; s++) {
            float u = (float)s / steps;
            float k = t1 + (t2 - t1) * u;
            TrailPt p;
            p.x = CatmullRom(p0.x, p1.x, p2.x, p3.x, t0, t1, t2, t3, k);
            p.y = CatmullRom(p0.y, p1.y, p2.y, p3.y, t0, t1, t2, t3, k);
            p.t = p1.t + (p2.t - p1.t) * u;
            out.push_back(p);
        }
    }
    out.push_back(control[n - 1]);
}

struct TrailPass {
    float widthScale, alphaScale, whiten, minWidth;
    bool  taper;                           // width follows fade² instead of fade
};

/**
 * Trail width at a given fade for one pass.
 * @param pass the pass whose width settings apply
 * @param fade 1 at the cursor, 0 at the end of the trail
 * @return stroke width in px
 */
static float PassWidth(const TrailPass& pass, float fade) {
    float width = g_settings.thickness * pass.widthScale * (pass.taper ? fade * fade : fade);
    return width < pass.minWidth ? pass.minWidth : width;
}

/**
 * Blends a premultiplied BGRA pixel over another (source-over).
 * @param src premultiplied source pixel
 * @param dst premultiplied destination pixel
 * @return the blended pixel
 */
static DWORD BlendOver(DWORD src, DWORD dst) {
    DWORD inv = 255 - (src >> 24);
    DWORD out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        DWORD channel = ((src >> shift) & 0xFF) + (((dst >> shift) & 0xFF) * inv + 127) / 255;
        out |= channel << shift;
    }
    return out;
}

/**
 * Rasterizes the trail as one continuous stroke and blends it over the DIB in g_dibBits.
 * Each pixel keeps the strongest coverage of any segment touching it, instead of stacking alphas,
 * so joints no longer show darker dots. Uses g_scratch as a temporary layer and leaves it zeroed.
 * @param pts dense trail points, oldest first
 * @param now current time on the Now() clock
 * @param lifetime trail fade duration in seconds
 * @param pass width, alpha and whitening for this stroke
 */
static void DrawTrailPass(const std::vector<TrailPt>& pts, double now, double lifetime,
                          const TrailPass& pass) {
    if (pts.size() < 2) return;

    //* Bounding box of the whole stroke, in surface pixels.
    float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
    for (const TrailPt& p : pts) {
        float fade = (float)(1.0 - (now - p.t) / lifetime);
        if (fade <= 0.01f) continue;
        float reach = PassWidth(pass, fade) * 0.5f + 1.0f;
        float x = p.x - g_vx, y = p.y - g_vy;
        if (x - reach < minX) minX = x - reach;
        if (y - reach < minY) minY = y - reach;
        if (x + reach > maxX) maxX = x + reach;
        if (y + reach > maxY) maxY = y + reach;
    }
    int bx0 = (int)floorf(minX), by0 = (int)floorf(minY);
    int bx1 = (int)ceilf(maxX),  by1 = (int)ceilf(maxY);
    if (bx0 < 0) bx0 = 0;
    if (by0 < 0) by0 = 0;
    if (bx1 > g_vw - 1) bx1 = g_vw - 1;
    if (by1 > g_vh - 1) by1 = g_vh - 1;
    if (bx1 < bx0 || by1 < by0) return;
    int bw = bx1 - bx0 + 1, bh = by1 - by0 + 1;
    MarkDrawn(bx0, by0, bx1 + 1, by1 + 1);

    if (g_scratch.size() < (size_t)bw * bh) g_scratch.resize((size_t)bw * bh, 0);
    g_spanMin.assign(bh, bw);
    g_spanMax.assign(bh, -1);

    Color color = TrailColor(1, pass.whiten);
    DWORD cr = color.GetR(), cg = color.GetG(), cb = color.GetB();

    //* Max-coverage pass into the scratch layer.
    for (size_t i = 1; i < pts.size(); i++) {
        const TrailPt& a = pts[i - 1];
        const TrailPt& b = pts[i];
        float fa = (float)(1.0 - (now - a.t) / lifetime);
        float fb = (float)(1.0 - (now - b.t) / lifetime);
        if (fa <= 0.01f && fb <= 0.01f) continue;

        float ax = a.x - g_vx - bx0, ay = a.y - g_vy - by0;
        float dx = b.x - a.x, dy = b.y - a.y;
        float len2 = dx * dx + dy * dy;
        float invLen2 = len2 > 0 ? 1.0f / len2 : 0;

        float reach = PassWidth(pass, fa > fb ? fa : fb) * 0.5f + 1.0f;
        int x0 = (int)floorf((ax < ax + dx ? ax : ax + dx) - reach);
        int x1 = (int)ceilf((ax > ax + dx ? ax : ax + dx) + reach);
        int y0 = (int)floorf((ay < ay + dy ? ay : ay + dy) - reach);
        int y1 = (int)ceilf((ay > ay + dy ? ay : ay + dy) + reach);
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > bw - 1) x1 = bw - 1;
        if (y1 > bh - 1) y1 = bh - 1;

        for (int y = y0; y <= y1; y++) {
            DWORD* row = &g_scratch[(size_t)y * bw];
            float ry = y - ay;
            for (int x = x0; x <= x1; x++) {
                float rx = x - ax;
                float u = (rx * dx + ry * dy) * invLen2;
                if (u < 0) u = 0;
                if (u > 1) u = 1;
                float fade = fa + (fb - fa) * u;
                if (fade <= 0.01f) continue;

                //? Distance to the segment gives round caps/joins and anti-aliasing in one step.
                float cx = rx - u * dx, cy = ry - u * dy;
                float d2 = cx * cx + cy * cy;
                float edge = PassWidth(pass, fade) * 0.5f + 0.5f;
                if (d2 >= edge * edge) continue;
                float coverage = edge - sqrtf(d2);
                if (coverage > 1) coverage = 1;

                DWORD alpha = AlphaByte(coverage * fade * pass.alphaScale);
                if (alpha <= (row[x] >> 24)) continue;
                row[x] = (alpha << 24) | (((cr * alpha + 127) / 255) << 16)
                       | (((cg * alpha + 127) / 255) << 8) | ((cb * alpha + 127) / 255);
            }
            if (x0 < g_spanMin[y]) g_spanMin[y] = x0;
            if (x1 > g_spanMax[y]) g_spanMax[y] = x1;
        }
    }

    //* Blend touched spans over the DIB and zero the scratch layer again.
    DWORD* dib = (DWORD*)g_dibBits;
    for (int y = 0; y < bh; y++) {
        DWORD* src = &g_scratch[(size_t)y * bw];
        DWORD* dst = dib + (size_t)(by0 + y) * g_vw + bx0;
        for (int x = g_spanMin[y]; x <= g_spanMax[y]; x++) {
            DWORD pixel = src[x];
            if (!pixel) continue;
            src[x] = 0;
            dst[x] = (pixel >> 24) == 255 ? pixel : BlendOver(pixel, dst[x]);
        }
    }
}

static void DrawLineStyle(double now, double lifetime) {
    static const TrailPass pass = { 1.0f, 1.0f, 0.0f, 1.0f, false };
    BuildSmoothTrail(g_smooth, kSmoothSpacing);
    DrawTrailPass(g_smooth, now, lifetime, pass);
}

static void DrawCometStyle(double now, double lifetime) {
    if (g_points.empty()) return;
    float w = (float)g_settings.thickness;

    static const TrailPass passes[] = {
        { 2.4f,  0.16f, 0.0f,  0.5f, true },
        { 1.3f,  0.40f, 0.15f, 0.5f, true },
        { 0.55f, 0.95f, 0.55f, 0.5f, true },
    };
    BuildSmoothTrail(g_smooth, kSmoothSpacing);
    for (const TrailPass& pass : passes) {
        DrawTrailPass(g_smooth, now, lifetime, pass);
    }

    // Glowing head at the cursor.
    const TrailPt& head = g_points.back();
    PointF c = Local(head.x, head.y);
    struct Halo { float radius, alpha, whiten; };
    const Halo halos[] = { {w * 2.0f, 0.20f, 0.0f}, {w * 1.1f, 0.5f, 0.2f}, {w * 0.55f, 0.95f, 0.7f} };
    for (const Halo& h : halos) {
        SolidBrush brush(TrailColor(h.alpha, h.whiten));
        g_graphics->FillEllipse(&brush, c.X - h.radius, c.Y - h.radius, h.radius * 2, h.radius * 2);
        //? +2 px covers GDI+ anti-aliasing outside the ellipse.
        MarkDrawn((int)floorf(c.X - h.radius) - 2, (int)floorf(c.Y - h.radius) - 2,
                  (int)ceilf(c.X + h.radius) + 2, (int)ceilf(c.Y + h.radius) + 2);
    }
}

static void RenderFrame() {
    if (!g_graphics) return;
    bool empty = g_points.empty();
    if (empty && g_lastFrameEmpty) return;
    g_lastFrameEmpty = empty;

    double now = Now();
    double lifetime = g_settings.durationMs / 1000.0;

    //? Only last frame's area can hold pixels, so clearing it replaces a full-surface Clear.
    DWORD* dib = (DWORD*)g_dibBits;
    for (LONG y = g_drawnRect.top; y < g_drawnRect.bottom; y++) {
        ZeroMemory(dib + (size_t)y * g_vw + g_drawnRect.left,
                   (size_t)(g_drawnRect.right - g_drawnRect.left) * 4);
    }

    SetRectEmpty(&g_frameRect);
    switch (g_settings.style) {
        case STYLE_LINE:  DrawLineStyle(now, lifetime); break;
        case STYLE_COMET: DrawCometStyle(now, lifetime); break;
    }
    g_graphics->Flush(FlushIntentionSync);

    //* Push only what changed: last frame's area (now cleared) plus this frame's.
    RECT dirty;
    UnionRect(&dirty, &g_drawnRect, &g_frameRect);
    if (g_fullUpdate) SetRect(&dirty, 0, 0, g_vw, g_vh);
    g_drawnRect = g_frameRect;
    g_fullUpdate = false;
    if (IsRectEmpty(&dirty)) return;

    //? Other topmost windows (taskbar, Task Manager) take the top spot whenever they're raised;
    //? claim it back each drawn frame so the trail stays above them.
    if (GetWindow(g_overlay, GW_HWNDPREV)) {
        SetWindowPos(g_overlay, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }

    POINT src = { 0, 0 };
    POINT dst = { g_vx, g_vy };
    SIZE  size = { g_vw, g_vh };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UPDATELAYEREDWINDOWINFO info = {};
    info.cbSize = sizeof(info);
    info.pptDst = &dst;
    info.psize = &size;
    info.hdcSrc = g_memDC;
    info.pptSrc = &src;
    info.pblend = &blend;
    info.dwFlags = ULW_ALPHA;
    info.prcDirty = &dirty;
    UpdateLayeredWindowIndirect(g_overlay, &info);
}

// --------------------------------------------------------------- tray menu

#define WM_TRAYICON   (WM_APP + 1)
#define IDM_QUIT      101
#define IDM_STARTUP   102
#define IDM_STYLE0    110   // +style
#define IDM_COLOR     150
#define IDM_THICK0    160   // +index
#define IDM_LENGTH0   170   // +index

static const int kThickness[]  = { 3, 6, 10, 16 };
static const wchar_t* kThicknessNames[] = { L"Thin", L"Medium", L"Thick", L"Extra Thick" };
static const int kDurations[]  = { 300, 600, 1200, 2500 };
static const wchar_t* kDurationNames[] = { L"Short", L"Medium", L"Long", L"Extra Long" };

static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

static bool StartupEnabled() {
    HKEY key;
    bool found = false;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) == ERROR_SUCCESS) {
        found = RegQueryValueExW(key, L"MouseTrail", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
        RegCloseKey(key);
    }
    return found;
}

static void ToggleStartup() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_WRITE, &key) != ERROR_SUCCESS) return;
    if (StartupEnabled()) {
        RegDeleteValueW(key, L"MouseTrail");
    } else {
        wchar_t path[MAX_PATH + 2];
        wchar_t quoted[MAX_PATH + 4];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        wsprintfW(quoted, L"\"%s\"", path);
        RegSetValueExW(key, L"MouseTrail", 0, REG_SZ, (const BYTE*)quoted,
                       (DWORD)((lstrlenW(quoted) + 1) * sizeof(wchar_t)));
    }
    RegCloseKey(key);
}

static void ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();

    HMENU styleMenu = CreatePopupMenu();
    const wchar_t* styleNames[] = { L"Line", L"Comet Glow" };
    for (int i = 0; i < STYLE_COUNT; i++) {
        AppendMenuW(styleMenu, MF_STRING | (g_settings.style == i ? MF_CHECKED : 0),
                    IDM_STYLE0 + i, styleNames[i]);
    }
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)styleMenu, L"Style");
    AppendMenuW(menu, MF_STRING, IDM_COLOR, L"Color…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    HMENU thickMenu = CreatePopupMenu();
    for (int i = 0; i < 4; i++) {
        AppendMenuW(thickMenu, MF_STRING | (g_settings.thickness == kThickness[i] ? MF_CHECKED : 0),
                    IDM_THICK0 + i, kThicknessNames[i]);
    }
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)thickMenu, L"Thickness");

    HMENU lengthMenu = CreatePopupMenu();
    for (int i = 0; i < 4; i++) {
        AppendMenuW(lengthMenu, MF_STRING | (g_settings.durationMs == kDurations[i] ? MF_CHECKED : 0),
                    IDM_LENGTH0 + i, kDurationNames[i]);
    }
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)lengthMenu, L"Trail Length");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu, MF_STRING | (StartupEnabled() ? MF_CHECKED : 0),
                IDM_STARTUP, L"Start with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_QUIT, L"Quit Mouse Trail");

    POINT cursor;
    GetCursorPos(&cursor);
    SetForegroundWindow(g_trayWnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, g_trayWnd, nullptr);
    PostMessageW(g_trayWnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

static void ChooseCustomColor() {
    static COLORREF customColors[16] = {};
    CHOOSECOLORW cc = {};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = g_trayWnd;
    cc.rgbResult = g_settings.color;
    cc.lpCustColors = customColors;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (ChooseColorW(&cc)) {
        g_settings.color = cc.rgbResult;
        SaveSettings();
    }
}

static NOTIFYICONDATAW g_nid = {};

static void AddTrayIcon(HINSTANCE hInst) {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_trayWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpyW(g_nid.szTip, L"Mouse Trail");
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    if (!g_settings.welcomed) {
        g_settings.welcomed = true;
        SaveSettings();
        NOTIFYICONDATAW info = g_nid;
        info.uFlags = NIF_INFO;
        info.dwInfoFlags = NIIF_INFO;
        lstrcpyW(info.szInfoTitle, L"Welcome to Mouse Trail");
        lstrcpyW(info.szInfo,
                 L"Move your mouse to see the trail! Right-click this tray icon "
                 L"to change the style, color, and more.");
        Shell_NotifyIconW(NIM_MODIFY, &info);
    }
}

// ----------------------------------------------------------- window procs

static LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static UINT g_taskbarCreatedMsg = 0;

static LRESULT CALLBACK TrayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreatedMsg) {          // Explorer restarted
        Shell_NotifyIconW(NIM_ADD, &g_nid);
        return 0;
    }
    switch (msg) {
    case WM_TIMER:
        TickModel();
        RenderFrame();
        return 0;
    case WM_TRAYICON:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP
            || LOWORD(lp) == WM_CONTEXTMENU) {
            ShowTrayMenu();
        }
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDM_QUIT) {
            DestroyWindow(hwnd);
        } else if (id == IDM_STARTUP) {
            ToggleStartup();
        } else if (id >= IDM_STYLE0 && id < IDM_STYLE0 + STYLE_COUNT) {
            g_settings.style = id - IDM_STYLE0;
        } else if (id == IDM_COLOR) {
            ChooseCustomColor();
        } else if (id >= IDM_THICK0 && id < IDM_THICK0 + 4) {
            g_settings.thickness = kThickness[id - IDM_THICK0];
        } else if (id >= IDM_LENGTH0 && id < IDM_LENGTH0 + 4) {
            g_settings.durationMs = kDurations[id - IDM_LENGTH0];
        }
        SaveSettings();
        return 0;
    }
    case WM_DISPLAYCHANGE:
        CreateSurface();
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ------------------------------------------------------------------- main

static void EnableDpiAwareness() {
    // Per-monitor v2 so cursor coordinates match physical pixels on scaled
    // displays; fall back gracefully on older Windows 10 builds.
    typedef BOOL(WINAPI * SetCtxFn)(HANDLE);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    SetCtxFn setCtx = user32
        ? (SetCtxFn)(void*)GetProcAddress(user32, "SetProcessDpiAwarenessContext")
        : nullptr;
    if (!setCtx || !setCtx((HANDLE)-4 /* PER_MONITOR_AWARE_V2 */)) {
        SetProcessDPIAware();
    }
}

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    // Single instance.
    CreateMutexW(nullptr, TRUE, L"MouseTrailSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    EnableDpiAwareness();
    LoadSettings();

    GdiplusStartupInput gdiplusInput;
    ULONG_PTR gdiplusToken;
    if (GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr) != Ok) return 1;

    WNDCLASSW overlayClass = {};
    overlayClass.lpfnWndProc = OverlayProc;
    overlayClass.hInstance = hInst;
    overlayClass.lpszClassName = L"MouseTrailOverlay";
    RegisterClassW(&overlayClass);

    WNDCLASSW trayClass = {};
    trayClass.lpfnWndProc = TrayProc;
    trayClass.hInstance = hInst;
    trayClass.lpszClassName = L"MouseTrailTray";
    RegisterClassW(&trayClass);

    g_overlay = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        L"MouseTrailOverlay", L"Mouse Trail", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, hInst, nullptr);

    g_trayWnd = CreateWindowExW(0, L"MouseTrailTray", L"Mouse Trail Tray",
                                WS_OVERLAPPED, 0, 0, 0, 0,
                                nullptr, nullptr, hInst, nullptr);

    if (!g_overlay || !g_trayWnd || !CreateSurface()) return 1;
    ShowWindow(g_overlay, SW_SHOWNOACTIVATE);

    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    AddTrayIcon(hInst);
    SetTimer(g_trayWnd, 1, 10, nullptr);   // ~64 fps (system timer resolution)

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DestroySurface();
    GdiplusShutdown(gdiplusToken);
    return 0;
}
