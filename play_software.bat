@echo off
rem Play Blinx 2 with the software renderer: the CPU draws the game, as it
rem did before Vulkan became the default (2026-10-14). It is the reference:
rem if something looks wrong with a plain double-click (Vulkan), see whether
rem it looks right here -- if it does, it is a Vulkan renderer bug. Everything
rem else is exactly a double-click on build\Release\blinx_2_recomp.exe: the
rem same window, sound, pad and keyboard, and your saves.
rem
rem The software renderer needs a fast multi-core CPU for 30 fps; busy scenes
rem such as stage 01-03's rain dip into the low 20s.
setlocal
set "RECOMP_GPU=software"
start "" "%~dp0build\Release\blinx_2_recomp.exe"
endlocal
