# Dev helpers for this repo. Load once per PowerShell session with:  . E:\Project\vcv-rack-plugins\scripts\dev.ps1
# Runs everything in the MSYS2 environment, so PowerShell needs no PATH changes and plugin builds
# get RACK_DIR while the Rack source build never does (a set RACK_DIR breaks it).

$script:VcvRoot = "/e/Project/vcv-rack-plugins"
$script:VcvSdk  = "$script:VcvRoot/Rack-SDK-2.6.6-win-x64/Rack-SDK"

function Invoke-Msys {
    param([Parameter(Mandatory)][string]$Cmd)
    $env:MSYSTEM = "UCRT64"
    & "C:\msys64\usr\bin\bash.exe" -lc "export PATH=/ucrt64/bin:/usr/bin:`$PATH; unset RACK_DIR; $Cmd"
}

# Plugin (RACK_DIR = SDK)
function wa-build   { Invoke-Msys "cd $script:VcvRoot/waveArtifact && RACK_DIR=$script:VcvSdk make -j4 $($args -join ' ')" }
function wa-install { Invoke-Msys "cd $script:VcvRoot/waveArtifact && RACK_DIR=$script:VcvSdk make install" }
function wa-dist    { Invoke-Msys "cd $script:VcvRoot/waveArtifact && RACK_DIR=$script:VcvSdk make dist" }
function wa-clean   { Invoke-Msys "cd $script:VcvRoot/waveArtifact && RACK_DIR=$script:VcvSdk make clean" }
function wa-test    { Invoke-Msys "cd $script:VcvRoot/waveArtifact/tests && make run" }

# Rack source build (no RACK_DIR)
function rack-build { Invoke-Msys "cd $script:VcvRoot/Rack && make -j4" }
function rack-run   { Invoke-Msys "cd $script:VcvRoot/Rack && make run" }

# Any other command in the MSYS2 environment, e.g.  msys "ls /e/Project"
function msys       { Invoke-Msys ($args -join ' ') }

# Interactive MinGW 64-bit shell
function mingw      { & "C:\msys64\mingw64.exe" -here }

Write-Host "vcv dev helpers loaded: wa-build wa-install wa-dist wa-clean wa-test rack-build rack-run msys mingw"
