[CmdletBinding()]
param(
    [ValidateSet("x64", "arm64", "all")]
    [string] $Architecture = "all",
    [string] $Version = "0.2.0-camera-commissioning.2",
    [string] $OutputDirectory = "artifacts",
    [switch] $SkipBuild,
    [string] $MatterSdkRoot,
    [switch] $BuildCameraSignaling
)

$ErrorActionPreference = "Stop"
$outerRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$artifactPath = Join-Path $outerRoot $OutputDirectory
$stagingPath = Join-Path $artifactPath "Matter.Windows.Controller.$Version"
$packagePath = Join-Path $artifactPath "Matter.Windows.Controller.$Version.nupkg"
if ((Test-Path $packagePath) -or (Test-Path $stagingPath) -or (Test-Path ($packagePath + ".zip"))) {
    throw "Package versions are immutable. Select an unused version/output directory: $Version"
}
$architectures = if ($Architecture -eq "all") { @("x64", "arm64") } else { @($Architecture) }
$buildInformation = @()
$sdkRoot = if ([string]::IsNullOrWhiteSpace($MatterSdkRoot)) {
    Join-Path $outerRoot "matterforwindows"
} else {
    (Resolve-Path $MatterSdkRoot).Path
}
if ($SkipBuild -and ($BuildCameraSignaling -or -not [string]::IsNullOrWhiteSpace($MatterSdkRoot))) {
    throw "Explicit SDK or camera build options require rebuilding; they cannot verify existing outputs with -SkipBuild."
}

foreach ($targetArchitecture in $architectures) {
    $outputName = "win-winrt-$targetArchitecture"
    $outputPath = Join-Path $outerRoot "out\$outputName"
    if (-not $SkipBuild) {
        & (Join-Path $PSScriptRoot "build-matter.ps1") -Architecture $targetArchitecture `
            -MatterSdkRoot $sdkRoot -BuildCameraSignaling:$BuildCameraSignaling
        if ($LASTEXITCODE -ne 0) { throw "Matter SDK build failed for $targetArchitecture." }
        & (Join-Path $PSScriptRoot "build-winrt.ps1") -Architecture $targetArchitecture -MatterSdkRoot $sdkRoot
        if ($LASTEXITCODE -ne 0) { throw "WinRT component build failed for $targetArchitecture." }
    }

    foreach ($asset in @("Matter.Windows.Controller.dll", "Matter.Windows.Controller.winmd")) {
        if (-not (Test-Path (Join-Path $outputPath $asset))) {
            throw "Missing $targetArchitecture output: $asset"
        }
    }
    $informationPath = Join-Path $outputPath "build-info.json"
    if (-not (Test-Path $informationPath)) {
        throw "Missing matched build provenance for $targetArchitecture. Rebuild the component."
    }
    $information = Get-Content $informationPath -Raw | ConvertFrom-Json
    if ($information.architecture -ne $targetArchitecture -or
        $information.dllSha256 -ne (Get-FileHash (Join-Path $outputPath "Matter.Windows.Controller.dll")).Hash -or
        $information.winmdSha256 -ne (Get-FileHash (Join-Path $outputPath "Matter.Windows.Controller.winmd")).Hash) {
        throw "The $targetArchitecture metadata/DLL does not match its build provenance."
    }
    $buildInformation += $information
}

if (@($architectures).Count -ne 2) {
    Write-Host "Built $Architecture. Select -Architecture all to create the dual-architecture package."
    return
}

if ($buildInformation.Count -ne 2 -or
    $buildInformation[0].sdkCommit -ne $buildInformation[1].sdkCommit -or
    $buildInformation[0].nativeCommit -ne $buildInformation[1].nativeCommit) {
    throw "Architecture outputs must use the same SDK/native commits."
}
Write-Host "Validating architecture metadata equivalence (not published-baseline acceptance)."
& {
    . (Join-Path $sdkRoot "scripts\setup\windows.ps1") -Architecture x64
    & (Join-Path $PSScriptRoot "verify-abi.ps1") `
        -BaselineWinmd (Join-Path $outerRoot "out\win-winrt-x64\Matter.Windows.Controller.winmd") `
        -CurrentWinmd (Join-Path $outerRoot "out\win-winrt-arm64\Matter.Windows.Controller.winmd") `
        -EquivalentWinmd (Join-Path $outerRoot "out\win-winrt-x64\Matter.Windows.Controller.winmd") `
        -NativeDll @((Join-Path $outerRoot "out\win-winrt-x64\Matter.Windows.Controller.dll"),
                    (Join-Path $outerRoot "out\win-winrt-arm64\Matter.Windows.Controller.dll"))
}
New-Item -ItemType Directory -Force $artifactPath | Out-Null
New-Item -ItemType Directory -Force `
    (Join-Path $stagingPath "winmd"), `
    (Join-Path $stagingPath "buildTransitive"), `
    (Join-Path $stagingPath "runtimes\win-x64\native"), `
    (Join-Path $stagingPath "runtimes\win-arm64\native") | Out-Null

Copy-Item (Join-Path $outerRoot "out\win-winrt-x64\Matter.Windows.Controller.dll") `
    (Join-Path $stagingPath "runtimes\win-x64\native")
Copy-Item (Join-Path $outerRoot "out\win-winrt-arm64\Matter.Windows.Controller.dll") `
    (Join-Path $stagingPath "runtimes\win-arm64\native")
Copy-Item (Join-Path $outerRoot "out\win-winrt-x64\Matter.Windows.Controller.winmd") `
    (Join-Path $stagingPath "runtimes\win-x64\native")
Copy-Item (Join-Path $outerRoot "out\win-winrt-arm64\Matter.Windows.Controller.winmd") `
    (Join-Path $stagingPath "runtimes\win-arm64\native")
Copy-Item (Join-Path $outerRoot "out\win-winrt-x64\Matter.Windows.Controller.winmd") `
    (Join-Path $stagingPath "winmd")
Copy-Item (Join-Path $outerRoot "winrt\Matter.Windows.Controller.xml") `
    (Join-Path $stagingPath "winmd")
Copy-Item (Join-Path $outerRoot "winrt\nuget\Matter.Windows.Controller.props") `
    (Join-Path $stagingPath "buildTransitive")
Copy-Item (Join-Path $outerRoot "winrt\nuget\Matter.Windows.Controller.targets") `
    (Join-Path $stagingPath "buildTransitive")
Copy-Item (Join-Path $outerRoot "winrt\nuget\Matter.Windows.Controller.manifest") `
    (Join-Path $stagingPath "winmd")
[ordered]@{
    packageVersion = $Version
    builds = $buildInformation
} | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $stagingPath "build-info.json") -Encoding utf8
Copy-Item (Join-Path $outerRoot "winrt\nuget\README.md") $stagingPath
Copy-Item (Join-Path $sdkRoot "LICENSE") $stagingPath

$nuspec = @"
<?xml version="1.0"?>
<package xmlns="http://schemas.microsoft.com/packaging/2013/05/nuspec.xsd">
  <metadata>
    <id>Matter.Windows.Controller</id>
    <version>$Version</version>
    <authors>Matter Windows contributors</authors>
    <license type="file">LICENSE</license>
    <readme>README.md</readme>
    <projectUrl>https://github.com/dotMorten/matter.winrt</projectUrl>
    <repository type="git" url="https://github.com/dotMorten/matter.winrt.git" />
    <description>Development-preview native C++/WinRT Matter controller for x64 and ARM64 Windows applications.</description>
    <tags>matter winrt windows iot controller</tags>
    <dependencies>
      <group targetFramework="net8.0-windows10.0.19041.0">
        <dependency id="Microsoft.Windows.CsWinRT" version="[2.3.1]" />
      </group>
    </dependencies>
  </metadata>
</package>
"@
Set-Content -Path (Join-Path $stagingPath "Matter.Windows.Controller.nuspec") -Value $nuspec -Encoding utf8
Compress-Archive -Path (Join-Path $stagingPath "*") -DestinationPath ($packagePath + ".zip")
Move-Item ($packagePath + ".zip") $packagePath
Remove-Item -Recurse -Force $stagingPath
Get-FileHash $packagePath -Algorithm SHA256 |
    ForEach-Object { "$($_.Hash)  $([System.IO.Path]::GetFileName($packagePath))" } |
    Set-Content ($packagePath + ".sha256") -Encoding ascii

Write-Host "Created $packagePath"
