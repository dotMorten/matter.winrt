# Matter WinRT

Native C++/WinRT Matter controller APIs and a WinUI 3 sample for x64 and
ARM64 Windows. The Matter SDK is pinned as the `matterforwindows` submodule so
the component and client UX can evolve independently from the SDK fork.

## Clone and build

```powershell
git clone --recurse-submodules https://github.com/dotMorten/matter.winrt
cd matter.winrt
.\tools\build.ps1
```

The build produces
`artifacts\Matter.Windows.Controller.0.1.0-preview.4.nupkg`. The package
contains a WinMD plus architecture-specific native DLLs for `win-x64` and
`win-arm64`.

Build the sample:

```powershell
dotnet build .\samples\ControllerApp\MatterControllerApp.csproj `
    -p:Platform=ARM64 -p:PlatformTarget=ARM64
```

The sample has separate pages for opening a commissioning window with a
device's sharing code and for reading or controlling devices already persisted
in the controller fabric.

This is a development preview. Operational credentials are file-backed and
test device attestation is enabled by default.
