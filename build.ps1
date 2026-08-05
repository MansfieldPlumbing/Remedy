[CmdletBinding()]
param(
    [switch]$Clean,
    [switch]$Test
)

$ErrorActionPreference = "Stop"

# Derive repository root dynamically from script location
$RepoRoot = $PSScriptRoot
$BuildDir = Join-Path $RepoRoot "build"

# Process-local toolchain discovery:
# 1. Prefer tools already in process PATH.
# 2. Add S:\bin fallback paths if directories exist and tools are not yet in PATH.
$fallbackPaths = @(
    "C:\bin\cmake\bin",
    "S:\bin\msvc\bin\Hostx64\x64",
    "S:\bin\msvc",
    "S:\bin\Windows Kits\10\bin\10.0.26100.0\x64\ucrt",
    "S:\bin\Windows Kits\10\bin\10.0.26100.0\x64",
    "S:\bin\platform-tools",
    "S:\bin\nuget",
    "S:\bin\dotnet"
)

$currentPaths = $env:PATH -split ';' | Where-Object { $_ -ne "" }
foreach ($p in $fallbackPaths) {
    if ((Test-Path $p) -and ($currentPaths -notcontains $p)) {
        $env:PATH = "$p;$env:PATH"
    }
}

$repoInclude = Join-Path $RepoRoot "include"
$repoSrc = Join-Path $RepoRoot "src"
$repoNativeCore = Join-Path $RepoRoot "src\native\core"

$env:INCLUDE = "$repoInclude;$repoSrc;$repoNativeCore;S:\bin\msvc\include;S:\bin\Windows Kits\10\Include\10.0.26100.0\ucrt;S:\bin\Windows Kits\10\Include\10.0.26100.0\um;S:\bin\Windows Kits\10\Include\10.0.26100.0\shared;S:\bin\Windows Kits\10\Include\10.0.26100.0\winrt"
$env:LIB = "S:\bin\msvc\lib\x64;S:\bin\Windows Kits\10\Lib\10.0.26100.0\ucrt\x64;S:\bin\Windows Kits\10\Lib\10.0.26100.0\um\x64"

# Verify required toolchain binaries exist in process PATH
foreach ($tool in @("cmake", "ctest", "cl")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        Write-Error "[build.ps1] Required tool '$tool' could not be located in PATH."
        exit 1
    }
}

if ($Clean) {
    Write-Host "[build.ps1] Cleaning build directory: $BuildDir"
    if (Test-Path $BuildDir) {
        Remove-Item -Recurse -Force $BuildDir
    }
}

if (-not (Test-Path $BuildDir)) {
    New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
}

Write-Host "[build.ps1] Configuring CMake..."
& cmake.exe -S $RepoRoot -B $BuildDir
if ($LASTEXITCODE -ne 0) {
    Write-Error "[build.ps1] CMake configuration failed with exit code $LASTEXITCODE"
    exit $LASTEXITCODE
}

Write-Host "[build.ps1] Building CMake targets..."
& cmake.exe --build $BuildDir --config Debug
if ($LASTEXITCODE -ne 0) {
    Write-Error "[build.ps1] CMake build failed with exit code $LASTEXITCODE"
    exit $LASTEXITCODE
}

if ($Test) {
    Write-Host "[build.ps1] Running CTest..."
    & ctest.exe --test-dir $BuildDir --output-on-failure -C Debug
    if ($LASTEXITCODE -ne 0) {
        Write-Error "[build.ps1] CTest failed with exit code $LASTEXITCODE"
        exit $LASTEXITCODE
    }
}

Write-Host "[build.ps1] Build driver completed successfully."