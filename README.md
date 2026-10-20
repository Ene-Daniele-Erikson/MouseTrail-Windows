<p align="center">
  <img src="icon/icon_256.png" width="160" alt="Mouse Trail icon">
</p>

<h1 align="center">Mouse Trail for Windows</h1>

<p align="center">
  A tiny Windows tray app that draws a smooth trail behind your mouse cursor.<br>
  Free, open source, no ads, no data collection.<br>
  <em>On a Mac? Get the <a href="https://github.com/changymon/MouseTrail">macOS version</a>.</em>
</p>

---

## Why I made this

This app exists because of a World of Warcraft addon. I used a cursor
trail addon from CurseForge for years, and eventually realized the thing
I missed most outside the game wasn't anything in the game — it was
being able to *find my mouse*.

Like a lot of neurodivergent people, I constantly lose track of my
cursor. A trail fixes the problem at the source — your cursor becomes
something your eyes track effortlessly, because it moves with color and
motion instead of being a small static arrow. And an accessibility aid
shouldn't cost money, so it's free.

If it helps you the way it helps me, that's the whole point. 🌈

## Features

- **Two styles** — clean **Line** or glowing **Comet**
- **Smooth, continuous trail** — follows every mouse movement, even at high polling rates
- **Any color** — pick it with the color picker (red by default)
- Adjustable **thickness** and **trail length**
- Works across **multiple displays**, never blocks a click
- Lives in the **system tray**, remembers your settings
- Optional **start with Windows**
- A single small `.exe` — no installer, no runtime, no network access

## Changes in this fork

This fork of [changymon/MouseTrail-Windows](https://github.com/changymon/MouseTrail-Windows)
focuses on a smoother trail and a smaller set of options.

### Changed

- **Trail is drawn as one continuous stroke.** The original drew each
  segment as its own semi-transparent line with round caps, so the
  overlapping caps showed up as darker dots at every joint. The trail
  is now rasterized directly into the overlay: each pixel takes the
  strongest coverage of any segment touching it instead of stacking
  alphas, so there are no overlap artifacts and the fade and taper stay
  smooth.
- **Fast movement no longer draws polygons.** The original read the
  cursor once per timer tick (~64 Hz), so fast curves became straight
  lines between samples. The app now reads every mouse position Windows
  recorded between ticks with `GetMouseMovePointsEx`, falling back to
  `GetCursorPos` when the history isn't available.
- **Stays above the taskbar.** The overlay re-claims the top spot
  whenever another always-on-top window (the taskbar, Task Manager)
  is raised above it. The Start menu, Search, notifications and
  Alt-Tab live in system layers above every app window, so they still
  cover the trail.
- **Redraws only what changed.** The original cleared and uploaded the
  whole virtual screen every frame. Each frame now clears and pushes
  only the area the trail touched, which cuts CPU and memory bandwidth
  sharply on large or multi-monitor setups.
- **Default color is red** (`#FF0000`). It's saved under a new `Color`
  registry value, so colors saved by the original app are ignored.

### Added

- **Spline smoothing.** Points are fitted with a centripetal
  Catmull-Rom spline and resampled every 3 px, so curves stay round
  even when the mouse moves quickly.
- **Bump removal at high polling rates.** Integer 1–2 px mouse steps
  used to show up as a wavy, staircase-like trail. New points are only
  fixed once they're at least 4 px from the previous one (the head
  still follows the cursor exactly), and two light averaging passes
  round off what's left.

### Removed

- **Sparkles** style (Line and Comet remain).
- **Rainbow mode and the nine color presets.** Color is now a single
  **Color…** item that opens the color picker.
- **Pause / Resume Trail** menu item.

## Install

1. Download **MouseTrail-Windows.zip** from the
   [latest release](../../releases/latest) and unzip it.
2. Run **MouseTrail.exe** (put it wherever you like, e.g. a `Tools`
   folder — "Start with Windows" remembers its location).
3. Windows SmartScreen will say *"Windows protected your PC"* — that's
   because the app isn't code-signed (certificates cost money; this is a
   free hobby project). Click **More info → Run anyway**. You only need
   to do this once.
4. Move your mouse! Right-click the trail icon in the system tray to
   change the style, color, thickness, and trail length.

If you'd rather not trust a downloaded binary: the release `.exe` is
compiled from this public source code by GitHub Actions (see
[the workflow](.github/workflows/release.yml) and the release's build
logs under the Actions tab), and it's one C++ file you can audit and
build yourself.

## Build from source

With [Visual Studio Build Tools](https://visualstudio.microsoft.com/downloads/)
installed, from a "Developer Command Prompt":

```bat
rc /nologo app.rc
cl /O2 /EHsc /DUNICODE /D_UNICODE main.cpp app.res /link /SUBSYSTEM:WINDOWS gdiplus.lib user32.lib gdi32.lib shell32.lib advapi32.lib comdlg32.lib ole32.lib /OUT:MouseTrail.exe
```

Or with MinGW-w64 (works cross-platform, even from a Mac):

```sh
x86_64-w64-mingw32-windres app.rc -O coff -o app.res.o
x86_64-w64-mingw32-g++ -O2 -std=c++17 -municode -mwindows main.cpp app.res.o -o MouseTrail.exe -lgdiplus -lgdi32 -luser32 -lshell32 -ladvapi32 -lcomdlg32 -lole32 -static
```

## Privacy

Mouse Trail reads the cursor position only to draw the trail, has no
network access, and collects nothing. Settings are stored in
`HKEY_CURRENT_USER\Software\MouseTrail`.

## License

[MIT](LICENSE) — do whatever you like with it.
