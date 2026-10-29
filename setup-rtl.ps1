# Download the official x64 user-space RTL runtime beside the receiver.
# Does not install or change USB drivers.
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    $expectedHash = '7EF33F1304647F65E5E0FDE43637A73D54F076E91E651A3CECC4F55A17FD9815'
    $url = 'https://github.com/rtlsdrblog/rtl-sdr-blog/releases/download/V1.4.0/Release.zip'
    New-Item -ItemType Directory -Force build/rtl-runtime | Out-Null
    $zipPath = Join-Path $PSScriptRoot 'build/rtl-runtime/Release.zip'
    if (!(Test-Path -LiteralPath $zipPath)) { Invoke-WebRequest $url -OutFile $zipPath }
    if ((Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash -ne $expectedHash) { throw 'RTL runtime SHA256 mismatch' }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($zipPath)
    try {
        foreach ($name in @('rtlsdr.dll', 'pthreadVC2.dll', 'msvcr100.dll')) {
            $entry = $zip.GetEntry("x64/$name")
            if (!$entry) { throw "Missing x64/$name" }
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, (Join-Path $PSScriptRoot "build/$name"), $true)
        }
    } finally { $zip.Dispose() }
    Write-Host 'Installed official RTL-SDR Blog V1.4.0 x64 DLLs in build. USB driver setup is separate.'
} finally { Pop-Location }
