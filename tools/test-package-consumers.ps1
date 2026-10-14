[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $PackageVersion,
    [Parameter(Mandatory)][string] $OutputDirectory,
    [ValidateSet("x64", "arm64")][string] $Architecture = "x64",
    [switch] $NativeAot,
    [switch] $Run
)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$output = [System.IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path $output) { throw "Use a fresh output directory for isolated projections." }
foreach ($project in @("BaselineConsumer", "CombinedConsumer")) {
    $projectOutput = Join-Path $output $project
    $publish = Join-Path $projectOutput "publish"
    $platform = if ($Architecture -eq "arm64") { "ARM64" } else { "x64" }
    & dotnet publish (Join-Path $root "tests\$project\$project.csproj") `
        --configuration Release --verbosity quiet --output $publish `
        "-p:MatterControllerPackageVersion=$PackageVersion" "-p:PlatformTarget=$platform" `
        "-p:RuntimeIdentifier=win-$Architecture" "-p:PublishAot=$($NativeAot.IsPresent.ToString().ToLowerInvariant())" `
        "-p:BaseIntermediateOutputPath=$projectOutput\obj\" "-p:OutputPath=$projectOutput\bin\"
    if ($LASTEXITCODE -ne 0) { throw "$project package projection/build failed for $Architecture." }
    if ($Run) {
        & (Join-Path $publish "$project.exe")
        if ($LASTEXITCODE -ne 0) { throw "$project package activation failed for $Architecture." }
    }
}
