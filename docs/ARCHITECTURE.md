# Architecture and invariants

## Ownership

| File | Owns |
| --- | --- |
| `src/core.h`, `src/core.cpp` | Unicode/file handling, config parsing and preservation, storage mode, process paths, Zalo discovery, window state transitions. |
| `src/watcher.cpp` | One-shot process/window observation, asynchronous Hide/Close, bounded retries, optional opacity restoration. |
| `src/startup.h`, `src/startup.cpp` | Current-user startup registration, verified Zalo entry discovery, durable backups, ownership and conflict-safe restore. |
| `src/gui.cpp` | Native dialogs, localization, menus, explicit preference changes, consent prompts, run status. |
| `src/main.cpp` | Entry routing, COM/common-controls initialization, error boundary. |
| `resources/` | Dialog resources, DPI/common-controls manifest, icon and version resources. |
| `.dev/installer/ZaloSnooze.iss` | Installation scope, files, shortcuts, uninstall registration, uninstaller, original-user startup handoff. |
| `.dev/config/` | Packaging templates: portable defaults and the installed-mode marker. |
| `.dev/tests/` | Native state tests, fixtures, GUI/installer/package checks. |

There is no plugin layer, service, web UI, dependency injection framework, network updater, or permanent tray process.

A global, per-configuration settings mutex prevents editors and command-line startup setup from overwriting each other's changes across sessions. The editor holds it for its lifetime; setup holds it across reload, consent, and commit. A separate session-local mutex permits only one active watcher.

The main window prioritizes one labelled enable/disable startup button. It lists resolved Zalo paths and uses native Windows gear/folder icons with accessible names and tooltips. Folder opening uses only the selected path's parent directory, not its executable. The locations dialog owns automatic/custom installation selection. Experimental and Restore live in Tools; Experimental owns its independent Save/Cancel boundary. There is no immediate-run button or GUI-owned watcher; operational CLI/background modes remain unchanged.

Layout uses dialog units and measured text, with DPI-aware non-client/menu sizing. Separate icon sizes serve the title bar and taskbar. About uses the same app icon, not a generic information symbol. The short localized header uses embedded 1600×180 24-bit BMP resources and native GDI scaling, not loose images or an image library. Onboarding remains real accessible text below the artwork. Version information lives in the title bar; installation type and actual startup state share one metadata line.

Configuration or recovery-read errors appear inline without abandoning menu/icon initialization. Experimental remains available to repair an invalid log path.

Artwork has two sources: the approved corner-anchored logo SVG and one banner SVG referencing it. The banner renderer produces full documentation PNGs and short embedded BMPs from that source, preserving the original lettering, helper label, slogan, window/tray illustration, and background details. Unused historical artwork and app-only preview PNGs are not kept in the workspace.

## Storage identity

Only the adjacent marker `installed.ini` containing `ZaloSnooze.InstallMode=Installed` selects installed mode. Installed mode uses the current user's known Local AppData folder. Otherwise settings live beside the running executable. An installation elsewhere never changes the identity of a portable copy.

Configuration writes use a temporary file and atomic replacement. Existing files are replaced with `ReplaceFileW`, preserving their ACL and attributes; a new file uses `MoveFileExW`. File-access errors fail closed. Unknown keys and comments survive saves; malformed individual values retain defaults. Existing UTF-8, UTF-16 LE, and legacy ANSI text can be read; saves use UTF-8 with BOM.

Relative Zalo paths resolve against the EXE folder. Relative logs resolve against the active config folder. The app never silently falls back to AppData when a portable folder is read-only.

Resolved log paths must not collide with preferences, helper or configured Zalo executables, the installed-mode marker, the recovery folder, or the legacy startup-shortcut backup. Canonical paths and existing hard-link aliases are checked before a watcher starts or Experimental settings save.

## Window lifecycle

One watcher owns a session-local mutex. It discovers only matching processes in its own Windows session. Process paths use `QueryFullProcessImageNameW`; no cross-bitness module inspection or injected hooks are needed.

Window recognition requires the expected executable, an unowned eligible initial title, class, and size. Known main windows may change title. WinEvent callbacks supplement periodic scans. Close requests are asynchronous and confirmed through subsequent state observations.

The taskbar option uses `ShowWindowAsync(SW_SHOWMINNOACTIVE)` instead of hiding the window and sending `WM_CLOSE`. Success then requires a visible, iconic taskbar window that stays minimized for one second. The existing close-to-tray path is unchanged when the option is off.

Rejected process IDs are cached only for one window scan; later scans and WinEvent callbacks can inspect them again. Version-folder equivalence requires the entire suffix to contain numeric components separated by single dots, not just a leading digit.

Startup bounces do not count as successful hides until the window stays hidden, or minimized in taskbar mode, for one second. A window first discovered in that state after launch is not immediately considered successful. Pre-existing hidden/minimized windows may settle without another request. Confirmed actions are capped at three; quiet and maximum observation periods are bounded.

Automatic startup first waits until Windows Network List Manager reports IPv4 or IPv6 Internet connectivity. An associated Wi-Fi network without Internet does not release the wait. The pending helper sleeps between checks, holds only the session watcher lock, and installs no window hooks or launches while offline. Disabling registration/approval cancels before launch. Unknown Internet status logs a fallback rather than waiting forever on a failed API.

Reload preferences after network readiness, then start the observation clock and existing watcher. `GraceSeconds` and `MaxSeconds` apply to this observation, not the preceding offline wait. The offline wait has no fixed timeout. Manual `--run` does not query connectivity; like other runs, it returns code 3 if another worker owns the session lock. Startup/configuration mutation locks are not held during the offline wait, so users can edit preferences or disable startup.

This is only the initial automatic-startup connection. Once the watcher exits, manual reopening and later reconnects/resumes are not intercepted. Zalo's actual server readiness is not inferred from Windows connectivity. Tests provide a deterministic Internet-status callback instead of changing the host network; production reads only the local Windows API and sends no custom connectivity probes.

Message processing checks the same maximum deadline as the outer watcher loop. A continuously nonempty queue must not postpone that check indefinitely.

Electron renderer/helper processes can have the same image path without a main window. Success requires settled eligible windows for each configured target, not one window for every process. Process-name overrides use the same precedence for discovery and completion. Running processes alone do not prove success.

Opacity changes are optional, off by default, and limited to existing readable global layered opacity. Never add layered styles or synchronously message a potentially frozen UI thread. Restore only opacity still owned by the helper.

## Startup ownership

Startup registration is per user, even for a machine-wide install. The app must not silently replace another copy's registration. Saved `LaunchAtStartup` is not proof that Windows startup is enabled.

Read owned registration separately from Windows approval. Strict `REG_SZ`/`REG_EXPAND_SZ` parsing rejects unsupported or malformed values without overwriting them. Native-view `StartupApproved\Run` and `Run32` records distinguish enabled, disabled, and unknown approval. The app's own startup toggle rewrites only its `ZaloSnooze` approval record (enabled or disabled, timestamped) in both buckets, so Task Manager and Settings > Startup apps reflect the app's choice; other approval records are only read. The UI refreshes on activation and links to Windows startup settings; uninstall checks ownership even when Windows disabled startup.

A fail-fast, recursive `Global\ZaloSnooze.Startup.<user SID>` mutex serializes startup mutations across copies and sessions, including public mutation functions and uninstall recovery. Keep consent prompts outside the mutation lock. Reload preferences under the lock, then pin auto-detected custom paths before removing their discovery entries. Standard installations remain automatic.

Zalo startup discovery carries the verified executable target with every Run entry, shortcut, and task. Include those targets in automatic handoff resolution before removing discovery entries. Scheduled tasks must belong to the current user, contain one verified executable action, and have a logon trigger. Common/machine-wide entries are not edited through the normal per-user toggle.

Before removing a Run entry, disabling a task, or removing a Startup shortcut, persist its recovery record. Replacing the same Run entry, shortcut, or task keeps only its newest record; restoration also prefers the newest record in older backups. Retain partial-failure backups. On restore, compare ownership and current values; never overwrite newer conflicting changes. Task restoration compares the definition, ignoring only the app's own change to Settings/Enabled. Newer trigger, action, or principal changes retain the backup instead of being overwritten.

Shortcut recovery compares full file bytes, not only executable targets. Keep backups when arguments, working directory, or any other bytes differ. Delete saved shortcut files only after the updated recovery record is committed.

Production recovery passes the native current-user Startup directory explicitly; tests pass an isolated folder to exercise the same engine without touching live startup files. Shortcut records must name `.lnk` targets directly inside that directory. Legacy migration accepts only this copy's exact executable with empty or `--startup` arguments, and retains differing existing backups.

Tools' Restore action is enabled only for an owned backup. After confirmation, it uses the same disable-startup operation as the main button: remove only this copy's Run value, save the disabled preference, then restore owned Zalo entries. Failed restoration retains recovery data and reports the error.

Machine-specific recovery state lives in Local AppData, even in portable mode, so copying a USB folder cannot replay another PC's startup backup. Application preferences remain portable.

## Installation boundaries

Inno Setup installs only declared files and shortcuts. It writes the storage marker, not user preferences. Optional startup configuration runs as the original user, not a credential-elevation administrator.

The Start Menu contains only the app shortcut. Upgrade cleanup removes the old uninstall shortcut; Windows uninstall registration and the real uninstaller remain. There is no in-app uninstall action.

Unelevated per-user uninstall cleans only the executing account's integration. All-user uninstall skips per-user cleanup and instructs each user to disable integration in a normal app first. App entry points reject elevated startup setup and recovery before mutations; lower-level engine tests use isolated data on either normal or elevated test hosts. No user-writable recovery file is replayed by an elevated production entry point. AppData settings remain. No recursive deletion of a custom install directory is allowed.

Build output must be x86 and import only allowed Windows system DLLs. Keep the dependency check when changing compilers or libraries.
