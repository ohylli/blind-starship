# Launch Starship without stealing focus, so a test run does not interrupt the developer's screen
# reader mid-sentence. Intended for both manual use and AI coding agents driving the game.
#
# Keep this file ASCII-only. Windows PowerShell 5.1 reads a BOM-less .ps1 as Windows-1252, where a
# UTF-8 em dash decodes into a stray smart quote - and 5.1 accepts smart quotes as string delimiters,
# so one dash in a comment can unbalance the whole script.
#
# The mechanism is one Win32 flag: the process is created with STARTF_USESHOWWINDOW and
# SW_SHOWMINNOACTIVE ("shown, but never activated"). Start-Process cannot express this - its
# -WindowStyle Minimized maps to SW_SHOWMINIMIZED, which activates. Windows' foreground lock does not
# help either: it deliberately grants foreground rights to a process started by the foreground
# process, which is exactly this case, so ForegroundLockTimeout is irrelevant however it is set.
#
# The show state covers the debug console too (src/port/Engine.cpp calls AllocConsole() on every
# _WIN32 build, not just debug ones) - measured to be the window that actually grabbed the foreground
# when only the game window was handled.
#
# SDL2's SDL_WINDOW_NO_ACTIVATION_WHEN_SHOWN hint was tried first and did NOT prevent the steal on its
# own; the show state does the whole job, so the hint is not set here.
#
# Measured end state: both windows come up minimized and stay that way - unlike Unity, SDL does not
# restore the window during startup. Nothing appears on screen until you alt-tab to the game. A
# screenshot would therefore need the window restored first, which no current workflow does.
#
# ImGui's multi-viewport feature ("Allow multi-windows", the gEnableMultiViewports CVar, on by
# default) promotes libultraship's notification overlay into a real OS window titled "GameOverlay".
# That window ignores the startup show state and comes up visible, where a screen reader will see it.
# Turn the setting off under F1 if it bothers you; it only costs dragging F1 menu panels outside the
# game window.
#
# Not handled: which virtual desktop the game lands on. Windows creates every window on whichever
# desktop is ACTIVE at that moment, not one tied to the launching process, so the game appears on top
# of whatever you are doing (minimized and silent, but present in that desktop's alt-tab list).
# Moving it afterward is not cleanly possible: the public API, IVirtualDesktopManager::
# MoveWindowToDesktop, refuses to move a window the calling process does not own. Tools that do it
# anyway (VirtualDesktopAccessor and friends) drive an undocumented COM interface whose identifiers
# change between Windows builds. Revisit only if the alt-tab clutter starts to matter.

param(
    # Which build to run. Defaults to the debug build; -Config Release for the other one.
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Debug',

    # Launch normally instead, taking focus like a double-click would. For reproducing something that
    # only happens when the game starts focused.
    [switch]$Focus,

    # How long to wait for the game to come up before reporting what it owns. With the
    # debug server enabled (gDebugServer.Enabled CVar) this is an upper bound - the wait
    # ends as soon as the server answers a health command. With it off, the full wait is
    # spent as a plain settle delay, like before.
    [int]$SettleSeconds = 8,

    # Debug server port to poll for the readiness handshake (gDebugServer.Port CVar).
    [int]$Port = 7764,

    # Warp straight into a level after the readiness handshake (name or id as the debug
    # server's warp command accepts, e.g. corneria, meteo, sector-x). Requires the debug
    # server on, and python for tools/debug_client.py. The warp waits server-side for the
    # boot sequence to finish, so no extra delay is needed here.
    [string]$Level,

    # Warp to a named checkpoint from tools/checkpoints.json (see debug_client.py
    # checkpoint-save). The checkpoint stores its level, so -Level is not needed with it.
    [string]$Checkpoint,

    # With -Level/-Checkpoint: skip the level intro cutscene / arrive debug-paused at the
    # first play frame.
    [switch]$NoIntro,
    [switch]$Paused
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $repo "build\x64\$Config\Starship.exe"
if (-not (Test-Path $exe)) {
    Write-Host "No executable at:" -ForegroundColor Red
    Write-Host "  $exe"
    Write-Host "Build it first: cmake --build build/x64 --config $Config" -ForegroundColor Yellow
    exit 1
}
$dir = Split-Path $exe

# Two instances share one starship.cfg.json and one log file, and the second one's settings win on
# exit. Refuse rather than quietly corrupt the config.
$running = Get-Process -Name 'Starship' -ErrorAction SilentlyContinue
if ($running) {
    $pids = $running.Id -join ', '
    Write-Host "Starship is already running (pid $pids)." -ForegroundColor Yellow
    Write-Host "Close it first - instances share starship.cfg.json and the log." -ForegroundColor Yellow
    exit 1
}

Add-Type -Namespace Starship -Name Launch -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)]
public struct PROCESS_INFORMATION { public IntPtr hProcess; public IntPtr hThread; public uint dwProcessId; public uint dwThreadId; }

[StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
public struct STARTUPINFO {
  public int cb; public string lpReserved; public string lpDesktop; public string lpTitle;
  public int dwX; public int dwY; public int dwXSize; public int dwYSize;
  public int dwXCountChars; public int dwYCountChars; public int dwFillAttribute;
  public int dwFlags; public short wShowWindow; public short cbReserved2;
  public IntPtr lpReserved2; public IntPtr hStdInput; public IntPtr hStdOutput; public IntPtr hStdError;
}

[DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
public static extern bool CreateProcess(string lpApplicationName, string lpCommandLine,
  IntPtr lpProcessAttributes, IntPtr lpThreadAttributes, bool bInheritHandles, uint dwCreationFlags,
  IntPtr lpEnvironment, string lpCurrentDirectory, ref STARTUPINFO si, out PROCESS_INFORMATION pi);

[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr hWnd, System.Text.StringBuilder s, int n);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr hWnd, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
[DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hWnd);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lParam);
public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
'@

if ($Focus) {
    Write-Host "Launching the $Config build normally (this will steal focus) ..."
    $script:targetPid = (Start-Process -FilePath $exe -WorkingDirectory $dir -PassThru).Id
} else {
    Write-Host "Launching the $Config build minimized, without taking focus ..."
    $si = New-Object Starship.Launch+STARTUPINFO
    $si.cb = [System.Runtime.InteropServices.Marshal]::SizeOf($si)
    $si.dwFlags = 0x00000001      # STARTF_USESHOWWINDOW
    $si.wShowWindow = 7           # SW_SHOWMINNOACTIVE
    $pi = New-Object Starship.Launch+PROCESS_INFORMATION
    $ok = [Starship.Launch]::CreateProcess($exe, $null, [IntPtr]::Zero, [IntPtr]::Zero, $false, 0,
                                           [IntPtr]::Zero, $dir, [ref]$si, [ref]$pi)
    if (-not $ok) {
        $err = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
        Write-Host "CreateProcess failed (win32 error $err)." -ForegroundColor Red
        exit 1
    }
    $script:targetPid = $pi.dwProcessId
}

# Readiness handshake: send "health" to the debug server (tools/debug_client.py describes
# the protocol) and return the parsed inner payload, or $null if the server did not answer.
function Test-DebugServer {
    param([int]$Port)
    $client = New-Object System.Net.Sockets.TcpClient
    try {
        $async = $client.BeginConnect('127.0.0.1', $Port, $null, $null)
        if (-not $async.AsyncWaitHandle.WaitOne(500)) { return $null }
        $client.EndConnect($async)
        $stream = $client.GetStream()
        $stream.ReadTimeout = 2000
        $bytes = [System.Text.Encoding]::UTF8.GetBytes("health`n")
        $stream.Write($bytes, 0, $bytes.Length)
        $reader = New-Object System.IO.StreamReader($stream)
        $line = $reader.ReadLine()
        if (-not $line) { return $null }
        $resp = $line | ConvertFrom-Json
        if ($resp.status -ne 'ok') { return $null }
        return ($resp.output | ConvertFrom-Json)
    } catch {
        return $null
    } finally {
        $client.Close()
    }
}

Write-Host "pid $targetPid; waiting up to $SettleSeconds s for startup ..."
$deadline = (Get-Date).AddSeconds($SettleSeconds)
$health = $null
while ($true) {
    # Polling the process as well as the port turns "exited during startup" into an
    # immediate, specific failure instead of a silent timeout.
    if (-not (Get-Process -Id $targetPid -ErrorAction SilentlyContinue)) {
        Write-Host "Starship exited during startup. Check the log:" -ForegroundColor Red
        Write-Host "  $dir\logs\Starship.log"
        exit 1
    }
    $health = Test-DebugServer -Port $Port
    if ($health) { break }
    if ((Get-Date) -gt $deadline) { break }
    Start-Sleep -Milliseconds 250
}

if ($health) {
    Write-Host ("Debug server ready on port " + $Port + ": " + $health.gameStateName + ", frame " + $health.frame)
} else {
    Write-Host "Debug server did not answer on port $Port (gDebugServer.Enabled off?). Proceeding on the settle timeout alone." -ForegroundColor Yellow
}

# Optional warp-on-launch. debug_client.py resolves -Checkpoint names and blocks until the
# level is actually up (or the warp times out server-side), so when this returns with 0
# the game is sitting in the level.
if ($Level -or $Checkpoint) {
    if (-not $health) {
        Write-Host "Cannot warp: the debug server is not answering." -ForegroundColor Red
        exit 1
    }
    $warpArgs = @((Join-Path $repo 'tools\debug_client.py'), '--port', "$Port", 'warp')
    if ($Level) { $warpArgs += $Level }
    if ($Checkpoint) { $warpArgs += @('--checkpoint', $Checkpoint) }
    if ($NoIntro) { $warpArgs += '--no-intro' }
    if ($Paused) { $warpArgs += '--paused' }
    Write-Host ("Warping: " + ($warpArgs[3..($warpArgs.Count - 1)] -join ' ') + " ...")
    & python @warpArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Warp failed (exit code $LASTEXITCODE)." -ForegroundColor Red
        exit 1
    }
}

# Report what came up and who holds the foreground, so an unattended run can tell whether the launch
# behaved - the developer cannot see the screen, and a silent success looks like a silent failure.
$windows = New-Object System.Collections.ArrayList
$cb = [Starship.Launch+EnumWindowsProc] {
    param($hWnd, $lParam)
    $wpid = 0
    [void][Starship.Launch]::GetWindowThreadProcessId($hWnd, [ref]$wpid)
    if ($wpid -eq $script:targetPid -and [Starship.Launch]::IsWindowVisible($hWnd)) {
        $t = New-Object System.Text.StringBuilder 512
        [void][Starship.Launch]::GetWindowTextW($hWnd, $t, $t.Capacity)
        $c = New-Object System.Text.StringBuilder 256
        [void][Starship.Launch]::GetClassNameW($hWnd, $c, $c.Capacity)
        [void]$script:windows.Add([pscustomobject]@{
            Class     = $c.ToString()
            Title     = $t.ToString()
            Minimized = [Starship.Launch]::IsIconic($hWnd)
        })
    }
    return $true
}
[void][Starship.Launch]::EnumWindows($cb, [IntPtr]::Zero)

$fg = [Starship.Launch]::GetForegroundWindow()
$fgPid = 0
[void][Starship.Launch]::GetWindowThreadProcessId($fg, [ref]$fgPid)
$fgTitle = New-Object System.Text.StringBuilder 512
[void][Starship.Launch]::GetWindowTextW($fg, $fgTitle, $fgTitle.Capacity)
$fgName = (Get-Process -Id $fgPid -ErrorAction SilentlyContinue).ProcessName
$fgText = $fgTitle.ToString()

Write-Host ""
Write-Host "Windows owned by pid ${targetPid}:"
$windows | Format-Table Class, Title, Minimized -AutoSize | Out-String | Write-Host
Write-Host "Foreground: pid $fgPid ($fgName) '$fgText'"

if ($fgPid -eq $targetPid) {
    if ($Focus) {
        Write-Host "Starship has the foreground, as -Focus asked for." -ForegroundColor Green
    } else {
        Write-Host "Starship took the foreground - focus was stolen." -ForegroundColor Red
        exit 1
    }
} else {
    Write-Host "Starship is running without the foreground. Alt-tab to it when you want to play." -ForegroundColor Green
}
exit 0
