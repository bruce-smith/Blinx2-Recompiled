@echo off
rem Replay a run recorded with record_blinx2.bat, exactly: the same game state
rem at every frame, on any machine speed.
rem
rem   replay_blinx2.bat <recording folder> [window]
rem
rem Headless by default (no window, sound muted, log to <folder>\replay.log);
rem "window" shows it. The run starts from a private copy of the recording's
rem saves (%TEMP%\blinx2_replay_save), so your own saves are never touched.
rem Extra switches for troubleshooting can be set before calling this, e.g.
rem   set RECOMP_INPUT_HASH=10,hash.bin   (guest-memory hashes every 10 polls)
setlocal
if "%~1"=="" (
    echo usage: replay_blinx2.bat ^<recording folder^> [window]
    exit /b 1
)
set "REC=%~f1"
set "EXE_DIR=%~dp0build\Release"
if not exist "%REC%\schedule.sched" (
    echo %REC% has no schedule.sched -- not a record_blinx2.bat recording
    exit /b 1
)
set "SAVE=%TEMP%\blinx2_replay_save"
robocopy "%REC%\save" "%SAVE%" /MIR /XF Partition*.img /NFL /NDL /NJH /NJS /NP >nul

set "RECOMP_SAVE_DIR=%SAVE%"
set "RECOMP_SCHED_REPLAY=%REC%\schedule.sched"
set "RECOMP_INPUT_REPLAY=%REC%\input.rec"
set "RECOMP_INPUT_RECORD=0"
rem The renderer the recording was made with: Vulkan when its game.log says
rem "[GPU] renderer: vulkan", else software. A run replays exactly only with
rem its own renderer, and recordings from before Vulkan became the default
rem (2026-10-14) were all made in software.
set "RECOMP_GPU=software"
if exist "%REC%\game.log" findstr /C:"[GPU] renderer: vulkan" "%REC%\game.log" >nul && set "RECOMP_GPU=vulkan"
echo Renderer: %RECOMP_GPU%

cd /d "%EXE_DIR%"
if /i "%~2"=="window" (
    echo Replaying %REC% in a window; log: %REC%\replay.log
    "%EXE_DIR%\blinx_2_recomp.exe" 2> "%REC%\replay.log"
    goto done
)
rem Headless, with the same devices the recording ran with: the pushbuffer
rem executor, USB and the APU (muted).
set "RECOMP_HEADLESS=1"
set "RECOMP_PB_EXEC=1"
set "RECOMP_USB=1"
set "RECOMP_AUDIO=1"
set "RECOMP_AUDIO_MUTE=1"
echo Replaying %REC% headless; log: %REC%\replay.log
"%EXE_DIR%\blinx_2_recomp.exe" 2> "%REC%\replay.log"
:done
rem Exit code 4: the run left the recording's path and the replay stopped
rem itself (the "[REPLAY] diverged" line in the log says where).
if errorlevel 4 if not errorlevel 5 (
    echo Replay STOPPED: it diverged from the recording. See %REC%\replay.log
    endlocal
    exit /b 4
)
echo Replay finished.
endlocal
