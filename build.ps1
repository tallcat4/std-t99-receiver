param([string]$Compiler = '', [switch]$SkipTests, [string]$Output = 'build/std-t99.exe')
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    if (!$Compiler) {
        $found = Get-Command g++ -ErrorAction SilentlyContinue
        if ($found) { $Compiler = $found.Source }
        elseif (Test-Path 'C:\Strawberry\c\bin\g++.exe') { $Compiler = 'C:\Strawberry\c\bin\g++.exe' }
        else { throw 'Install a C++20 compiler, or build with CMake and Visual Studio 2022.' }
    }
    New-Item -ItemType Directory -Force build | Out-Null
    & ./setup-fftw.ps1
    $flags = @('-std=c++20', '-O2', '-static', '-Ibuild/fftw', '-Isrc/include', '-Isrc', '-Ivendor/spear/apps/std_t98', '-Ivendor/spear/dsp/include')
    $sources = @('src/input.cpp', 'src/output.cpp', 'src/dashboard.cpp', 'src/protocol.cpp', 'src/messages.cpp',
        'src/receiver.cpp', 'src/session.cpp', 'vendor/spear/apps/std_t98/dsp/rrc.cpp',
        'vendor/spear/apps/std_t98/dsp/symbol_sync.cpp', 'src/frame_sync.cpp',
        'vendor/spear/dsp/src/fir.cpp', 'vendor/spear/dsp/src/fftw_planner.cpp')
    & $Compiler @flags src/main.cpp @sources build/fftw/libfftw3f-3.dll -lshell32 -o $Output
    if ($LASTEXITCODE) { throw 'Receiver compilation failed' }
    if (!$SkipTests) {
        & $Compiler @flags tests/native/test_main.cpp vendor/spear/dsp/src/channelizer.cpp @sources build/fftw/libfftw3f-3.dll -o build/t99-tests.exe
        if ($LASTEXITCODE) { throw 'Test compilation failed' }
        Push-Location build
        try { & ./t99-tests.exe; if ($LASTEXITCODE) { throw 'Native tests failed' } } finally { Pop-Location }
    }
    Write-Host "Ready: $Output"
} finally { Pop-Location }
