# Official FFTW single-precision x64 runtime and development files.
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    $expectedHash = 'CFD88DC0E8D7001115EA79E069A2C695D52C8947F5B4F3B7AC54A192756F439F'
    New-Item -ItemType Directory -Force build/fftw | Out-Null
    $zipPath = Join-Path $PSScriptRoot 'build/fftw/fftw-3.3.5-dll64.zip'
    if (!(Test-Path -LiteralPath $zipPath)) {
        Invoke-WebRequest 'https://www.fftw.org/pub/fftw/fftw-3.3.5-dll64.zip' -OutFile $zipPath
    }
    if ((Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash -ne $expectedHash) { throw 'FFTW SHA256 mismatch' }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($zipPath)
    try {
        foreach ($name in @('fftw3.h', 'libfftw3f-3.dll', 'libfftw3f-3.def', 'COPYING', 'COPYRIGHT', 'README-WINDOWS')) {
            $entry = $zip.GetEntry($name)
            if (!$entry) { throw "Missing FFTW file: $name" }
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, (Join-Path $PSScriptRoot "build/fftw/$name"), $true)
        }
    } finally { $zip.Dispose() }
    if (!(Test-Path build/libfftw3f-3.dll) -or
        (Get-FileHash build/libfftw3f-3.dll).Hash -ne (Get-FileHash build/fftw/libfftw3f-3.dll).Hash) {
        Copy-Item build/fftw/libfftw3f-3.dll build/libfftw3f-3.dll
    }
    Write-Host 'Ready: official FFTW x64 single-precision library in build/fftw'
} finally { Pop-Location }
