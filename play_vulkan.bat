@echo off
rem Play Blinx 2 with the Vulkan renderer: the graphics card draws the game
rem instead of the CPU (in the rain of stage 01-03 it holds 30 fps where the
rem software renderer drops to about 23). Everything else is exactly a
rem double-click on build\Release\blinx_2_recomp.exe: the same window, sound,
rem pad and keyboard, and your saves.
rem
rem The Vulkan renderer is new. If something looks wrong, a plain double-click
rem plays with the usual software renderer, the reference. If this PC has no
rem usable Vulkan driver, the game notes it in blinx2_recomp.log and uses the
rem software renderer by itself.
rem
rem (RECOMP_VK_PRESENT=1, presenting through a Vulkan swapchain instead of the
rem usual window drawing, is left off: in tests the window stayed white.)
setlocal
set "RECOMP_GPU=vulkan"
start "" "%~dp0build\Release\blinx_2_recomp.exe"
endlocal
