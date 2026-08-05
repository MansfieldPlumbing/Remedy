[CmdletBinding()]
param(
    [switch]$Clean,
    [switch]$Test
)

$ErrorActionPreference = "Stop"

# Configure environment paths for MSVC compiler, CMake, and Debug CRT DLLs
$env:PATH = "C:\bin\cmake\bin;S:\bin\vs\VC\Redist\MSVC\14.51.36223\debug_nonredist\x64\Microsoft.VC145.DebugCRT;S:\bin\Windows Kits\10\bin\10.0.26100.0\x64\ucrt;S:\bin\msvc\bin\Hostx64\x64;S:\bin\msvc;S:\bin\Windows Kits\10\bin\10.0.26100.0\x64;S:\bin\platform-tools;S:\bin\nuget;S:\bin\dotnet;$env:PATH"
$env:INCLUDE = "S:\Remedy\include;S:\Remedy\src;S:\Remedy\src\native\core;S:\bin\msvc\include;S:\bin\Windows Kits\10\Include\10.0.26100.0\ucrt;S:\bin\Windows Kits\10\Include\10.0.26100.0\um;S:\bin\Windows Kits\10\Include\10.0.26100.0\shared;S:\bin\Windows Kits\10\Include\10.0.26100.0\winrt"
$env:LIB = "S:\bin\msvc\lib\x64;S:\bin\Windows Kits\10\Lib\10.0.26100.0\ucrt\x64;S:\bin\Windows Kits\10\Lib\10.0.26100.0\um\x64"

$rootDir = "S:\Remedy"
$buildDir = "S:\Remedy\build"

if ($Clean) {
    Write-Host "[build.ps1] Cleaning build directory: $buildDir"
    if (Test-Path $buildDir) {
        Remove-Item -Recurse -Force $buildDir
    }
}

if (-not (Test-Path $buildDir)) {
    New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
}

Write-Host "[build.ps1] Configuring CMake..."
& cmake.exe -S $rootDir -B $buildDir
if ($LASTEXITCODE -ne 0) {
    Write-Error "[build.ps1] CMake configuration failed with exit code $LASTEXITCODE"
    exit $LASTEXITCODE
}

Write-Host "[build.ps1] Building CMake targets..."
& cmake.exe --build $buildDir --config Debug
if ($LASTEXITCODE -ne 0) {
    Write-Error "[build.ps1] CMake build failed with exit code $LASTEXITCODE"
    exit $LASTEXITCODE
}

if ($Test) {
    Write-Host "[build.ps1] Running CTest..."
    & ctest.exe --test-dir $buildDir --output-on-failure -C Debug
    if ($LASTEXITCODE -ne 0) {
        Write-Error "[build.ps1] CTest failed with exit code $LASTEXITCODE"
        exit $LASTEXITCODE
    }
}

Write-Host "[build.ps1] Build driver completed successfully."
