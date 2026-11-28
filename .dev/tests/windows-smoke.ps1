param(
    [string]$Executable = (Join-Path (Split-Path $PSScriptRoot -Parent) 'build\ZaloSnooze.exe')
)
$ErrorActionPreference = 'Stop'
$dev = Split-Path $PSScriptRoot -Parent
$root = Split-Path $dev -Parent
$mainTitle = 'ZaloSnooze v' + [Diagnostics.FileVersionInfo]::GetVersionInfo($Executable).ProductVersion
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('ZaloSnooze-ui-tests-' + [guid]::NewGuid().ToString('N'))
$process = $null
$duplicate = $null
$elevated = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class SnoozeUiTest {
    private delegate bool Callback(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] private static extern bool EnumWindows(Callback callback, IntPtr data);
    [DllImport("user32.dll")] private static extern bool EnumChildWindows(IntPtr parent, Callback callback, IntPtr data);
    [DllImport("user32.dll")] private static extern int GetDlgCtrlID(IntPtr h);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
    [DllImport("user32.dll")] private static extern IntPtr GetMenu(IntPtr h);
    [DllImport("user32.dll")] private static extern IntPtr GetSubMenu(IntPtr menu,int index);
    [DllImport("user32.dll")] private static extern int GetMenuItemCount(IntPtr menu);
    [DllImport("user32.dll")] private static extern uint GetMenuItemID(IntPtr menu,int index);
    [DllImport("user32.dll")] private static extern uint GetMenuState(IntPtr menu,uint id,uint flags);
    private static uint CommandState(IntPtr menu,uint id) {
        for(int i=0;i<GetMenuItemCount(menu);++i) {
            if(GetMenuItemID(menu,i)==id)return GetMenuState(menu,(uint)i,0x400);
            var sub=GetSubMenu(menu,i);if(sub!=IntPtr.Zero){var state=CommandState(sub,id);if(state!=uint.MaxValue)return state;}
        }
        return uint.MaxValue;
    }
    public static bool MenuEnabled(IntPtr window,uint id) {var state=CommandState(GetMenu(window),id);return state!=uint.MaxValue && (state&3)==0;}
    private static bool MenuContains(IntPtr menu,uint id) {
        for(int i=0;i<GetMenuItemCount(menu);++i) {
            if(GetMenuItemID(menu,i)==id)return true;
            var sub=GetSubMenu(menu,i);if(sub!=IntPtr.Zero && MenuContains(sub,id))return true;
        }
        return false;
    }
    public static bool HasMenuCommand(IntPtr window,uint id) {return MenuContains(GetMenu(window),id);}
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint message, IntPtr wp, IntPtr lp);
    private const uint MessageTimeoutMilliseconds = 2000;
    private const uint MessageBlock = 1;
    private const uint MessageAbortIfHung = 2;
    [DllImport("user32.dll", SetLastError=true)] private static extern IntPtr SendMessageTimeout(IntPtr h,uint message,IntPtr wp,IntPtr lp,uint flags,uint timeout,out IntPtr result);
    [DllImport("user32.dll", EntryPoint="SendMessageTimeoutW", CharSet=CharSet.Unicode, SetLastError=true)] private static extern IntPtr SendMessageTimeoutText(IntPtr h,uint message,IntPtr wp,string text,uint flags,uint timeout,out IntPtr result);
    [DllImport("user32.dll", EntryPoint="SendMessageTimeoutW", CharSet=CharSet.Unicode, SetLastError=true)] private static extern IntPtr SendMessageTimeoutBuffer(IntPtr h,uint message,IntPtr wp,StringBuilder text,uint flags,uint timeout,out IntPtr result);
    private static IntPtr Checked(IntPtr sent,IntPtr result,uint message) {
        if(sent==IntPtr.Zero) throw new TimeoutException("Window message " + message + " timed out or failed (error " + Marshal.GetLastWin32Error() + ").");
        return result;
    }
    public static IntPtr SendMessage(IntPtr h,uint message,IntPtr wp,IntPtr lp) {
        IntPtr result;var sent=SendMessageTimeout(h,message,wp,lp,MessageBlock|MessageAbortIfHung,MessageTimeoutMilliseconds,out result);return Checked(sent,result,message);
    }
    public static IntPtr SendText(IntPtr h,uint message,IntPtr wp,string text) {
        IntPtr result;var sent=SendMessageTimeoutText(h,message,wp,text,MessageBlock|MessageAbortIfHung,MessageTimeoutMilliseconds,out result);return Checked(sent,result,message);
    }
    private static IntPtr ReadText(IntPtr h,uint message,IntPtr wp,StringBuilder text) {
        IntPtr result;var sent=SendMessageTimeoutBuffer(h,message,wp,text,MessageBlock|MessageAbortIfHung,MessageTimeoutMilliseconds,out result);return Checked(sent,result,message);
    }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out Rect rect);
    [DllImport("user32.dll")] private static extern bool GetIconInfo(IntPtr h, out IconInfo info);
    [DllImport("gdi32.dll", CharSet=CharSet.Unicode)] private static extern int GetObject(IntPtr h, int size, out BitmapInfo info);
    [DllImport("gdi32.dll")] private static extern bool DeleteObject(IntPtr h);
    private struct IconInfo { public int Icon; public uint HotX,HotY; public IntPtr Mask,Color; }
    private struct BitmapInfo { public int Type,Width,Height,WidthBytes; public ushort Planes,Bits; public IntPtr Pixels; }
    public static int IconWidth(IntPtr window, int kind) {
        var icon=SendMessage(window,0x7F,(IntPtr)kind,IntPtr.Zero);
        IconInfo info;if(icon==IntPtr.Zero || !GetIconInfo(icon,out info))return 0;
        try{BitmapInfo bitmap;return GetObject(info.Color,Marshal.SizeOf(typeof(BitmapInfo)),out bitmap)>0?bitmap.Width:0;}
        finally{if(info.Mask!=IntPtr.Zero)DeleteObject(info.Mask);if(info.Color!=IntPtr.Zero)DeleteObject(info.Color);}
    }
    public struct Rect { public int Left,Top,Right,Bottom; }
    public static IntPtr[] Windows(uint pid) {
        var result = new List<IntPtr>();
        EnumWindows((h,p) => { uint current; GetWindowThreadProcessId(h,out current); if(current==pid && IsWindowVisible(h))result.Add(h); return true; },IntPtr.Zero);
        return result.ToArray();
    }
    public static string Title(IntPtr h) { var value=new StringBuilder(1024); ReadText(h,0xD,(IntPtr)1024,value); return value.ToString(); }
    public static string ListText(IntPtr h,int index) {var value=new StringBuilder(32768);ReadText(h,0x189,(IntPtr)index,value);return value.ToString();}
    public static string Describe(IntPtr h) {
        var result=new StringBuilder();
        EnumChildWindows(h,(child,p)=>{result.Append(GetDlgCtrlID(child)).Append(": ").Append(Title(child)).Append("; ");return true;},IntPtr.Zero);
        return result.ToString();
    }
}
'@
function Wait-Window {
    param([int]$ProcessId, [string]$Title)
    $clock = [Diagnostics.Stopwatch]::StartNew()
    do {
        foreach ($window in [SnoozeUiTest]::Windows($ProcessId)) {
            if ([SnoozeUiTest]::Title($window) -eq $Title) { Start-Sleep -Milliseconds 100; return $window }
        }
        Start-Sleep -Milliseconds 50
    } while ($clock.Elapsed.TotalSeconds -lt 8)
    throw "Window not found: $Title"
}
function Click-Action { param([IntPtr]$Window, [int]$Action) [void][SnoozeUiTest]::PostMessage($Window, 0x111, [IntPtr]$Action, [IntPtr]::Zero) }
function Check-Controls {
    param([IntPtr]$Window, [int[]]$Ids)
    foreach ($id in $Ids) { if ([SnoozeUiTest]::GetDlgItem($Window, $id) -eq [IntPtr]::Zero) { throw "Control $id is missing." } }
}
function Check-Main {
    param([IntPtr]$Window)
    Check-Controls $Window @(1001,1002,1004,1006,1007,1008,1101,1105,1117)
    if ([SnoozeUiTest]::IsWindowVisible([SnoozeUiTest]::GetDlgItem($Window,1003))) { Check-Controls $Window @(1003) }
    foreach ($removed in @(1009,1104,1115)) { if ([SnoozeUiTest]::GetDlgItem($Window,$removed) -ne [IntPtr]::Zero) { throw 'Optional actions still occupy main-window rows.' } }
    foreach ($menu in @(1104,40008)) { if (-not [SnoozeUiTest]::HasMenuCommand($Window,$menu)) { throw 'Tools menu is missing a required action.' } }
    foreach ($id in @(1006,1007)) {
        if ([SnoozeUiTest]::SendMessage([SnoozeUiTest]::GetDlgItem($Window,$id),0xF6,[IntPtr]1,[IntPtr]::Zero) -eq [IntPtr]::Zero) { throw 'Path action is missing its native icon.' }
        if (-not [SnoozeUiTest]::Title([SnoozeUiTest]::GetDlgItem($Window,$id))) { throw 'Icon button has no accessible label.' }
    }
    if ([SnoozeUiTest]::Title([SnoozeUiTest]::GetDlgItem($Window,1006)) -match 'Test|Thử ngay') { throw 'Test now was not removed.' }
    if ([SnoozeUiTest]::Title($Window) -cne $mainTitle) { throw 'Version missing from main title.' }
    if ([SnoozeUiTest]::Title([SnoozeUiTest]::GetDlgItem($Window,1001)) -match '\d+\.\d+\.\d+') { throw 'Version duplicated in main content.' }
    if ([SnoozeUiTest]::HasMenuCommand($Window,40009)) { throw 'In-app uninstall action is still present.' }
}
function Check-BoundedMessage {
    param([string]$Fixture, [string]$Marker)
    $fixtureProcess = Start-Process -FilePath $Fixture -ArgumentList ('"' + $Marker + '" busy') -PassThru
    try {
        $clock = [Diagnostics.Stopwatch]::StartNew()
        while (-not (Test-Path -LiteralPath $Marker)) {
            if ($clock.Elapsed.TotalSeconds -gt 5) { throw 'Message-timeout fixture did not start.' }
            Start-Sleep -Milliseconds 20
        }
        Start-Sleep -Milliseconds 300
        $window = [IntPtr][long](Get-Content -LiteralPath $Marker -Raw)
        $probe = [Diagnostics.Stopwatch]::StartNew()
        $timedOut = $false
        try { [void][SnoozeUiTest]::Title($window) } catch [TimeoutException] { $timedOut = $true }
        if (-not $timedOut -or $probe.Elapsed.TotalSeconds -gt 5) { throw 'Cross-process GUI read did not fail within its bound.' }
    } finally {
        if (-not $fixtureProcess.HasExited) { Stop-Process -Id $fixtureProcess.Id -Force }
        $fixtureProcess.Dispose()
    }
}
try {
    New-Item -ItemType Directory -Path $temporary | Out-Null
    $exe = Join-Path $temporary 'ZaloSnooze.exe'
    $ini = Join-Path $temporary 'ZaloSnooze.ini'
    Copy-Item -LiteralPath $Executable -Destination $exe
    Copy-Item -LiteralPath (Join-Path $dev 'config\ZaloSnooze.ini') -Destination $ini
    Check-BoundedMessage (Join-Path $dev 'build\tray_fixture.exe') (Join-Path $temporary 'message-timeout.ready')
    $originalStartup = (Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -ErrorAction SilentlyContinue).ZaloSnooze
    $process = Start-Process -FilePath $exe -PassThru
    $window = Wait-Window $process.Id $mainTitle
    Start-Sleep -Milliseconds 150
    Check-Main $window
    $smallIcon = [SnoozeUiTest]::IconWidth($window,0)
    if ($smallIcon -le 0 -or [SnoozeUiTest]::IconWidth($window,1) -le $smallIcon) { throw 'Window icons do not use separate small and large sizes.' }
    if ([SnoozeUiTest]::MenuEnabled($window,40008)) { throw 'Restore must be disabled without an owned backup.' }
    $minimizeControl = [SnoozeUiTest]::GetDlgItem($window,1117)
    if ($minimizeControl -eq [IntPtr]::Zero) { throw 'Minimize-to-taskbar option is missing.' }
    [void][SnoozeUiTest]::SendMessage($minimizeControl,0xF5,[IntPtr]::Zero,[IntPtr]::Zero)
    Start-Sleep -Milliseconds 150
    if ((Get-Content -LiteralPath $ini -Raw -Encoding UTF8) -notmatch '(?m)^MinimizeToTaskbar=true\r?$') { throw 'Minimize-to-taskbar option was not saved.' }
    Check-Main $window
    [void][SnoozeUiTest]::SendMessage($minimizeControl,0xF5,[IntPtr]::Zero,[IntPtr]::Zero)
    Start-Sleep -Milliseconds 150
    if ((Get-Content -LiteralPath $ini -Raw -Encoding UTF8) -notmatch '(?m)^MinimizeToTaskbar=false\r?$') { throw 'Minimize-to-taskbar option did not toggle off.' }
    $lockedIni = [IO.File]::Open($ini, 'Open', 'Read', 'Read')
    try {
        [void][SnoozeUiTest]::PostMessage($minimizeControl,0xF5,[IntPtr]::Zero,[IntPtr]::Zero)
        $saveError = Wait-Window $process.Id 'ZaloSnooze'
        if ([SnoozeUiTest]::SendMessage($minimizeControl,0xF0,[IntPtr]::Zero,[IntPtr]::Zero).ToInt32() -ne 0) { throw 'Failed save left the taskbar checkbox checked.' }
        [void][SnoozeUiTest]::PostMessage($saveError,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
    } finally { $lockedIni.Dispose() }
    $probe = Start-Process -FilePath (Join-Path $dev 'build\snooze_tests.exe') -ArgumentList ('--config-lock-probe "' + $ini + '"') -NoNewWindow -Wait -PassThru
    try { if ($probe.ExitCode -ne 3) { throw 'An open editor did not block command-line startup configuration.' } } finally { $probe.Dispose() }
    if ([SnoozeUiTest]::GetDlgItem($window, 1002) -eq [IntPtr]::Zero) { throw 'Main startup control missing.' }
    $duplicate = Start-Process -FilePath $exe -PassThru
    $notice = Wait-Window $duplicate.Id 'ZaloSnooze'
    if ([SnoozeUiTest]::GetDlgItem($notice, 1002) -ne [IntPtr]::Zero) { throw 'Duplicate settings editor was allowed.' }
    $ok = [SnoozeUiTest]::GetDlgItem($notice, 1)
    if ($ok -eq [IntPtr]::Zero) { $ok = [SnoozeUiTest]::GetDlgItem($notice, 2) }
    $ready = [Diagnostics.Stopwatch]::StartNew()
    while ($ok -eq [IntPtr]::Zero -and $ready.Elapsed.TotalSeconds -lt 5) {
        Start-Sleep -Milliseconds 50
        $ok = [SnoozeUiTest]::GetDlgItem($notice, 1)
        if ($ok -eq [IntPtr]::Zero) { $ok = [SnoozeUiTest]::GetDlgItem($notice, 2) }
    }
    if ($ok -eq [IntPtr]::Zero) { throw ('Duplicate-editor notice has no OK button. Controls: ' + [SnoozeUiTest]::Describe($notice)) }
    [void][SnoozeUiTest]::PostMessage($ok, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero)
    if (-not $duplicate.WaitForExit(5000)) { throw 'Duplicate-editor notice did not close.' }
    Click-Action $window 40005
    Start-Sleep -Milliseconds 250
    if (-not (Get-Content -LiteralPath $ini -Raw -Encoding UTF8).Contains('Language=en')) { throw 'Language menu did not save portable configuration.' }
    Check-Main $window
    if ([SnoozeUiTest]::Title([SnoozeUiTest]::GetDlgItem($window,1001)) -cne 'Portable · Automatic startup: Off') { throw 'Metadata does not show this copy installation type and actual startup state.' }
    if (-not [SnoozeUiTest]::Title([SnoozeUiTest]::GetDlgItem($window,1008)).Contains('Windows sign-in')) { throw 'First-use instructions do not explain sign-in behavior.' }
    if ([SnoozeUiTest]::Title([SnoozeUiTest]::GetDlgItem($window,1002)) -cne 'Enable automatic startup') { throw 'Startup button does not describe the actual next action.' }
    Click-Action $window 40006
    $about = Wait-Window $process.Id 'About ZaloSnooze'
    Check-Controls $about @(1206,1207,1)
    if ([SnoozeUiTest]::SendMessage([SnoozeUiTest]::GetDlgItem($about,1206),0x173,[IntPtr]1,[IntPtr]::Zero) -ne [SnoozeUiTest]::SendMessage($window,0x7F,[IntPtr]1,[IntPtr]::Zero)) { throw 'About does not use the app icon.' }
    Click-Action $about 1
    Start-Sleep -Milliseconds 100
    $compactBounds = [SnoozeUiTest+Rect]::new()
    [void][SnoozeUiTest]::GetWindowRect($window,[ref]$compactBounds)
    [void][SnoozeUiTest]::SendText([SnoozeUiTest]::GetDlgItem($window,1003),0xC,[IntPtr]::Zero,'Could not confirm success. Check the selected Zalo installations in Zalo locations, or enable diagnostic logging under Experimental.')
    [void][SnoozeUiTest]::PostMessage($window,0x800A,[IntPtr]::Zero,[IntPtr]::Zero)
    Start-Sleep -Milliseconds 150
    Check-Main $window
    $expandedBounds = [SnoozeUiTest+Rect]::new()
    [void][SnoozeUiTest]::GetWindowRect($window,[ref]$expandedBounds)
    if ($expandedBounds.Bottom - $expandedBounds.Top -le $compactBounds.Bottom - $compactBounds.Top) { throw 'Long status text did not expand the compact layout.' }
    Click-Action $window 40005
    Start-Sleep -Milliseconds 100
    $before = (Get-FileHash -LiteralPath $ini).Hash
    Click-Action $window 1007
    $settings = Wait-Window $process.Id 'Zalo locations'
    Check-Controls $settings @(1116,1105,1101,1103,1102,1,2)
    if ([SnoozeUiTest]::IconWidth($settings,0) -le 0) { throw 'Settings did not inherit the app icon.' }
    if ([SnoozeUiTest]::GetDlgItem($settings,1104) -ne [IntPtr]::Zero) { throw 'Experimental action must be in Tools, not nested inside locations.' }
    Click-Action $settings 2
    Start-Sleep -Milliseconds 100
    Click-Action $window 1104
    Start-Sleep -Milliseconds 100
    $advanced = Wait-Window $process.Id 'Experimental'
    Check-Controls $advanced @(1106,1107,1108,1109,1110,1111,1112,1113,1114,1,2)
    [void][SnoozeUiTest]::SendText([SnoozeUiTest]::GetDlgItem($advanced,1109),0xC,[IntPtr]::Zero,'2')
    Click-Action $advanced 2
    Start-Sleep -Milliseconds 150
    if ((Get-FileHash -LiteralPath $ini).Hash -ne $before) { throw 'Cancel modified configuration.' }
    Click-Action $window 1104
    $advanced = Wait-Window $process.Id 'Experimental'
    [void][SnoozeUiTest]::SendText([SnoozeUiTest]::GetDlgItem($advanced,1109),0xC,[IntPtr]::Zero,'2')
    if ([SnoozeUiTest]::Title([SnoozeUiTest]::GetDlgItem($advanced,1109)) -ne '2') { throw 'Could not edit quiet period in test dialog.' }
    Click-Action $advanced 1
    Start-Sleep -Milliseconds 150
    if ((Get-Content -LiteralPath $ini -Raw -Encoding UTF8) -notmatch '(?m)^GraceSeconds=2\r?$') {
        throw 'Experimental Save did not persist changes.'
    }
    $before = (Get-FileHash -LiteralPath $ini).Hash
    [void][SnoozeUiTest]::PostMessage($window, 0x10, [IntPtr]::Zero, [IntPtr]::Zero)
    if (-not $process.WaitForExit(8000) -or $process.ExitCode -ne 0) { throw 'GUI did not close cleanly.' }
    if ((Get-FileHash -LiteralPath $ini).Hash -ne $before) { throw 'Closing GUI modified configuration.' }
    $afterStartup = (Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -ErrorAction SilentlyContinue).ZaloSnooze
    if ($originalStartup -cne $afterStartup) { throw 'GUI open/close changed startup registration.' }
    $process.Dispose(); $process = $null
    $invalidDirectory = Join-Path $temporary 'invalid-settings'
    New-Item -ItemType Directory -Path $invalidDirectory | Out-Null
    $invalidExe = Join-Path $invalidDirectory 'ZaloSnooze.exe'
    $invalidIni = Join-Path $invalidDirectory 'ZaloSnooze.ini'
    Copy-Item -LiteralPath $Executable -Destination $invalidExe
    [IO.File]::WriteAllText($invalidIni,"Language=en`r`nLogFile=ZaloSnooze.ini`r`n",[Text.UTF8Encoding]::new($true))
    $invalidHash = (Get-FileHash -LiteralPath $invalidIni).Hash
    $process = Start-Process -FilePath $invalidExe -PassThru
    $window = Wait-Window $process.Id $mainTitle
    Check-Main $window
    if (-not [SnoozeUiTest]::Title([SnoozeUiTest]::GetDlgItem($window,1003)).Contains('Diagnostic logs cannot overwrite')) { throw 'Invalid log settings were not reported inline.' }
    if ((Get-FileHash -LiteralPath $invalidIni).Hash -cne $invalidHash) { throw 'Opening invalid preferences rewrote them.' }
    Click-Action $window 1104
    $advanced = Wait-Window $process.Id 'Experimental'
    [void][SnoozeUiTest]::SendMessage([SnoozeUiTest]::GetDlgItem($advanced,1113),0xF1,[IntPtr]::Zero,[IntPtr]::Zero)
    Click-Action $advanced 1
    Start-Sleep -Milliseconds 150
    if ((Get-Content -LiteralPath $invalidIni -Raw -Encoding UTF8) -notmatch '(?m)^LogFile=""\r?$') { throw 'Experimental could not repair an invalid log path.' }
    if ([SnoozeUiTest]::Title([SnoozeUiTest]::GetDlgItem($window,1003)).Contains('Diagnostic logs cannot overwrite')) { throw 'Repaired log settings did not clear the error.' }
    [void][SnoozeUiTest]::PostMessage($window,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
    if (-not $process.WaitForExit(8000) -or $process.ExitCode -ne 0) { throw 'Repaired settings editor did not exit cleanly.' }
    $process.Dispose(); $process = $null
    $first = Join-Path $temporary 'Account, A\Zalo.exe'
    $second = Join-Path $temporary 'Office B\Zalo.exe'
    foreach ($file in @($first,$second)) { New-Item -ItemType Directory -Path (Split-Path $file) -Force | Out-Null; Copy-Item -LiteralPath $exe -Destination $file }
    [IO.File]::WriteAllText($ini, ('Language=en' + "`r`nLaunchAtStartup=1`r`n" + 'ZaloExe="' + $first + '","' + $second + '"' + "`r`nKeepThis=preserved`r`n"), [Text.Encoding]::UTF8)
    $process = Start-Process -FilePath $exe -PassThru
    $window = Wait-Window $process.Id $mainTitle
    if (-not [SnoozeUiTest]::Title([SnoozeUiTest]::GetDlgItem($window,1001)).EndsWith('Off')) { throw 'Metadata trusted saved preference instead of actual startup registration.' }
    Check-Main $window
    $mainPaths = [SnoozeUiTest]::GetDlgItem($window,1101)
    if ([SnoozeUiTest]::SendMessage($mainPaths,0x18B,[IntPtr]::Zero,[IntPtr]::Zero).ToInt32() -ne 2 -or [SnoozeUiTest]::ListText($mainPaths,0) -cne $first -or [SnoozeUiTest]::ListText($mainPaths,1) -cne $second) { throw 'Main does not show full configured installation paths.' }
    if (-not [SnoozeUiTest]::IsWindowEnabled([SnoozeUiTest]::GetDlgItem($window,1006))) { throw 'Selected installation cannot open its folder.' }
    Click-Action $window 1007
    $settings = Wait-Window $process.Id 'Zalo locations'
    $list = [SnoozeUiTest]::GetDlgItem($settings,1101)
    if ([SnoozeUiTest]::SendMessage($list,0x18B,[IntPtr]::Zero,[IntPtr]::Zero).ToInt32() -ne 2) { throw 'Custom installation list lost a path.' }
    [void][SnoozeUiTest]::SendMessage($list,0x185,[IntPtr]1,[IntPtr]::Zero)
    [void][SnoozeUiTest]::PostMessage($settings,0x111,[IntPtr]((1 -shl 16) -bor 1101),[IntPtr]::Zero)
    Click-Action $settings 1102
    Start-Sleep -Milliseconds 100
    if ([SnoozeUiTest]::SendMessage($list,0x18B,[IntPtr]::Zero,[IntPtr]::Zero).ToInt32() -ne 1) { throw 'Removing one installation affected the wrong number of paths.' }
    Click-Action $settings 1
    Start-Sleep -Milliseconds 150
    $saved = Get-Content -LiteralPath $ini -Raw -Encoding UTF8
    if ($saved.Contains($first) -or -not $saved.Contains($second) -or -not $saved.Contains('KeepThis=preserved')) { throw 'Custom installation save lost or changed unrelated settings.' }
    if ([SnoozeUiTest]::SendMessage($mainPaths,0x18B,[IntPtr]::Zero,[IntPtr]::Zero).ToInt32() -ne 1 -or [SnoozeUiTest]::ListText($mainPaths,0) -cne $second) { throw 'Main paths did not refresh after managing installations.' }
    Remove-Item -LiteralPath $second
    Click-Action $window 40005
    Start-Sleep -Milliseconds 150
    if (-not [SnoozeUiTest]::Title([SnoozeUiTest]::GetDlgItem($window,1003)).Contains('unavailable')) { throw 'Missing custom installation was reported as ready.' }
    Check-Main $window
    Remove-Item -LiteralPath (Split-Path $second -Parent)
    Click-Action $window 1006
    $folderError = Wait-Window $process.Id 'ZaloSnooze'
    if (-not [SnoozeUiTest]::Describe($folderError).Contains('The Zalo folder is unavailable.')) { throw 'Open-folder action did not validate the selected installation folder.' }
    $ok = [SnoozeUiTest]::GetDlgItem($folderError,2)
    if ($ok -eq [IntPtr]::Zero) { $ok = [SnoozeUiTest]::GetDlgItem($folderError,1) }
    [void][SnoozeUiTest]::PostMessage($ok,0xF5,[IntPtr]::Zero,[IntPtr]::Zero)
    Start-Sleep -Milliseconds 100
    New-Item -ItemType Directory -Path (Split-Path $second -Parent) | Out-Null
    Copy-Item -LiteralPath $exe -Destination $second
    Click-Action $window 1007
    $settings = Wait-Window $process.Id 'Zalo locations'
    [void][SnoozeUiTest]::SendMessage([SnoozeUiTest]::GetDlgItem($settings,1116),0xF1,[IntPtr]1,[IntPtr]::Zero)
    Click-Action $settings 1116
    Click-Action $settings 1
    Start-Sleep -Milliseconds 150
    if ((Get-Content -LiteralPath $ini -Raw -Encoding UTF8) -notmatch '(?m)^ZaloExe=\r?$') { throw 'Automatic detection did not clear explicit path selection.' }
    Click-Action $window 40004
    Start-Sleep -Milliseconds 100
    Click-Action $window 1007
    $settings = Wait-Window $process.Id 'Vị trí Zalo'
    Check-Controls $settings @(1116,1105,1101,1103,1102,1,2)
    Click-Action $settings 2
    Start-Sleep -Milliseconds 100
    Click-Action $window 1104
    $advanced = Wait-Window $process.Id 'Thử nghiệm'
    Check-Controls $advanced @(1106,1107,1108,1109,1110,1111,1112,1113,1114,1,2)
    Click-Action $advanced 2
    Start-Sleep -Milliseconds 100
    Click-Action $window 40006
    $about = Wait-Window $process.Id 'Giới thiệu ZaloSnooze'
    Check-Controls $about @(1206,1207,1)
    Click-Action $about 1
    Start-Sleep -Milliseconds 100
    [void][SnoozeUiTest]::PostMessage($window,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
    if (-not $process.WaitForExit(8000)) { throw 'Custom-path GUI did not exit.' }
    $process.Dispose(); $process = $null
    foreach ($mode in @('disabled','unknown')) {
        $sandbox = Join-Path $temporary ('startup-' + $mode)
        $fixture = Join-Path $dev 'build\snooze_tests.exe'
        $process = Start-Process -FilePath $fixture -ArgumentList ('--startup-ui-fixture "' + $sandbox + '" ' + $mode) -PassThru
        $window = Wait-Window $process.Id $mainTitle
        Check-Main $window
        $meta = [SnoozeUiTest]::Title([SnoozeUiTest]::GetDlgItem($window,1001))
        $button = [SnoozeUiTest]::Title([SnoozeUiTest]::GetDlgItem($window,1002))
        if (-not [SnoozeUiTest]::HasMenuCommand($window,40010)) { throw 'Windows startup settings route is missing.' }
        if ($mode -eq 'disabled') {
            if (-not $meta.EndsWith('Disabled by Windows') -or $button -cne 'Remove startup registration') { throw 'Windows-disabled startup is misleadingly reported as On.' }
            $keyPath = 'Software\ZaloSnooze.UiTests.' + $process.Id + '\Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run'
            $registry = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser,[Microsoft.Win32.RegistryView]::Registry64)
            try { $approval = $registry.OpenSubKey($keyPath,$true) } finally { $registry.Dispose() }
            if (-not $approval) { throw 'Cannot open isolated startup approval for writing.' }
            try {
                $approval.SetValue('ZaloSnooze',[byte[]]@(2,0,0,0,0,0,0,0,0,0,0,0),[Microsoft.Win32.RegistryValueKind]::Binary)
                [void][SnoozeUiTest]::PostMessage($window,0x6,[IntPtr]1,[IntPtr]::Zero)
                Start-Sleep -Milliseconds 150
                if (-not [SnoozeUiTest]::Title([SnoozeUiTest]::GetDlgItem($window,1001)).EndsWith('On') -or [SnoozeUiTest]::Title([SnoozeUiTest]::GetDlgItem($window,1002)) -cne 'Disable automatic startup') { throw 'Returning from Windows Settings did not refresh enabled startup.' }
                Check-Main $window
                $approval.SetValue('ZaloSnooze',[byte[]]@(3,0,0,0,0,0,0,0,0,0,0,0),[Microsoft.Win32.RegistryValueKind]::Binary)
                [void][SnoozeUiTest]::PostMessage($window,0x6,[IntPtr]1,[IntPtr]::Zero)
                Start-Sleep -Milliseconds 150
                if (-not [SnoozeUiTest]::Title([SnoozeUiTest]::GetDlgItem($window,1001)).EndsWith('Disabled by Windows')) { throw 'Returning from Windows Settings did not refresh disabled startup.' }
            } finally { $approval.Dispose() }
            Click-Action $window 1002
            if ($elevated) {
                $notice = Wait-Window $process.Id 'ZaloSnooze'
                if (-not [SnoozeUiTest]::Describe($notice).Contains('without administrator rights')) { throw 'Elevated startup changes did not fail closed.' }
                $ok = [SnoozeUiTest]::GetDlgItem($notice,2)
                if ($ok -eq [IntPtr]::Zero) { $ok = [SnoozeUiTest]::GetDlgItem($notice,1) }
                [void][SnoozeUiTest]::PostMessage($ok,0xF5,[IntPtr]::Zero,[IntPtr]::Zero)
                Start-Sleep -Milliseconds 150
                if ((Get-Content -LiteralPath (Join-Path $sandbox 'ZaloSnooze.ini') -Raw -Encoding UTF8) -notmatch '(?m)^LaunchAtStartup=true\r?$') { throw 'Elevated startup action changed preferences.' }
            } else {
                Start-Sleep -Milliseconds 150
                if (-not [SnoozeUiTest]::Title([SnoozeUiTest]::GetDlgItem($window,1001)).EndsWith('Off')) { throw 'Cannot remove this copy registration while Windows disables startup.' }
                if ((Get-Content -LiteralPath (Join-Path $sandbox 'ZaloSnooze.ini') -Raw -Encoding UTF8) -notmatch '(?m)^LaunchAtStartup=false\r?$') { throw 'Removing disabled registration did not save the disabled preference.' }
            }
        } elseif (-not $meta.EndsWith('Unknown') -or $button -cne 'Open Windows startup settings') { throw 'Unknown Windows approval is not handled safely.' }
        [void][SnoozeUiTest]::PostMessage($window,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
        if (-not $process.WaitForExit(8000) -or $process.ExitCode -ne 0) { throw 'Startup-state GUI sandbox failed or rewrote Windows approval.' }
        $process.Dispose(); $process = $null
    }
    if ($originalStartup -cne (Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -ErrorAction SilentlyContinue).ZaloSnooze) { throw 'Sandbox UI tests changed live startup registration.' }
    Write-Host 'PASS: bilingual text and controls, editor/setup exclusion, repairable invalid settings, actual startup approval, privilege policy, icon path actions, Tools/About, Experimental, multiple paths, side-effect-free close'
} finally {
    if ($duplicate) { if (-not $duplicate.HasExited) { $duplicate.Kill(); $duplicate.WaitForExit() }; $duplicate.Dispose() }
    if ($process) { if (-not $process.HasExited) { [void]$process.CloseMainWindow(); if (-not $process.WaitForExit(3000)) { $process.Kill(); $process.WaitForExit() } }; $process.Dispose() }
    if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Recurse -Force }
}
$global:LASTEXITCODE = 0
