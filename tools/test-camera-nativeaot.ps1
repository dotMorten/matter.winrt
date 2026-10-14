[CmdletBinding()]
param(
    [string] $MatterSdkRoot,
    [string] $PackageVersion
)

$ErrorActionPreference = "Stop"
$winrtRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$sdkRoot = if ([string]::IsNullOrWhiteSpace($MatterSdkRoot)) {
    Join-Path $winrtRoot "matterforwindows"
} else {
    (Resolve-Path $MatterSdkRoot).Path
}
. (Join-Path $sdkRoot "scripts\setup\windows.ps1") -Architecture x64
$output = Join-Path $winrtRoot "out\win-winrt-x64"
$options = Join-Path $output "compile-options.rsp"
$library = Join-Path $sdkRoot "out\matter-winrt-sdk-x64\obj\out\matter-winrt-sdk-target\matter-controller-sdk.lib"
if (-not (Test-Path $options) -or -not (Test-Path $library)) {
    throw "Build the matching x64 SDK and WinRT component before running NativeAOT tests."
}
$object = Join-Path $output "CameraNativeAotFixture.obj"
$fixture = Join-Path $output "CameraNativeAotFixture.dll"
& cl.exe "@$options" /c (Join-Path $winrtRoot "winrt\CameraNativeAotFixture.cpp") "/Fo$object"
if ($LASTEXITCODE -ne 0) {
    throw "NativeAOT fixture compilation failed."
}
$objects = @(Get-ChildItem (Join-Path $output "obj") -Filter "*.obj" -File | ForEach-Object { $_.FullName })
& link.exe /nologo /DLL /MACHINE:X64 "/OUT:$fixture" $object @objects $library `
    WindowsApp.lib ws2_32.lib iphlpapi.lib Advapi32.lib Ole32.lib Shell32.lib dnsapi.lib
if ($LASTEXITCODE -ne 0) {
    throw "NativeAOT fixture link failed."
}
$publishName = if ([string]::IsNullOrWhiteSpace($PackageVersion)) { "camera-nativeaot" } else { "camera-nativeaot-$PackageVersion" }
$publish = Join-Path $winrtRoot "out\$publishName"
New-Item -ItemType Directory -Force (Join-Path $winrtRoot "artifacts") | Out-Null
& dotnet publish (Join-Path $winrtRoot "tests\CameraNativeAot\CameraNativeAot.csproj") `
    -c Release -o $publish "-p:MatterControllerPackageVersion=$PackageVersion" -p:PlatformTarget=x64 `
    "-p:BaseIntermediateOutputPath=$publish\obj\" --verbosity quiet
if ($LASTEXITCODE -ne 0) {
    throw "NativeAOT client publish failed."
}
& (Join-Path $publish "CameraNativeAot.exe")
if ($LASTEXITCODE -ne 0) {
    throw "NativeAOT camera projection tests failed."
}
