[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $PackageVersion,
    [Parameter(Mandatory)][string] $OutputDirectory,
    [ValidateSet("x64", "arm64")][string] $Architecture = "x64",
    [string] $MatterSdkRoot,
    [string] $PackageSource,
    [switch] $NativeAot,
    [switch] $Run
)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
if (Test-Path $output) { throw "Use a fresh output directory for isolated projections." }
$source = if ($PackageSource) { (Resolve-Path $PackageSource).Path } else { Join-Path $root "artifacts" }
$package = Join-Path $source "Matter.Windows.Controller.$PackageVersion.nupkg"
if (-not (Test-Path $package)) { throw "Missing package under test: $package" }
function Get-PackageAssetHash {
    param([string] $Name)
    $archive = [System.IO.Compression.ZipFile]::OpenRead($package)
    try {
        $entry = $archive.GetEntry($Name)
        if (-not $entry) { throw "Package is missing $Name." }
        $stream = $entry.Open()
        try { return [Convert]::ToHexString([System.Security.Cryptography.SHA256]::HashData($stream)) }
        finally { $stream.Dispose() }
    } finally { $archive.Dispose() }
}
$expectedWinmd = Get-PackageAssetHash "winmd/Matter.Windows.Controller.winmd"
$expectedDll = Get-PackageAssetHash "runtimes/win-$Architecture/native/Matter.Windows.Controller.dll"
if ($NativeAot) {
    $sdkRoot = if ($MatterSdkRoot) { (Resolve-Path $MatterSdkRoot).Path } else { Join-Path $root "matterforwindows" }
    . (Join-Path $sdkRoot "scripts\setup\windows.ps1") -Architecture $Architecture
}
foreach ($project in @("BaselineConsumer", "CombinedConsumer")) {
    $projectOutput = Join-Path $output $project
    $publish = Join-Path $projectOutput "publish"
    $platform = if ($Architecture -eq "arm64") { "ARM64" } else { "x64" }
    & dotnet publish (Join-Path $root "tests\$project\$project.csproj") `
        --configuration Release --verbosity quiet --output $publish `
        "-p:MatterControllerPackageVersion=$PackageVersion" "-p:PlatformTarget=$platform" `
        "-p:RuntimeIdentifier=win-$Architecture" "-p:PublishAot=$($NativeAot.IsPresent.ToString().ToLowerInvariant())" `
        "-p:RestoreAdditionalProjectSources=$source" `
        "-p:BaseIntermediateOutputPath=$projectOutput\obj\" "-p:OutputPath=$projectOutput\bin\"
    if ($LASTEXITCODE -ne 0) { throw "$project package projection/build failed for $Architecture." }
    $assets = Get-Content (Join-Path $projectOutput "obj\project.assets.json") -Raw | ConvertFrom-Json
    $installedWinmd = @($assets.packageFolders.PSObject.Properties.Name |
        ForEach-Object { Join-Path $_ "matter.windows.controller\$PackageVersion\winmd\Matter.Windows.Controller.winmd" } |
        Where-Object { Test-Path $_ } | Select-Object -First 1)
    if ($installedWinmd.Count -ne 1 -or (Get-FileHash $installedWinmd[0]).Hash -ne $expectedWinmd -or
        (Get-FileHash (Join-Path $publish "Matter.Windows.Controller.dll")).Hash -ne $expectedDll) {
        throw "$project did not consume the exact package WinMD and architecture-specific DLL under test."
    }
    if ($Run) {
        & (Join-Path $publish "$project.exe")
        if ($LASTEXITCODE -ne 0) { throw "$project package activation failed for $Architecture." }
    }
}
