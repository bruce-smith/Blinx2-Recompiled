@echo off
rem Export Blinx 2's stages and characters to glTF 2.0 (.glb), for Blender,
rem Unity, Unreal, Godot and other games. Everything comes from your own copy
rem of the game; the files are for your own use -- do not redistribute them.
rem
rem   export_assets.bat [game_files] [extracted assets] [output folder]
rem
rem   game_files  the folder with default.xbe (and UDATA, your saves). Default:
rem               RECOMP_GAME_DIR, else ..\game_files next to this folder.
rem   assets      the .ipk archives extracted with blipks (the superproject's
rem               assets\extracted). Default: ..\assets\extracted. Without
rem               it the models keep their colours but get no textures.
rem   output      default: export\ in this folder.
rem
rem It writes
rem   stages\M11.glb ... M121.glb, G1..G5.glb
rem       every stage: textured geometry, collision (nodes named
rem       *_collision) and the game's object placements as marker nodes.
rem   characters\<save>\<team><slot>_<name>.glb
rem       your own characters from each save (team ts = Time Sweepers,
rem       tg = Tom-Tom gang): rigged (glTF skin) and with all their motions
rem       as animations.
rem   presets\<preset>.glb
rem       the character editor's 45 stock presets, rigged, no motions.
rem
rem Needs Python 3 (py -3) with numpy and Pillow. Takes about 10 minutes.
rem The tool and its options: tools\ninja\README.md.
setlocal enabledelayedexpansion
set "HERE=%~dp0"
set "GAME=%~1"
if "%GAME%"=="" set "GAME=%RECOMP_GAME_DIR%"
if "%GAME%"=="" set "GAME=%HERE%..\game_files"
set "ASSETS=%~2"
if "%ASSETS%"=="" set "ASSETS=%HERE%..\assets\extracted"
set "OUT=%~3"
if "%OUT%"=="" set "OUT=%HERE%export"
set "TOOL=%HERE%tools\ninja\ninja2gltf.py"
set "XBE=%GAME%\default.xbe"

if not exist "%XBE%" (
  echo No default.xbe in "%GAME%". Pass your game_files folder as the first argument.
  exit /b 1
)
set "TEX=--assets "%ASSETS%""
if not exist "%ASSETS%" (
  echo No extracted assets in "%ASSETS%": exporting without textures.
  set "TEX="
)
if not exist "%OUT%" mkdir "%OUT%"
set "LOG=%OUT%\export.log"
echo Exporting to "%OUT%" ^(log: "%LOG%"^)
echo. > "%LOG%"

echo Stages...
py -3 "%TOOL%" "%XBE%" stages "%OUT%\stages" %TEX% --collision --placements >> "%LOG%" 2>&1
if errorlevel 1 (echo Stage export failed, see "%LOG%". & exit /b 1)

echo Your characters...
for /d %%S in ("%GAME%\UDATA\4d530065\*") do (
  if exist "%%S\blinx2data.bin" (
    if not exist "%OUT%\characters\%%~nxS" mkdir "%OUT%\characters\%%~nxS"
    for %%T in (ts tg) do (
      for /l %%N in (0,1,7) do (
        py -3 "%TOOL%" "%XBE%" MDLPL "%OUT%\characters\%%~nxS\%%T%%N.glb" --skin --save "%%S" --team %%T --slot %%N --motions MOTPL,MOTP0 %TEX% > "%OUT%\one.log" 2>&1
        if not errorlevel 1 (
          for /f "tokens=3" %%W in ('findstr /b /c:"exporting" "%OUT%\one.log"') do (
            move /y "%OUT%\characters\%%~nxS\%%T%%N.glb" "%OUT%\characters\%%~nxS\%%T%%N_%%W.glb" > nul
            echo   %%~nxS %%T%%N %%W
          )
        )
        type "%OUT%\one.log" >> "%LOG%"
      )
    )
  )
)
del "%OUT%\one.log" 2> nul

echo Editor presets...
if not exist "%OUT%\presets" mkdir "%OUT%\presets"
for %%F in ("%ASSETS%\media\edit_char\*.edt") do (
  py -3 "%TOOL%" "%XBE%" MDLPL "%OUT%\presets\%%~nF.glb" --skin --edt "%%F" %TEX% >> "%LOG%" 2>&1
)

echo Done: "%OUT%"
endlocal
