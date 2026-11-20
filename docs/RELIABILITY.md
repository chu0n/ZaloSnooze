# Reliability and test coverage

[Architecture](ARCHITECTURE.md) · [Development](DEVELOPMENT.md) · [Debugging](DEBUGGING.md)

## Contract

ZaloSnooze is a one-shot desktop helper. It starts missing configured Zalo instances, watches eligible main windows in the current session, requests Close-to-tray or minimizes them to the taskbar when selected, and exits after a quiet period or maximum observation time. It never kills Zalo or injects a library.

Automatic startup waits for initial Windows-reported Internet access before launching Zalo and starting the observation clock. This includes a first connection to a new Wi-Fi network after an offline sign-in. The offline wait has no fixed timeout and ends if startup ownership or approval changes. Manual runs do not use this readiness gate.

Success means matching running processes and confirmed settled hidden main windows, or confirmed minimized taskbar windows when that option is enabled. It does not prove login, message delivery, notification behavior, or the actual tray/taskbar button's presence.

Window recognition is external and asynchronous. A first frame may appear before a callback. This is not a privacy boundary or a guarantee against screen disclosure.

## Safety properties

- Missing INI uses defaults; unreadable existing INI fails before launches or hooks.
- Only the current Windows session is eligible. Normal matching uses process paths; process-name overrides intentionally weaken that check.
- Window operations are asynchronous. Startup bounces do not consume confirmed-hide/minimize retries, and confirmed actions are capped.
- Portable settings never silently fall back to AppData. Atomic saves preserve comments, unknown keys, and an existing file's ACL.
- Editors and startup setup share a cross-session configuration lock. A per-user mutex serializes startup changes and recovery.
- Startup changes require explicit action. Verified entries are backed up before removal or disabling; replacing the same entry keeps only the newest generation. Custom targets remain saved after handoff.
- Windows startup approval is read, never overwritten. Copied preferences do not grant startup registration on another PC.
- Restore never overwrites newer conflicting entries. Saved shortcut files remain until recovery records commit.
- Elevated app entry points refuse startup setup and recovery. All-user uninstall leaves per-user integration untouched; each user must disable it from a normal app first.
- Logs cannot overwrite preferences, application binaries, the installed-mode marker, recovery data, or the legacy startup-shortcut backup, including existing hard-link aliases.
- Setup removes only owned files and registration. Settings and unrelated files survive uninstall.
- Experimental opacity suppression is off by default. Normal use does not change layered opacity.

## Verification

Run from a Windows interactive desktop after installing the tools in [Development](DEVELOPMENT.md):

```powershell
.\.dev\build.ps1
.\.dev\test.ps1
.\.dev\package.ps1 -Tag v1.0.0
.\.dev\tests\installer-smoke.ps1
.\.dev\tests\release-smoke.ps1
```

The local verification baseline contains **420 native assertions**, plus GUI, installer, and package checks. These checks have passed on Windows 11 x64 with the MINGW32 toolchain and Inno Setup 6. They are not proof of untested platforms or a remote CI run.

| Gate | Coverage |
| --- | --- |
| Native core | Unicode, INI parsing/preservation, locked files, preserved ACLs, portable/installed storage, protected logs including the legacy shortcut backup, version-folder matching, and window-state transitions. |
| Process matching | x86 queries of an x64 parent, independent custom installations, unrelated multi-window processes, and mixed process overrides requiring every target to settle. |
| Startup and recovery | Registration ownership/formats, Windows approval variants, cross-process locks, newest-generation Run/shortcut/task recovery, legacy conflicts, record-commit failures, and safe retries. |
| Watcher fixtures | Visible/hidden/minimized windows, startup bounces, frozen UI, process exit, windowless helpers, reopening, opacity restoration, and a continuously nonempty message queue. |
| Offline startup | Waiting longer than the observation limit without launching; a delayed first online window; an already-running hidden target revealing itself; preference reload; registration/approval cancellation; unknown-status fallback; and manual runs/reopening. |
| GUI | Bilingual layout, text/control bounds, icons, actual startup state, editor/setup exclusion, invalid-setting repair, Save/Cancel, custom/multiple paths, side-effect-free close, and bounded cross-process messages. |
| Installer | Isolated per-user install/upgrade/uninstall, shortcuts, Windows registration, preference preservation, and unrelated-file preservation. |
| Packages and release | Source/tag/binary version agreement, x86 PE, system-DLL imports, exact portable contents/defaults/hash, extracted-app GUI, per-tag publication serialization, and mocked draft publication and failure handling. |

Registry mutation tests redirect HKCU into disposable keys. Recovery tests use an isolated Startup directory. Fixtures own their processes and never kill the user's Zalo. GitHub publication checks use mocked commands and do not publish anything.

Offline-startup tests simulate Windows connectivity without disconnecting the host. A native x86 Windows Internet query was also checked on the connected host; this does not substitute for a real offline sign-in and new-Wi-Fi test.

## Limits and unverified cases

- Real sign-in after reboot, fresh QR login, and multiple independent real Zalo installations need dedicated tests.
- A real offline sign-in followed by joining a new Wi-Fi network still needs end-to-end verification with Zalo. Windows Internet status can differ from actual Zalo-server reachability; an unavailable status API falls back to bounded observation.
- Later reconnects or laptop resumes after the helper has exited are outside this one-shot startup behavior. A pending offline startup can tuck an already-running window when its first connection returns.
- Live Windows Startup Apps toggling and startup mutations from separate Windows sessions need verification beyond local sandboxes.
- ARM64 x86 emulation and a real x86 Windows machine have not been tested. Native x64 binaries are not shipped.
- Elevated all-user installation/removal, credential-switch UAC, and live scheduled-task rollback need separate verification.
- GitHub Actions and tagged publication need a remote run after upload. Mocked publication checks do not prove GitHub permissions or availability.
- Broader themes, high contrast, and DPI changes across monitors need visual testing.
- Force-killing an experimental-opacity watcher can bypass cleanup and leave Zalo transparent. Quit Zalo from its tray and reopen it.
- Observation limits do not bound every OS, disk, COM, or diagnostic-write delay.
- Executables are unsigned. Checksums establish integrity, not publisher identity.
- MSYS2's deprecated MINGW32 environment remains a build-tool availability risk. Preserve x86 and dependency checks during any toolchain change.
