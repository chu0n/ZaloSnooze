# Build and package ZaloSnooze

[English README](../README.en.md) · [Tiếng Việt](../README.md) · [Architecture](ARCHITECTURE.md)

## Install the tools

Use Windows with an interactive desktop for the UI tests. Install [MSYS2](https://www.msys2.org/) and follow its update instructions. In the MSYS2 terminal:

```bash
pacman -S --needed mingw-w64-i686-gcc mingw-w64-i686-cmake mingw-w64-i686-ninja
```

These are the **MINGW32** packages. UCRT64 builds for x64 and cannot produce this project’s x86 executable. MSYS2 has deprecated MINGW32; any replacement must preserve x86 output and pass the same runtime dependency checks.

Install [Inno Setup 6](https://jrsoftware.org/isinfo.php) for packaging and installer tests. These build tools don’t ship with the app.

## Build and test

Open PowerShell at the repository root. Adjust the MSYS2 path if needed:

```powershell
.\.dev\build.ps1 -Msys2Root 'C:\msys64'
.\.dev\test.ps1 -Msys2Root 'C:\msys64'
```

Build scripts, tests, artwork tools, and the installer source live under `.dev/`. The app and test fixtures go into `.dev/build/`. Tests open isolated windows and do not close your running Zalo.

## Create the distribution files

```powershell
.\.dev\package.ps1 -Msys2Root 'C:\msys64'
.\.dev\tests\installer-smoke.ps1
.\.dev\tests\release-smoke.ps1
```

Pass `-InnoCompiler` to the packaging and installer test scripts if they cannot find `ISCC.exe`.

| Filename format | Contents |
| --- | --- |
| `ZaloSnooze-<version>-Setup.exe` | Installer with the x86 app and its MIT license |
| `ZaloSnooze-<version>-Portable-x86.zip` | x86 app, default INI, portable guide, and MIT license |

Both files go into `.dev/release/`. Git ignores `.dev/build/` and `.dev/release/`, not the scripts and sources under `.dev/`. Do not commit user settings, logs, or test captures. The portable package must not contain `installed.ini`.

## GitHub publishing

The public repository is [chu0n/ZaloSnooze](https://github.com/chu0n/ZaloSnooze). Pushes to `main` and pull requests only compile the app. Run the tests above locally before tagging. When publishing a binary release, use a tag that matches `CMakeLists.txt`. The tag workflow builds both packages and stages them with their checksums before publishing. It does not replace an existing release.

Local builds link to the official project by default. GitHub Actions uses the repository running the workflow. To set the link explicitly:

```powershell
.\.dev\build.ps1 -Msys2Root 'C:\msys64' -ProjectUrl 'https://github.com/chu0n/ZaloSnooze'
```

Keep the version unchanged until the initial source upload is confirmed. See [repository rules](../AGENTS.md).

## Maintenance

- [Reliability and test coverage](RELIABILITY.md) lists the checks and known limits.
- [Troubleshooting](DEBUGGING.md) explains logging and startup recovery.
- [Artwork](assets/README.md) lists the checked-in assets and how to regenerate them.
- Operational commands: `--settings` opens preferences; `--run` applies the watcher once; `--startup` is for registered sign-in startup. Setup and uninstall use `--enable-startup` and `--uninstall-user`.
