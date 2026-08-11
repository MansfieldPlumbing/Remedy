[CmdletBinding()]
param(
    [string]$DeviceSerial = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = $PSScriptRoot
$ndkRoot = "S:\bin\android-sdk\ndk\27.3.13750724"
$cmake = "S:\bin\cmake\bin\cmake.exe"
$ninja = "S:\bin\ninja\ninja.exe"
$adb = "S:\bin\platform-tools\adb.exe"
$buildDir = Join-Path $repoRoot "build-android-arm64"

foreach ($required in @($cmake, $ninja, $adb, (Join-Path $ndkRoot "build\cmake\android.toolchain.cmake"))) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Required tool not found: $required" }
}

& $cmake -S $repoRoot -B $buildDir -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$ninja" `
    "-DCMAKE_TOOLCHAIN_FILE=$ndkRoot\build\cmake\android.toolchain.cmake" `
    -DANDROID_ABI=arm64-v8a `
    -DANDROID_PLATFORM=android-35 `
    -DANDROID_STL=c++_static `
    -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $cmake --build $buildDir --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$adbArgs = @()
if (-not [string]::IsNullOrWhiteSpace($DeviceSerial)) { $adbArgs += @("-s", $DeviceSerial) }
$remoteRoot = "/data/local/tmp/remedy-receipt-a"
& $adb @adbArgs shell "mkdir -p $remoteRoot"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $adb @adbArgs push (Join-Path $buildDir "remedy_inherited_channel_worker") "$remoteRoot/worker"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $adb @adbArgs push (Join-Path $buildDir "test_inherited_worker_channel_android") "$remoteRoot/receipt"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $adb @adbArgs shell "chmod 700 $remoteRoot/worker $remoteRoot/receipt && $remoteRoot/receipt $remoteRoot/worker"
exit $LASTEXITCODE
