# Called by record_blinx2.bat: snapshot the starting state, start the game with
# schedule and input recording on, and watch the launch so a stuck one is
# impossible to miss. See record_blinx2.bat.
param([string]$ExeDir, [string]$GameDir, [string]$CacheDir, [int]$WindowSecs = 120)

$ErrorActionPreference = 'Stop'
$exe = Join-Path $ExeDir 'blinx_2_recomp.exe'

function Fail([string]$msg) {
    Write-Host ''
    Write-Host "NOT RECORDING: $msg" -ForegroundColor Red
    exit 1
}

if (-not (Test-Path $exe)) { Fail "blinx_2_recomp.exe not found in $ExeDir" }

# A build that records a replayable schedule prints this (xboxrecomp sched.c);
# an older exe would run, record something no replay can follow, or hang.
Write-Host 'Checking the game build ...'
$bytes = [IO.File]::ReadAllBytes($exe)
$text = [Text.Encoding]::GetEncoding(28591).GetString($bytes)
if ($text.IndexOf('guest schedule (format %u)') -lt 0) {
    Fail ("this blinx_2_recomp.exe is too old to make exact recordings. " +
          "Rebuild build\Release from the current xboxrecomp, or play by " +
          "double-clicking the exe as usual.")
}
$bytes = $null; $text = $null

# Two instances share the saves and the Z: cache: the second hits a sharing
# violation on every cache file and sits for many minutes without a window.
$running = Get-CimInstance Win32_Process -Filter "Name='blinx_2_recomp.exe'" |
    Where-Object { $_.ExecutablePath -and
                   ($_.ExecutablePath -ieq (Resolve-Path $exe).Path) }
if ($running) {
    Fail ("the game is already running from this folder (PID " +
          (($running | ForEach-Object { $_.ProcessId }) -join ', ') +
          "). Close it first.")
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$rec = Join-Path $ExeDir "recordings\$stamp"
New-Item -ItemType Directory -Force -Path (Join-Path $rec 'save') | Out-Null

Write-Host "Saving the starting state to $rec\save ..."
# The partition images are large and never change; the replay recreates them.
$rc = '/E', '/XF', 'Partition*.img', '/NFL', '/NDL', '/NJH', '/NJS', '/NP'
if ($env:RECOMP_SAVE_DIR) {
    robocopy $env:RECOMP_SAVE_DIR "$rec\save" @rc | Out-Null
} else {
    robocopy $CacheDir "$rec\save" @rc | Out-Null
    robocopy "$GameDir\UDATA" "$rec\save\UDATA" @rc | Out-Null
    robocopy "$GameDir\TDATA" "$rec\save\TDATA" @rc | Out-Null
}

$env:RECOMP_SCHED_RECORD = "$rec\schedule.sched"
$env:RECOMP_INPUT_RECORD = "$rec\input.rec"
$log = "$rec\game.log"

Write-Host "Recording this run to $rec"
Write-Host "Game log: $log"
Write-Host 'Starting the game ...'
# The log goes to a file, as on a double-click: written to this console it
# was what made the boot take minutes before the window appeared.
$p = Start-Process -FilePath $exe -WorkingDirectory $ExeDir -PassThru `
                   -RedirectStandardError $log -NoNewWindow
$null = $p.Handle          # keeps the exit code readable after the exit
$t0 = Get-Date
$window = $false
$warned = $false
while (-not $p.HasExited) {
    Start-Sleep -Milliseconds 500
    $p.Refresh()
    $secs = [int]((Get-Date) - $t0).TotalSeconds
    if (-not $window -and $p.MainWindowHandle -ne [IntPtr]::Zero) {
        $window = $true
        Write-Host "Game window open after $secs s. Play; close the window to finish." -ForegroundColor Green
    }
    if (-not $window -and -not $warned -and $secs -ge $WindowSecs) {
        $warned = $true
        Write-Host ''
        Write-Host "WARNING: no game window after $secs s -- the launch looks stuck." -ForegroundColor Yellow
        Write-Host "  The game (PID $($p.Id)) is still running in the background."
        Write-Host "  Its log: $log"
        Write-Host '  Press K to stop it, or keep waiting.'
        Write-Host ''
    }
    $key = $false
    if ($warned -and -not $window) {
        try { $key = [Console]::KeyAvailable } catch { }   # no console input
    }
    if ($key) {
        if ([Console]::ReadKey($true).Key -eq 'K') {
            Stop-Process -Id $p.Id -Force
            Write-Host "Stopped. The partial recording is in $rec; its log tells where the launch stuck."
            exit 2
        }
    }
}
Write-Host ''
Write-Host "Game exited (code $($p.ExitCode))."
Write-Host "Recording saved in $rec"
Write-Host "Replay it with:  replay_blinx2.bat `"$rec`""
