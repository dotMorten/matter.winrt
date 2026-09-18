# Matter WinRT controller sample

This diagnostic WinUI 3 app consumes the local `Matter.Windows.Controller`
NuGet package. It initializes and closes the controller with the application
window. The Connect page commissions with a multi-admin sharing code or
Bluetooth LE and allocates node identifiers internally. The Devices page uses
a known-device dropdown for generic attribute queries, Basic Information
reads, and On/Off and Level Control commands.

To add a device that is already owned by another Matter controller, open the
device's commissioning window in that controller and paste its manual sharing
code or `MT:` QR setup payload into **Sharing code**. The sample uses that code
for multi-admin on-network commissioning, then persists the assigned node and
can read Basic Information, On/Off, and Level Control state.

Create the package from the repository root:

```powershell
.\scripts\tools\windows_winrt_controller.ps1
```

Build or run the native ARM64 app:

```powershell
dotnet build .\examples\winrt-controller-app\MatterControllerApp.csproj `
    -p:Platform=ARM64 -p:PlatformTarget=ARM64

winapp run .\examples\winrt-controller-app\MatterControllerApp.csproj `
    --arch arm64
```

For an unpackaged build, add
`-p:WindowsPackageType=None -p:Unpackaged=true`. The project includes
registration-free WinRT manifest entries and copies the architecture-matched
component beside the app.

The component is a development preview. It uses file-backed operational keys
and permits test device attestation by default.
