# Artwork

Only current sources and consumed outputs belong here:

| File | Consumer |
| --- | --- |
| `logo-transparent.svg` | Shared editable logo; both renderers use it. |
| `logo-transparent.png` | Installer artwork and bilingual README. |
| `logo.ico` | App, title bar/taskbar, About, Setup, and uninstaller. |
| `banner.svg` | Shared editable banner; references the current logo, never copied letter paths. |
| `banner.en.png`, `banner.vi.png` | English/Vietnamese README headers. |
| `app-banner.en.bmp`, `app-banner.vi.bmp` | Localized 24-bit header resources embedded in the EXE. |

Run `.dev/tools/render-icon.ps1` and `.dev/tools/render-banner.ps1` after any logo change. Normal builds use checked-in outputs; users need no renderer or image library.

The middle and top Z rotate 10° and 19° around fixed overlap anchors. Keep the bubble body near the canvas center, slightly left horizontally, rather than centering all visible pixels or the rising-Z bounds. Icon frames render directly from SVG at 16, 20, 24, 32, 40, 48, 64, 96, 128, and 256 pixels. Native tests check coverage and bubble alignment.

The banner retains the original Comic Sans MS wordmark and shadow, helper label, slogan, window/tray frame, blue texture, dots, stars, and curves. Vietnamese supporting text uses Segoe UI because Comic Sans lacks some Vietnamese glyphs. The renderer localizes text and repositions groups for the short 1600×180 app header. Full README banners remain 1600×560. Lettering is rasterized during generation; the app has no logo-font dependency. Small decorative text is intentionally small in the short header; real onboarding remains outside the image.

Keep only the listed sources and consumed outputs. Do not add local previews, recovery archives, or real chat screenshots to documentation.
