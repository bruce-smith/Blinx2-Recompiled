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
rem   game.log         the game's log
rem Close the game window to end the run; the recording is complete then.
rem
rem It refuses to start with an exe too old to record exactly, or while the
rem game is already running from build\Release, and says so if no window has
rem appeared after two minutes. The work is in record_blinx2.ps1.
setlocal
rem Where the exe keeps saves when RECOMP_SAVE_DIR is not set (src\main.c):
rem UDATA/TDATA in the game folder, the Z: cache under %LOCALAPPDATA%.
rem The game folder is found as the exe finds it: RECOMP_GAME_DIR, else
rem game_files next to the exe, else the build's default (BLINX2_GAME_DIR in
rem CMakeLists.txt; change GAME_DIR below if you configured another one).
set "GAME_DIR=E:\Blinx2project\game_files"
if exist "%~dp0build\Release\game_files\default.xbe" set "GAME_DIR=%~dp0build\Release\game_files"
if defined RECOMP_GAME_DIR set "GAME_DIR=%RECOMP_GAME_DIR%"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0record_blinx2.ps1" ^
    -ExeDir "%~dp0build\Release" ^
    -GameDir "%GAME_DIR%" ^
    -CacheDir "%LOCALAPPDATA%\xboxrecomp"
echo.
pause
endlocal
