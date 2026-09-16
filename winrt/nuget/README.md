# Matter.Windows.Controller

Development-preview C++/WinRT Matter controller for x64 and ARM64 Windows apps.
The package includes a WinMD reference and selects the matching native component
DLL from `runtimes/win-x64/native` or `runtimes/win-arm64/native`.
Managed .NET projects must also reference `Microsoft.Windows.CsWinRT` 2.3.1 or
later so the WinMD is projected into C# at build time.

Set `ControllerOptions.StoragePath` to an absolute, app-owned directory before
calling `MatterController.CreateAsync`. Only one controller may be active in a
process, and the storage directory is exclusively owned while it is open.

This preview uses file-backed operational credentials and allows test device
attestation by default. It is not production security infrastructure.

For multi-admin commissioning, request a sharing code from the device's
existing controller and set `OnNetworkCommissioningParameters.SetupCode` to
the manual code or `MT:` QR payload before calling `CommissionOnNetworkAsync`.
