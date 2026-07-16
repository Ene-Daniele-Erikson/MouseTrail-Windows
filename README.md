<p align="center">
  <img src="icon/icon_256.png" width="160" alt="Mouse Trail icon">
</p>

<h1 align="center">Mouse Trail for Windows</h1>

<p align="center">
  A tiny Windows tray app that draws a colorful trail behind your mouse cursor.<br>
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

- **Three styles** — clean **Line**, glowing **Comet**, or twinkling **Sparkles**
- **Any color** — rainbow mode, nine presets, or a custom color picker
- Adjustable **thickness** and **trail length**
- Works across **multiple displays**, never blocks a click
- Lives in the **system tray**, remembers your settings
- Optional **start with Windows**
- A single small `.exe` — no installer, no runtime, no network access

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
