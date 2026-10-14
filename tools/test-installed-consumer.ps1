[CmdletBinding()]
param(
    [Parameter(Mandatory)][string] $PublishedConsumer,
    [Parameter(Mandatory)][string] $OutputDirectory
)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$published = (Resolve-Path $PublishedConsumer).Path
$output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
$identity = "Matter.CombinedSdk.Acceptance"
if (Test-Path $output) { throw "Use a fresh installed-package test directory." }
if (Get-AppxPackage -Name $identity) { throw "An acceptance package is already registered; do not replace it." }
New-Item -ItemType Directory $output | Out-Null
foreach ($file in @("CombinedConsumer.exe", "Matter.Windows.Controller.dll")) {
    Copy-Item (Join-Path $published $file) $output
}
Copy-Item (Join-Path $root "samples\ControllerApp\Assets\StoreLogo.png") (Join-Path $output "logo.png")
[xml] $manifest = Get-Content (Join-Path $root "tests\CombinedConsumer\AppxManifest.xml") -Raw
[xml] $activation = Get-Content (Join-Path $root "winrt\nuget\Matter.Windows.Controller.manifest") -Raw
$server = $manifest.SelectSingleNode("//*[local-name()='InProcessServer']")
foreach ($class in $activation.SelectNodes("//*[local-name()='activatableClass']")) {
    $entry = $manifest.CreateElement("ActivatableClass", $server.NamespaceURI)
    $entry.SetAttribute("ActivatableClassId", $class.name)
    $entry.SetAttribute("ThreadingModel", $class.threadingModel)
    [void] $server.AppendChild($entry)
}
$manifestPath = Join-Path $output "AppxManifest.xml"
$manifest.Save($manifestPath)
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace MatterPackageAcceptance {
    [ComImport, Guid("2e941141-7f97-4756-ba1d-9decde894a3d"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IApplicationActivationManager {
        [PreserveSig]
        int ActivateApplication([MarshalAs(UnmanagedType.LPWStr)] string appId,
            [MarshalAs(UnmanagedType.LPWStr)] string arguments, uint options, out uint processId);
    }
    public static class Launcher {
        public static uint Activate(string appId, string arguments) {
            object manager = Activator.CreateInstance(Type.GetTypeFromCLSID(
                new Guid("45ba127d-10a8-46ea-8ab7-56ea9078943c")));
            try {
                uint processId;
                Marshal.ThrowExceptionForHR(((IApplicationActivationManager)manager).ActivateApplication(
                    appId, arguments, 0, out processId));
                return processId;
            } finally { Marshal.ReleaseComObject(manager); }
        }
    }
}
'@
$registered = $null
$process = $null
try {
    Add-AppxPackage -Register $manifestPath
    $registered = Get-AppxPackage -Name $identity
    if (-not $registered) { throw "Acceptance package was not registered." }
    $resultPath = Join-Path $output "result.json"
    $processId = [MatterPackageAcceptance.Launcher]::Activate(
        "$($registered.PackageFamilyName)!Acceptance", "--result `"$resultPath`"")
    $process = Get-Process -Id $processId -ErrorAction SilentlyContinue
    if ($process -and -not $process.WaitForExit(120000)) {
        Stop-Process -Id $processId
        throw "Installed consumer exceeded its two-minute deadline."
    }
    if (-not (Test-Path $resultPath)) { throw "Installed consumer did not produce an acceptance result." }
    $result = Get-Content $resultPath -Raw | ConvertFrom-Json
    if (-not $result.succeeded -or $result.packageFullName -ne $registered.PackageFullName) {
        throw "Consumer failed or was not launched with the expected installed package identity."
    }
    Write-Host "PASS installed x64 package identity and commissioning/camera native activation."
} finally {
    if ($process -and -not $process.HasExited) { Stop-Process -Id $process.Id }
    if (-not $registered) { $registered = Get-AppxPackage -Name $identity }
    if ($registered -and $registered.InstallLocation -eq $output) {
        Remove-AppxPackage -Package $registered.PackageFullName
    }
}
