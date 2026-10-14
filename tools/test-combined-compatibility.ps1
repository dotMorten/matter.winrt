[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $BaselinePackage,
    [Parameter(Mandatory)][string] $CombinedPackage,
    [Parameter(Mandatory)][string] $OutputDirectory,
    [Parameter(Mandatory)][string] $MatterSdkRoot,
    [switch] $Run
)
$ErrorActionPreference = "Stop"
$output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
if (Test-Path $output) { throw "Use a fresh acceptance directory." }
New-Item -ItemType Directory $output | Out-Null
$packages = @{}
foreach ($entry in @(@("baseline", $BaselinePackage), @("combined", $CombinedPackage))) {
    $name = $entry[0]
    $package = (Resolve-Path $entry[1]).Path
    $destination = Join-Path $output $name
    [System.IO.Compression.ZipFile]::ExtractToDirectory($package, $destination)
    $nuspec = @(Get-ChildItem $destination -Filter "*.nuspec" -File)
    if ($nuspec.Count -ne 1) { throw "Expected exactly one package manifest." }
    [xml] $metadata = Get-Content $nuspec[0].FullName -Raw
    if ($metadata.package.metadata.id -ne "Matter.Windows.Controller") { throw "Unexpected baseline/package identity." }
    $packages[$name] = @{
        directory = $destination
        version = [string]$metadata.package.metadata.version
        sha256 = (Get-FileHash $package).Hash
        source = Split-Path $package -Parent
    }
}
if ($packages.baseline.version -ne "0.1.0-preview.14") {
    throw "Release acceptance requires the actual published preview.14 package, not a reconstructed or older contract."
}
$packages | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $output "packages.json") -Encoding utf8
. (Join-Path $MatterSdkRoot "scripts\setup\windows.ps1") -Architecture x64
$combined = $packages.combined.directory
& (Join-Path $PSScriptRoot "verify-abi.ps1") `
    -BaselineWinmd (Join-Path $packages.baseline.directory "winmd\Matter.Windows.Controller.winmd") `
    -CurrentWinmd (Join-Path $combined "winmd\Matter.Windows.Controller.winmd") `
    -NativeDll @((Join-Path $combined "runtimes\win-x64\native\Matter.Windows.Controller.dll"),
                (Join-Path $combined "runtimes\win-arm64\native\Matter.Windows.Controller.dll")) `
    -ReportDirectory (Join-Path $output "abi")
& (Join-Path $PSScriptRoot "test-package-consumers.ps1") -PackageVersion $packages.combined.version `
    -OutputDirectory (Join-Path $output "x64") -MatterSdkRoot $MatterSdkRoot `
    -PackageSource $packages.combined.source -NativeAot -Run:$Run
& (Join-Path $PSScriptRoot "test-package-consumers.ps1") -PackageVersion $packages.combined.version `
    -OutputDirectory (Join-Path $output "arm64") -MatterSdkRoot $MatterSdkRoot `
    -PackageSource $packages.combined.source -Architecture arm64 -NativeAot
