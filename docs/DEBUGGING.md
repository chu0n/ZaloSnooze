# Debugging the native helper

[Development](DEVELOPMENT.md) · [Reliability](RELIABILITY.md)

## Start with diagnostics

Open Tools → Experimental, enable diagnostic logging, and choose Save. A relative log is written beside the active INI. The default filename is `ZaloSnooze.log`; logs rotate above approximately 1 MB into `.old`. If that file already exists, logging pauses at the size limit until you move or remove it.

Run `ZaloSnooze.exe --run` once from the app folder. Check its exit code and log for launch failure, queued Close, confirmed hidden state, and final result. Do not infer success from a queued request alone. Logs cannot target preferences, application binaries, or startup recovery files.

The menu's settings-folder action shows where configuration is stored. For portable mode, keep the EXE and INI together. If writing fails, move to a writable folder rather than running the helper as administrator.

Before sharing logs, inspect paths and any Windows error details for personal information. Native diagnostics avoid dumping chat contents or arbitrary window titles.

## Test a failure without touching real Zalo

Run `.\.dev\test.ps1`. The native fixture tests cover startup bounces, busy UI, Close-to-tray, helper processes without windows, and process exit on Close. A failure retains its own temporary evidence directory and prints its path.

Never use broad process-name kill commands for test cleanup. Native tests hold handles to their own fixture processes.

## Startup troubleshooting

- The metadata shows this copy's registration and Windows approval: On, Off, Disabled by Windows, or Unknown.
- Use Tools → Windows startup settings to inspect approval. The startup button also rewrites the app's own approval record, so enabling in ZaloSnooze re-enables the entry in Task Manager / Settings > Startup apps, and the metadata stays truthful.
- Return to the app to refresh approval. Unknown state does not silently enable or replace registration.
- A copied INI does not enable startup on a new PC.
- If another copy owns registration, disable it there first.
- Moving or renaming a portable executable requires re-registering startup.
- Use Tools → Restore Zalo startup to disable the helper's startup and recover this copy's saved entries.
- Native recovery records live in `%LOCALAPPDATA%\ZaloSnooze\startup-state`.
- Keep conflict backups. Do not delete them to hide an error or overwrite newer startup entries manually.
- If another startup change is in progress, wait for it to finish and retry. Copies share a per-user mutation lock across Windows sessions.
- Close the editor before command-line startup setup for the same configuration. Editors and setup share a cross-session configuration lock.
- Reopen normally if startup setup or recovery reports administrator rights. Elevated all-user uninstall deliberately skips per-user recovery.
- Invalid settings appear inline without hiding Tools. Use Experimental to disable an unsafe log path or correct other advanced fields.
- If startup is enabled but Zalo has not launched, check Windows' Internet status. Automatic startup waits while Windows reports offline; Wi-Fi association alone may not satisfy that check.
- Logs distinguish waiting for Internet, readiness, cancellation, and unavailable-status fallback. The observation timer begins after the wait, not at Windows sign-in.
- Disable startup to cancel a pending offline worker. Open Zalo directly for offline use; a concurrent helper `--run` returns code 3 while that worker owns the session lock.
- VPNs and captive portals can affect Windows' Internet status. The helper does not probe Zalo servers or monitor later reconnects after exiting.

Task Scheduler policies and elevated Zalo processes can prevent normal-user operations. The helper must report failure rather than elevate itself silently.

## Native debugger

Use the MSYS2 MINGW32 debugger or WinDbg. `.dev/build/` contains normal compiler outputs; configure a separate Debug build when symbols are needed. Do not attach a debugger to the user's Zalo unless the investigation requires it and the user approves.

Observe HWND, PID, session, executable path, visible state, queued Close, and the confirmed-hide counter. Keep observation bounded. Avoid synchronous cross-process UI calls that can stall on a frozen app.

When detaching, preserve normal execution and cleanup. Do not terminate Zalo as a substitute for detaching.

## Experimental transparency recovery

The experimental opacity option defaults to off. If force termination leaves Zalo transparent, exit Zalo through its tray and reopen it. Do not add layered window styles or install a recovery service as a workaround.

Before changing the implementation, check the invariants in ARCHITECTURE and add a regression fixture for the observed failure.
