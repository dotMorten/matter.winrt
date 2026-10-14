[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $OutputDirectory,
    [string] $PackageVersion = "0.2.0-camera.22"
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$output = [System.IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path (Join-Path $output "result.json")) {
    throw "Use a fresh output directory so a previous result cannot satisfy this run."
}
New-Item -ItemType Directory -Force $output | Out-Null
$binaryDirectory = Join-Path $output "bin"
& dotnet build (Join-Path $root "samples\ControllerApp\MatterControllerApp.csproj") `
    -p:Platform=x64 -p:Unpackaged=true -p:MatterCameraMediaTest=true `
    "-p:MatterControllerPackageVersion=$PackageVersion" "-p:OutputPath=$binaryDirectory\" `
    --nologo --verbosity quiet
if ($LASTEXITCODE -ne 0) {
    throw "Synthetic WebView2 test build failed."
}
$executable = Join-Path $binaryDirectory "MatterControllerApp.exe"
$resultPath = Join-Path $output "result.json"
$process = Start-Process -FilePath $executable -ArgumentList @("--camera-media-selftest", "`"$resultPath`"") -PassThru
if (-not $process.WaitForExit(120000)) {
    Stop-Process -Id $process.Id
    throw "Synthetic WebView2 media test exceeded its two-minute deadline."
}
if (-not (Test-Path $resultPath)) {
    throw "Synthetic WebView2 test exited without a result (exit code $($process.ExitCode))."
}
$result = Get-Content $resultPath -Raw | ConvertFrom-Json
if ($process.ExitCode -ne 0 -or -not $result.succeeded -or $result.permissionRequests -ne 0) {
    throw "Synthetic WebView2 media test failed: $($result.message)"
}
$result | ConvertTo-Json -Depth 8
