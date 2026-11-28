# Changelog

## 1.0.1

- Turning startup on or off now writes the Windows startup approval record, so Task Manager and Settings > Startup apps follow the app instead of silently blocking sign-in startup.
- Enabling startup checks Windows approval first. When Windows has blocked the entry, the app says so and keeps the registration ready for when it is allowed.
- Startup status is re-read when the window is activated, so changes made in Windows appear without restarting the app.

## 1.0.0

- First public release for Windows. Start Zalo at sign-in in the tray or minimized to the taskbar.
- Includes a Setup installer and a portable x86 ZIP, with Vietnamese and English settings.
