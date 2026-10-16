# Package the Windows build for someone who has their own copy of the game:
# the exe, the launchers, the README, a settings file to point at the game
# files, the licences and (by default) the Visual C++ runtime DLLs. No game
# data, no saves, no logs: the game files are the player's own (see the
# README, "Extracting the game files").
#
#   powershell -ExecutionPolicy Bypass -File tools\package_windows.ps1
#       [-BuildDir build\Release] [-OutDir dist] [-WithPdb] [-NoVcRuntime]
#
# Writes <OutDir>\blinx2-recomp-windows-<commit>.zip and leaves the unpacked
# folder next to it. The launchers in the package find the exe next to
# themselves (in the source tree they look in build\Release).
param(
    [string]$BuildDir = (Join-Path $PSScriptRoot '..\build\Release'),
    [string]$OutDir   = (Join-Path $PSScriptRoot '..\dist'),
    [switch]$WithPdb,
    [switch]$NoVcRuntime
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$BuildDir = (Resolve-Path $BuildDir).Path
$exe = Join-Path $BuildDir 'blinx_2_recomp.exe'
if (-not (Test-Path $exe)) { throw "blinx_2_recomp.exe not found in $BuildDir; build it first (README.md, Building)" }

# The commits the exe was built from (cmake/git_sha.cmake writes them on
# every build), for the package name and the notes.
$shaFile = Join-Path (Split-Path $BuildDir) 'build_sha\build_sha.h'
$blinx = 'unknown'; $xbr = 'unknown'
if (Test-Path $shaFile) {
    $h = [IO.File]::ReadAllText($shaFile)
    if ($h -match 'BLINX2_GIT_SHA "([^"]+)"') { $blinx = $Matches[1] }
    if ($h -match 'XBOXRECOMP_GIT_SHA "([^"]+)"') { $xbr = $Matches[1] }
}
if ($blinx -like '*dirty*' -or $xbr -like '*dirty*') {
    Write-Warning "the exe was built from uncommitted sources ($blinx / $xbr)"
}
$name = "blinx2-recomp-windows-$($blinx -replace '[^0-9A-Za-z.+-]', '')"
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$stage = Join-Path $OutDir $name
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory $stage | Out-Null

Copy-Item $exe $stage
if ($WithPdb -and (Test-Path (Join-Path $BuildDir 'blinx_2_recomp.pdb'))) {
    Copy-Item (Join-Path $BuildDir 'blinx_2_recomp.pdb') $stage   # named crash stacks
}

# Launchers, pointed at the exe next to them. Byte-for-byte copies apart from
# that path (read and written as raw text, no re-encoding).
foreach ($f in 'play_software.bat', 'record_blinx2.bat', 'record_blinx2.ps1', 'replay_blinx2.bat') {
    $src = Join-Path $repo $f
    if (-not (Test-Path $src)) { continue }
    $t = [IO.File]::ReadAllText($src)
    $t = $t.Replace('%~dp0build\Release\', '%~dp0').Replace('%~dp0build\Release"', '%~dp0."')
    [IO.File]::WriteAllText((Join-Path $stage $f), $t, (New-Object Text.UTF8Encoding $false))
}
Copy-Item (Join-Path $repo 'README.md') $stage

# blinx2_settings.ini: the game reads it next to the exe and writes the
# menu's settings back into it. game_dir says where the game files are.
$ini = @(
    '; Blinx 2 (recomp) settings. The in-game menu (Escape) writes the rest.',
    '; game_dir: the folder holding default.xbe and the .ipk/.iph files, e.g.',
    ';   game_dir=D:\Games\Blinx2\game_files',
    '; Leave it empty to use a game_files folder next to blinx_2_recomp.exe.',
    '[paths]',
    'game_dir='
) -join "`r`n"
[IO.File]::WriteAllText((Join-Path $stage 'blinx2_settings.ini'), $ini + "`r`n", (New-Object Text.UTF8Encoding $false))

# Licences: the toolkit's and the third-party ones it builds in.
$lic = Join-Path $stage 'licenses'
New-Item -ItemType Directory $lic | Out-Null
$xr = Join-Path (Split-Path $repo) 'xboxrecomp'
if (Test-Path (Join-Path $xr 'LICENSE')) { Copy-Item (Join-Path $xr 'LICENSE') (Join-Path $lic 'xboxrecomp-LICENSE.txt') }
if (Test-Path (Join-Path $xr 'LICENSES')) { Copy-Item -Recurse (Join-Path $xr 'LICENSES\*') $lic }

# The Visual C++ runtime the exe imports (MSVCP140, VCRUNTIME140, _1),
# app-local from Visual Studio's redistributable folder, so the package runs
# without installing the redistributable.
$vcNote = 'bundled'
if (-not $NoVcRuntime) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $crt = $null
    if (Test-Path $vswhere) {
        $vs = & $vswhere -latest -property installationPath
        if ($vs) {
            $crt = Get-ChildItem (Join-Path $vs 'VC\Redist\MSVC') -Directory -ErrorAction SilentlyContinue |
                Sort-Object Name -Descending | ForEach-Object {
                    Get-ChildItem (Join-Path $_.FullName 'x64') -Directory -Filter 'Microsoft.VC14*.CRT' -ErrorAction SilentlyContinue
                } | Select-Object -First 1
        }
    }
    if ($crt) {
        foreach ($d in 'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') {
            Copy-Item (Join-Path $crt.FullName $d) $stage
        }
    } else {
        Write-Warning 'Visual C++ runtime DLLs not found; the package needs the VC++ 2015-2022 x64 redistributable installed'
        $vcNote = 'not bundled: install the Microsoft Visual C++ 2015-2022 Redistributable (x64)'
    }
} else {
    $vcNote = 'not bundled: install the Microsoft Visual C++ 2015-2022 Redistributable (x64)'
}

$quick = @(
    "Blinx 2 (recomp) for Windows -- blinx2-recomp $blinx, xboxrecomp $xbr",
    '',
    'You need your own copy of the game: the files extracted from the disc',
    '(default.xbe and the .ipk/.iph archives). README.md, "Extracting the game',
    'files", says how. None are included here.',
    '',
    '1. Put them in a folder named game_files next to blinx_2_recomp.exe, or',
    '   write their folder into blinx2_settings.ini: game_dir=<folder>.',
    '2. Double-click blinx_2_recomp.exe (Vulkan renderer if the graphics driver',
    '   has it, else software), or play_software.bat (software renderer).',
    '   Keys and pads: README.md, "Controls". Escape opens the menu.',
    '',
    'Saves go in <game files folder>\UDATA. Logs (blinx2_recomp.log) and',
    'recordings go next to the exe.',
    '',
    'Requirements: 64-bit Windows 10 or 11; a CPU with several cores (the',
    'software renderer needs them for 30 fps); a GPU driver with Vulkan 1.2',
    "for the Vulkan renderer (optional). Visual C++ runtime: $vcNote.",
    'Media Foundation (part of Windows; Windows "N" editions need the Media',
    'Feature Pack).'
) -join "`r`n"
[IO.File]::WriteAllText((Join-Path $stage 'QUICKSTART.txt'), $quick + "`r`n", (New-Object Text.UTF8Encoding $false))

# Nothing of the game may be in it.
$bad = Get-ChildItem -Recurse $stage | Where-Object {
    $_.Extension -in '.xbe', '.ipk', '.iph', '.sfd', '.adx', '.xbx', '.rec', '.sched', '.log' -or
    $_.Name -in 'UDATA', 'TDATA', 'game_files', 'recordings'
}
if ($bad) { throw "game data or run output in the package: $($bad.FullName -join ', ')" }

$zip = "$stage.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
Write-Host "Package: $zip ($([math]::Round((Get-Item $zip).Length / 1MB)) MB)"
Get-ChildItem $stage | ForEach-Object { Write-Host "  $($_.Name)" }
