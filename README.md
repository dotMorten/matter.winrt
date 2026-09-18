# Matter WinRT

Native C++/WinRT Matter controller APIs and a WinUI 3 sample for x64 and
ARM64 Windows. The Matter SDK is pinned as the `matterforwindows` submodule so
the component and client UX can evolve independently from the SDK fork.

## Application-facing API

`Matter.Windows.Controller` is the application-facing Windows API over the
native Matter SDK. Applications consume its WinRT metadata and
architecture-specific DLL instead of linking to the Matter SDK's internal C++
types or depending on their ABI.

The API is device-type agnostic. Matter device types are compositions of
clusters on endpoints, so the generic `ReadAttributeAsync`,
`WriteAttributeAsync`, `InvokeCommandAsync`, and `SubscribeAttributeAsync`
methods accept arbitrary endpoint, cluster, attribute, and command identifiers.
`ReadEventsAsync` and `SubscribeEventAsync` provide the corresponding generic
event access required by event-driven devices. Together, these operations work
across every device type represented by the pinned Matter data model.
`BasicInformationCluster`, `OnOffCluster`, and `LevelControlCluster` are typed
conveniences, not a device-type support list.

Use `WriteAttributeTimedAsync` and `InvokeCommandTimedAsync` with
`TimedInteractionOptions` for attributes and commands that require a timed
interaction, including Door Lock, Energy EVSE, Administrator Commissioning,
and Thread management operations.

Generic values use `IPropertySet`: decimal context-tag strings identify
structure fields, `value` wraps a scalar root, inspectable vectors represent
Matter lists, and byte vectors represent octet strings. Responses use the same
recursive representation.

This package is a development preview. Its WinRT contract is the intended
application boundary, but preview releases do not yet guarantee ABI
compatibility. Applications must deploy the native DLL from the same package
version used at build time.

## Versioning and compatibility

- The complete policy is documented in
  [VERSIONING.md](VERSIONING.md).
- The NuGet package follows semantic versioning. Preview suffixes identify
  pre-stable contracts and may contain breaking API or ABI changes.
- A future stable release will preserve existing WinRT metadata within a major
  version. Additive APIs increment the minor version; breaking changes require
  a new major version.
- Stable API removal requires deprecation for at least one minor release unless
  an immediate security fix makes that impossible.
- The public binary contract is the WinRT metadata plus the standard activation
  exports. Matter SDK C++ headers, classes, STL types, and internal DLL symbols
  are not public ABI.
- Each package pins one Matter SDK commit. Updating that pin requires x64 and
  ARM64 component builds, sample builds against the produced package, and a
  package version change.
- The Windows workflow compares generated WinMD metadata with the adopted
  release baseline and validates the x64 and ARM64 native export tables.

## Clone and build

```powershell
git clone --recurse-submodules https://github.com/dotMorten/matter.winrt
cd matter.winrt
.\tools\build.ps1
```

The build produces
`artifacts\Matter.Windows.Controller.0.1.0-preview.6.nupkg`. The package
contains a WinMD plus architecture-specific native DLLs for `win-x64` and
`win-arm64`.

Build the sample:

```powershell
dotnet build .\samples\ControllerApp\MatterControllerApp.csproj `
    -p:Platform=ARM64 -p:PlatformTarget=ARM64
```

The sample initializes its persisted controller fabric when the app launches
and closes it with the window. Its **Connect** page accepts a sharing code or
BLE setup parameters and allocates local node identifiers automatically. Its
**Devices** page selects from a dropdown of known devices, reads common
clusters, queries generic attribute paths, and provides On/Off and Level
Control commands.

This is a development preview. Operational credentials are file-backed and
test device attestation is enabled by default.
