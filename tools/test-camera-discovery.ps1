[CmdletBinding()]
param(
    [ValidateSet("x64", "arm64")]
    [string] $Architecture = "x64",
    [string] $MatterSdkRoot,
    [switch] $BuildOnly
)

$ErrorActionPreference = "Stop"
function Invoke-NativeRegression {
    param([string] $Executable)
    $process = Start-Process -FilePath $Executable -NoNewWindow -PassThru
    if (-not $process.WaitForExit(120000)) {
        Stop-Process -Id $process.Id
        throw "Native regression exceeded its two-minute deadline: $Executable"
    }
    if ($process.ExitCode -ne 0) {
        throw "Native regression failed with exit code $($process.ExitCode): $Executable"
    }
}

$winrtRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$sdkRoot = if ([string]::IsNullOrWhiteSpace($MatterSdkRoot)) {
    Join-Path $winrtRoot "matterforwindows"
} else {
    (Resolve-Path $MatterSdkRoot).Path
}
. (Join-Path $sdkRoot "scripts\setup\windows.ps1") -Architecture $Architecture
$output = Join-Path $winrtRoot "out\win-winrt-$Architecture"
$options = Join-Path $output "compile-options.rsp"
$library = Join-Path $sdkRoot "out\matter-winrt-sdk-$Architecture\obj\out\matter-winrt-sdk-target\matter-controller-sdk.lib"
if (-not (Test-Path $options) -or -not (Test-Path $library)) {
    throw "Build the matched SDK and WinRT component before running the camera discovery projection tests."
}
$object = Join-Path $output "CameraDiscoveryProjectionTests.obj"
$executable = Join-Path $output "CameraDiscoveryProjectionTests.exe"
& cl.exe "@$options" /c (Join-Path $winrtRoot "winrt\CameraDiscoveryProjectionTests.cpp") "/Fo$object"
if ($LASTEXITCODE -ne 0) {
    throw "Camera discovery projection test compilation failed."
}
$objects = @(Get-ChildItem (Join-Path $output "obj") -Filter "*.obj" -File | ForEach-Object { $_.FullName })
$machine = if ($Architecture -eq "arm64") { "ARM64" } else { "X64" }
& link.exe /nologo "/MACHINE:$machine" "/OUT:$executable" $object @objects $library `
    WindowsApp.lib ws2_32.lib iphlpapi.lib Advapi32.lib Ole32.lib Shell32.lib dnsapi.lib
if ($LASTEXITCODE -ne 0) {
    throw "Camera discovery projection test link failed."
}
if (-not $BuildOnly) {
    Invoke-NativeRegression $executable
}

$tlvObject = Join-Path $output "ProjectionTlvTests.obj"
$tlvExecutable = Join-Path $output "ProjectionTlvTests.exe"
& cl.exe "@$options" /DMATTER_WINRT_ADMISSION_TESTS /c (Join-Path $winrtRoot "winrt\ProjectionTlvTests.cpp") "/Fo$tlvObject"
if ($LASTEXITCODE -ne 0) {
    throw "Private TLV bridge test compilation failed."
}
# The test includes Projection.cpp to exercise private functions without exporting a test API.
$tlvObjects = @($objects | Where-Object { [System.IO.Path]::GetFileName($_) -ne "Projection.obj" })
& link.exe /nologo "/MACHINE:$machine" "/OUT:$tlvExecutable" $tlvObject @tlvObjects $library `
    WindowsApp.lib ws2_32.lib iphlpapi.lib Advapi32.lib Ole32.lib Shell32.lib dnsapi.lib
if ($LASTEXITCODE -ne 0) {
    throw "Private TLV bridge test link failed."
}
if (-not $BuildOnly) {
    Invoke-NativeRegression $tlvExecutable
}

$cancellationObject = Join-Path $output "ReadCancellationTests.obj"
$cancellationExecutable = Join-Path $output "ReadCancellationTests.exe"
& cl.exe "@$options" /c (Join-Path $winrtRoot "winrt\ReadCancellationTests.cpp") "/Fo$cancellationObject"
if ($LASTEXITCODE -ne 0) {
    throw "Public read cancellation test compilation failed."
}
& link.exe /nologo "/MACHINE:$machine" "/OUT:$cancellationExecutable" $cancellationObject @objects $library `
    WindowsApp.lib ws2_32.lib iphlpapi.lib Advapi32.lib Ole32.lib Shell32.lib dnsapi.lib
if ($LASTEXITCODE -ne 0) {
    throw "Public read cancellation test link failed."
}
if (-not $BuildOnly) {
    Invoke-NativeRegression $cancellationExecutable
}
