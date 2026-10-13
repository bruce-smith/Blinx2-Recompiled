@echo off
rem Play Blinx 2 as usual -- window, rendering, sound, pad and keyboard -- and
rem record the run so it replays exactly (replay_blinx2.bat): the input, the
rem order the game's threads and devices ran in, and everything the game read
rem from the host clock. Plain double-click play does not record any of this.
rem
rem Each run gets a folder build\Release\recordings\<date-time>\ holding
rem   save\            the saves, title data and Z: cache the run started from
rem   schedule.sched   the guest schedule (about 10-15 KB a second of play)
rem   input.rec        the pad input
rem Close the window to end the run; the recording is complete at that point.
setlocal
set "EXE_DIR=%~dp0build\Release"
rem Where the exe keeps saves when RECOMP_SAVE_DIR is not set (src\main.c):
rem UDATA/TDATA in the game folder, the Z: cache under %LOCALAPPDATA%.
set "GAME_DIR=E:\Blinx2project\game_files"
set "CACHE_DIR=%LOCALAPPDATA%\xboxrecomp"

if not exist "%EXE_DIR%\blinx_2_recomp.exe" (
    echo blinx_2_recomp.exe not found in %EXE_DIR%
    exit /b 1
)
for /f %%t in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set "STAMP=%%t"
set "REC=%EXE_DIR%\recordings\%STAMP%"
mkdir "%REC%\save" 2>nul

echo Saving the starting state to %REC%\save ...
rem The partition images are large and never change; the replay recreates them.
if defined RECOMP_SAVE_DIR (
    rem An explicit save dir holds everything, UDATA/TDATA included.
    robocopy "%RECOMP_SAVE_DIR%" "%REC%\save" /E /XF Partition*.img /NFL /NDL /NJH /NJS /NP >nul
) else (
    robocopy "%CACHE_DIR%" "%REC%\save" /E /XF Partition*.img /NFL /NDL /NJH /NJS /NP >nul
    robocopy "%GAME_DIR%\UDATA" "%REC%\save\UDATA" /E /NFL /NDL /NJH /NJS /NP >nul
    robocopy "%GAME_DIR%\TDATA" "%REC%\save\TDATA" /E /NFL /NDL /NJH /NJS /NP >nul
)

set "RECOMP_SCHED_RECORD=%REC%\schedule.sched"
set "RECOMP_INPUT_RECORD=%REC%\input.rec"

cd /d "%EXE_DIR%"
echo Recording this run to %REC%
"%EXE_DIR%\blinx_2_recomp.exe"
echo.
echo Recording saved in %REC%
echo Replay it with:  replay_blinx2.bat "%REC%"
endlocal
