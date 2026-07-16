// Mouse Trail for Windows — colorful trails behind the mouse cursor.
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
#include <cstdlib>

using namespace Gdiplus;

// ---------------------------------------------------------------- settings

enum TrailStyle { STYLE_LINE = 0, STYLE_COMET = 1, STYLE_SPARKLES = 2 };
enum ColorMode  { COLOR_RAINBOW = 0, COLOR_SOLID = 1 };

struct Preset { const wchar_t* name; COLORREF color; };
static const Preset kPresets[] = {
    { L"White",  RGB(255, 255, 255) },
    { L"Black",  RGB(0, 0, 0) },
    { L"Red",    RGB(255, 69, 58) },
    { L"Orange", RGB(255, 149, 0) },
    { L"Yellow", RGB(255, 214, 10) },
    { L"Green",  RGB(51, 214, 74) },
    { L"Blue",   RGB(10, 133, 255) },
    { L"Purple", RGB(191, 89, 242) },
    { L"Pink",   RGB(255, 94, 166) },
};
static const int kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);

struct Settings {
    int      style      = STYLE_LINE;
    int      colorMode  = COLOR_RAINBOW;
    COLORREF solidColor = RGB(255, 255, 255);
    int      thickness  = 6;      // px
    int      durationMs = 600;    // trail fade time
    bool     enabled    = true;
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
    g_settings.colorMode  = (int)RegGetDW(L"ColorMode", COLOR_RAINBOW);
    g_settings.solidColor = (COLORREF)RegGetDW(L"SolidColor", RGB(255, 255, 255));
    g_settings.thickness  = (int)RegGetDW(L"Thickness", 6);
    g_settings.durationMs = (int)RegGetDW(L"DurationMs", 600);
    g_settings.enabled    = RegGetDW(L"Enabled", 1) != 0;
    g_settings.welcomed   = RegGetDW(L"Welcomed", 0) != 0;
}

static void SaveSettings() {
    RegSetDW(L"Style", g_settings.style);
    RegSetDW(L"ColorMode", g_settings.colorMode);
    RegSetDW(L"SolidColor", g_settings.solidColor);
    RegSetDW(L"Thickness", g_settings.thickness);
    RegSetDW(L"DurationMs", g_settings.durationMs);
    RegSetDW(L"Enabled", g_settings.enabled ? 1 : 0);
    RegSetDW(L"Welcomed", g_settings.welcomed ? 1 : 0);
}

// ------------------------------------------------------------- trail model

struct TrailPt { float x, y; double t; };
struct Sparkle {
    float x, y, vx, vy;
    double birth, lifetime;
    float size, hueJitter, twinklePhase;
};

static std::vector<TrailPt> g_points;
static std::vector<Sparkle> g_sparkles;
static POINT  g_lastCursor  = { LONG_MIN, LONG_MIN };
static double g_lastTick    = 0;

static double Now() {
    static LARGE_INTEGER freq = {};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)freq.QuadPart;
}

static float FRand(float lo, float hi) {
    return lo + (hi - lo) * ((float)rand() / (float)RAND_MAX);
}

static void SpawnSparkles(POINT from, POINT to, float distance, double now) {
    int count = (int)(distance / 5);
    if (count < 1) count = 1;
    if (count > 8) count = 8;
    double duration = g_settings.durationMs / 1000.0;
    for (int i = 0; i < count; i++) {
        float t = FRand(0, 1);
        Sparkle s;
        s.x = from.x + (to.x - from.x) * t + FRand(-4, 4);
        s.y = from.y + (to.y - from.y) * t + FRand(-4, 4);
        s.vx = FRand(-35, 35);
        s.vy = FRand(-45, 15);            // y grows downward; bias upward
        s.birth = now;
        s.lifetime = duration * FRand(0.9f, 1.8f);
        s.size = g_settings.thickness * FRand(0.35f, 0.8f);
        s.hueJitter = FRand(-0.08f, 0.08f);
        s.twinklePhase = FRand(0, 6.2831853f);
        g_sparkles.push_back(s);
    }
}

static void TickModel() {
    double now = Now();
    double dt = now - g_lastTick;
    if (dt > 0.05) dt = 0.05;
    g_lastTick = now;

    POINT cursor;
    GetCursorPos(&cursor);

    bool first = (g_lastCursor.x == LONG_MIN);
    float dx = (float)(cursor.x - g_lastCursor.x);
    float dy = (float)(cursor.y - g_lastCursor.y);
    float moved = first ? 1.0f : sqrtf(dx * dx + dy * dy);
    if (first || moved > 0.5f) {
        if (!first && g_settings.style == STYLE_SPARKLES) {
            SpawnSparkles(g_lastCursor, cursor, moved, now);
        }
        TrailPt p = { (float)cursor.x, (float)cursor.y, now };
        g_points.push_back(p);
        g_lastCursor = cursor;
    }

    double lifetime = g_settings.durationMs / 1000.0;
    size_t keep = 0;
    for (size_t i = 0; i < g_points.size(); i++) {
        if (now - g_points[i].t <= lifetime) g_points[keep++] = g_points[i];
    }
    g_points.resize(keep);

    keep = 0;
    for (size_t i = 0; i < g_sparkles.size(); i++) {
        Sparkle& s = g_sparkles[i];
        s.x += s.vx * (float)dt;
        s.y += s.vy * (float)dt;
        s.vy += 55.0f * (float)dt;        // gravity (downward)
        s.vx *= (1.0f - 1.2f * (float)dt);
        s.vy *= (1.0f - 0.4f * (float)dt);
        if (now - s.birth <= s.lifetime) g_sparkles[keep++] = s;
    }
    g_sparkles.resize(keep);
}

static void ClearModel() {
    g_points.clear();
    g_sparkles.clear();
    g_lastCursor.x = LONG_MIN;
    g_lastCursor.y = LONG_MIN;
}

// ------------------------------------------------------------------ colors

static void HsvToRgb(float h, float s, float v, BYTE& r, BYTE& g, BYTE& b) {
    h = h - floorf(h);
    float i = floorf(h * 6.0f);
    float f = h * 6.0f - i;
    float p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
    float rf, gf, bf;
    switch ((int)i % 6) {
        case 0: rf = v; gf = t; bf = p; break;
        case 1: rf = q; gf = v; bf = p; break;
        case 2: rf = p; gf = v; bf = t; break;
        case 3: rf = p; gf = q; bf = v; break;
        case 4: rf = t; gf = p; bf = v; break;
        default: rf = v; gf = p; bf = q; break;
    }
    r = (BYTE)(rf * 255 + 0.5f);
    g = (BYTE)(gf * 255 + 0.5f);
    b = (BYTE)(bf * 255 + 0.5f);
}

static BYTE AlphaByte(double a) {
    if (a < 0) a = 0;
    if (a > 1) a = 1;
    return (BYTE)(a * 255 + 0.5);
}

// Trail color for something born at `time`, before fade/whitening.
static Color TrailColor(double time, float hueJitter, double alpha, float whiten) {
    BYTE r, g, b;
    if (g_settings.colorMode == COLOR_RAINBOW) {
        float hue = (float)fmod(time * 0.4, 1.0) + hueJitter;
        HsvToRgb(hue, 0.9f, 1.0f, r, g, b);
    } else {
        r = GetRValue(g_settings.solidColor);
        g = GetGValue(g_settings.solidColor);
        b = GetBValue(g_settings.solidColor);
    }
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

static void DrawLineStyle(double now, double lifetime) {
    for (size_t i = 1; i < g_points.size(); i++) {
        const TrailPt& p0 = g_points[i - 1];
        const TrailPt& p1 = g_points[i];
        double t = 1.0 - (now - p1.t) / lifetime;
        if (t <= 0.01) continue;
        Pen pen(TrailColor(p1.t, 0, t, 0), (REAL)(g_settings.thickness * t < 1 ? 1 : g_settings.thickness * t));
        pen.SetStartCap(LineCapRound);
        pen.SetEndCap(LineCapRound);
        pen.SetLineJoin(LineJoinRound);
        g_graphics->DrawLine(&pen, Local(p0.x, p0.y), Local(p1.x, p1.y));
    }
}

static void DrawCometStyle(double now, double lifetime) {
    if (g_points.empty()) return;
    float w = (float)g_settings.thickness;

    struct Pass { float widthScale, alphaScale, whiten; };
    static const Pass passes[] = { {2.4f, 0.16f, 0.0f}, {1.3f, 0.40f, 0.15f}, {0.55f, 0.95f, 0.55f} };

    for (const Pass& pass : passes) {
        for (size_t i = 1; i < g_points.size(); i++) {
            const TrailPt& p0 = g_points[i - 1];
            const TrailPt& p1 = g_points[i];
            double t = 1.0 - (now - p1.t) / lifetime;
            if (t <= 0.01) continue;
            float width = (float)(w * pass.widthScale * t * t);
            if (width < 0.5f) width = 0.5f;
            Pen pen(TrailColor(p1.t, 0, t * pass.alphaScale, pass.whiten), width);
            pen.SetStartCap(LineCapRound);
            pen.SetEndCap(LineCapRound);
            g_graphics->DrawLine(&pen, Local(p0.x, p0.y), Local(p1.x, p1.y));
        }
    }

    // Glowing head at the cursor.
    const TrailPt& head = g_points.back();
    PointF c = Local(head.x, head.y);
    struct Halo { float radius, alpha, whiten; };
    const Halo halos[] = { {w * 2.0f, 0.20f, 0.0f}, {w * 1.1f, 0.5f, 0.2f}, {w * 0.55f, 0.95f, 0.7f} };
    for (const Halo& h : halos) {
        SolidBrush brush(TrailColor(head.t, 0, h.alpha, h.whiten));
        g_graphics->FillEllipse(&brush, c.X - h.radius, c.Y - h.radius, h.radius * 2, h.radius * 2);
    }
}

static void DrawSparklesStyle(double now) {
    for (const Sparkle& s : g_sparkles) {
        double fade = 1.0 - (now - s.birth) / s.lifetime;
        if (fade <= 0.01) continue;
        double twinkle = 0.65 + 0.35 * sin(now * 9 + s.twinklePhase);
        double alpha = fade * twinkle;
        float r = s.size * (0.6f + 0.4f * (float)fade);
        PointF c = Local(s.x, s.y);

        // Four-pointed star.
        PointF star[8];
        for (int i = 0; i < 8; i++) {
            float angle = i * 3.14159265f / 4.0f;
            float radius = (i % 2 == 0) ? r : r * 0.38f;
            star[i] = PointF(c.X + radius * cosf(angle), c.Y + radius * sinf(angle));
        }
        SolidBrush brush(TrailColor(s.birth, s.hueJitter, alpha, 0));
        g_graphics->FillPolygon(&brush, star, 8);

        // Bright core so sparkles pop on any background.
        SolidBrush core(TrailColor(s.birth, s.hueJitter, alpha, 0.7f));
        g_graphics->FillEllipse(&core, c.X - r * 0.25f, c.Y - r * 0.25f, r * 0.5f, r * 0.5f);
    }
}

static void RenderFrame() {
    if (!g_graphics) return;
    bool empty = g_points.empty() && g_sparkles.empty();
    if (empty && g_lastFrameEmpty) return;
    g_lastFrameEmpty = empty;

    double now = Now();
    double lifetime = g_settings.durationMs / 1000.0;

    g_graphics->Clear(Color(0, 0, 0, 0));
    switch (g_settings.style) {
        case STYLE_LINE:     DrawLineStyle(now, lifetime); break;
        case STYLE_COMET:    DrawCometStyle(now, lifetime); break;
        case STYLE_SPARKLES: DrawSparklesStyle(now); break;
    }
    g_graphics->Flush(FlushIntentionSync);

    POINT src = { 0, 0 };
    POINT dst = { g_vx, g_vy };
    SIZE  size = { g_vw, g_vh };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    HDC screen = GetDC(nullptr);
    UpdateLayeredWindow(g_overlay, screen, &dst, &size, g_memDC, &src, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

// --------------------------------------------------------------- tray menu

#define WM_TRAYICON   (WM_APP + 1)
#define IDM_TOGGLE    100
#define IDM_QUIT      101
#define IDM_STARTUP   102
#define IDM_STYLE0    110   // +style
#define IDM_RAINBOW   120
#define IDM_PRESET0   130   // +preset index
#define IDM_CUSTOM    150
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
    AppendMenuW(menu, MF_STRING, IDM_TOGGLE,
                g_settings.enabled ? L"Pause Trail" : L"Resume Trail");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    HMENU styleMenu = CreatePopupMenu();
    const wchar_t* styleNames[] = { L"Line", L"Comet Glow", L"Sparkles" };
    for (int i = 0; i < 3; i++) {
        AppendMenuW(styleMenu, MF_STRING | (g_settings.style == i ? MF_CHECKED : 0),
                    IDM_STYLE0 + i, styleNames[i]);
    }
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)styleMenu, L"Style");

    HMENU colorMenu = CreatePopupMenu();
    AppendMenuW(colorMenu, MF_STRING | (g_settings.colorMode == COLOR_RAINBOW ? MF_CHECKED : 0),
                IDM_RAINBOW, L"Rainbow");
    AppendMenuW(colorMenu, MF_SEPARATOR, 0, nullptr);
    for (int i = 0; i < kPresetCount; i++) {
        bool checked = g_settings.colorMode == COLOR_SOLID
                       && g_settings.solidColor == kPresets[i].color;
        AppendMenuW(colorMenu, MF_STRING | (checked ? MF_CHECKED : 0),
                    IDM_PRESET0 + i, kPresets[i].name);
    }
    AppendMenuW(colorMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(colorMenu, MF_STRING, IDM_CUSTOM, L"Custom…");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)colorMenu, L"Color");
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
    cc.rgbResult = g_settings.solidColor;
    cc.lpCustColors = customColors;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (ChooseColorW(&cc)) {
        g_settings.colorMode = COLOR_SOLID;
        g_settings.solidColor = cc.rgbResult;
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
        if (g_settings.enabled) {
            TickModel();
            RenderFrame();
        }
        return 0;
    case WM_TRAYICON:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP
            || LOWORD(lp) == WM_CONTEXTMENU) {
            ShowTrayMenu();
        }
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDM_TOGGLE) {
            g_settings.enabled = !g_settings.enabled;
            if (!g_settings.enabled) {
                ClearModel();
                g_lastFrameEmpty = false;
                RenderFrame();               // one clear pass
            }
        } else if (id == IDM_QUIT) {
            DestroyWindow(hwnd);
        } else if (id == IDM_STARTUP) {
            ToggleStartup();
        } else if (id >= IDM_STYLE0 && id < IDM_STYLE0 + 3) {
            g_settings.style = id - IDM_STYLE0;
        } else if (id == IDM_RAINBOW) {
            g_settings.colorMode = COLOR_RAINBOW;
        } else if (id >= IDM_PRESET0 && id < IDM_PRESET0 + kPresetCount) {
            g_settings.colorMode = COLOR_SOLID;
            g_settings.solidColor = kPresets[id - IDM_PRESET0].color;
        } else if (id == IDM_CUSTOM) {
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
    g_lastTick = Now();
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
