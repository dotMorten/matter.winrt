[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string] $PackagePath,
    [Parameter(Mandatory)]
    [string] $ComponentDll
)

$ErrorActionPreference = "Stop"
$package = (Resolve-Path $PackagePath).Path
$component = (Resolve-Path $ComponentDll).Path
$archive = [System.IO.Compression.ZipFile]::OpenRead($package)
try {
    foreach ($name in @("Matter.Windows.Controller.dll", "Camera/receiver.html", "Camera/receiver.js", "Camera/receiver.css", "Camera/realtime-audio.js")) {
        if (-not $archive.GetEntry($name)) {
            throw "Camera MSIX is missing $name."
        }
    }
    if ($archive.GetEntry("Camera/camera-receiver-media-test.js")) {
        throw "Ordinary camera MSIX must not ship the synthetic capture fixture."
    }
    $manifestEntry = $archive.GetEntry("AppxManifest.xml")
    if (-not $manifestEntry) {
        throw "Camera MSIX is missing its manifest."
    }
    $reader = [System.IO.StreamReader]::new($manifestEntry.Open())
    try {
        [xml] $manifest = $reader.ReadToEnd()
    } finally {
        $reader.Dispose()
    }
    $registered = @($manifest.SelectNodes("//*[local-name()='InProcessServer'][*[local-name()='Path']='Matter.Windows.Controller.dll']/*[local-name()='ActivatableClass']") |
        ForEach-Object { $_.ActivatableClassId })
    foreach ($type in @("MatterControllerCameraDiscovery", "MatterControllerCamera", "MatterCameraIceCandidate", "MatterCameraIceServer", "MatterCameraVideoStreamOptions", "MatterCameraAudioStreamOptions", "MatterCameraStreamSelection")) {
        if ("Matter.Windows.Controller.$type" -notin $registered) {
            throw "Camera MSIX does not register $type for native WinRT activation."
        }
    }
    if (-not $manifest.SelectSingleNode("//*[local-name()='DeviceCapability'][@Name='microphone']")) {
        throw "Camera MSIX does not declare microphone capability for explicit push-to-talk."
    }
    $nativeStream = $archive.GetEntry("Matter.Windows.Controller.dll").Open()
    $hash = [System.Security.Cryptography.SHA256]::Create()
    try {
        $actualHash = [BitConverter]::ToString($hash.ComputeHash($nativeStream)).Replace("-", "")
    } finally {
        $hash.Dispose()
        $nativeStream.Dispose()
    }
    if ($actualHash -ne (Get-FileHash $component -Algorithm SHA256).Hash) {
        throw "Camera MSIX contains a mismatched native component."
    }
} finally {
    $archive.Dispose()
}
Write-Host "PASS camera MSIX receiver content, native activation registrations, and matched DLL."
