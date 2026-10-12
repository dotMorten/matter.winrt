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

## Camera discovery and signaling development

The native Matter SDK (`richiemsft/matterforwindows`) and this WinRT SDK
(`dotMorten/matter.winrt`) are independent projects and Git repositories.
Native camera signaling and `chip-tool` belong to the native SDK; WinRT IDL,
projections, NuGet packaging, and the WinUI/WebView2 receiver belong here.
The dependency is one-way: this component builds against native SDK headers and
libraries. Neither the native SDK nor `chip-tool` depends on WinRT. The pinned
native submodule is a build dependency, not a merged WinRT/native project.
For local development, keep independent checkouts in sibling directories and
pass the native checkout through `-MatterSdkRoot`.

`MatterControllerCameraDiscovery` adds typed Descriptor access without changing
the existing generic attribute contract. `ReadDescriptorAsync` exposes PartsList,
ServerList, ClientList, and structured DeviceTypeList through typed WinRT
collections. Chunked attribute lists are reassembled before projection.
Malformed, null, and out-of-range values produce errors, not empty-list fallbacks.
`TryDiscoverCamerasAsync` returns a typed `MatterCameraDiscoveryResult` with
either a camera collection or structured validation/discovery failure. A
successful empty collection means a reachable topology with no camera cluster
composition; failure has no collection and preserves known native status.
Unknown failures stay unknown rather than being reported as unreachable.

`DiscoverCamerasAsync` walks the advertised endpoint topology from endpoint 0,
deduplicates endpoints, and returns each endpoint exposing both Camera AV Stream
Management and WebRTC Transport Provider. It does not assume a camera occupies
endpoint 1. Discovery is limited to 128 endpoints and 120 seconds; each attribute
read remains limited to 30 seconds. Canceling the asynchronous operation closes
its pending native read without shutting down the controller.

```csharp
var discovery = new MatterControllerCameraDiscovery(controller);
var cameras = await discovery.DiscoverCamerasAsync(nodeId);
foreach (var camera in cameras)
{
    foreach (var type in camera.DeviceTypes)
        Console.WriteLine($"Endpoint {camera.EndpointId}: {type.DeviceTypeId}, revision {type.Revision}");
}
```

Discovery reports cluster composition only, not media connectivity or playback.
For client-generated, video-only offers, set
`ControllerOptions.EnableCameraSignaling = true` before controller creation and
use `MatterControllerCamera.StartWithAllocatedVideoStreamAsync`. The SDK must be
built with signaling enabled; otherwise the operation is unsupported. Default
controllers do not host camera endpoints. `MatterControllerCamera.IsSignalingSupported`
reports the compiled capability without initializing an endpoint.
The option is exposed through the additive, versioned `IControllerOptions2`
interface; the existing `IControllerOptions` binary contract is unchanged.

The opt-in host uses the native SDK's code-driven Descriptor, WebRTC requestor,
and shared Binding cluster for its Camera Controller endpoint. Its Binding
manager uses the controller's persistent storage and shared CASE manager, with
on-demand rather than startup connections. Initialization errors are surfaced.
Teardown detaches the manager before the commissioner forgets its fabric,
preserving stored bindings without releasing another client's CASE sessions.
Only the active authenticated camera peer may read endpoint metadata; arbitrary
Binding writes and administration are denied. This endpoint composition is not
a claim of complete root-node/product certification.

Camera-enabled controllers listen for UDP and TCP on port 5540. Controller
creation fails explicitly if the required listener cannot be established; it
does not fall back to independently chosen, incorrectly advertised ports.

The start operation verifies the endpoint composition, advertised privacy
attributes, and the selected allocated LiveView stream before sending an offer.
`ReadCapabilitiesAsync` exposes the endpoint Descriptor and both camera clusters'
revisions, raw feature maps, attribute IDs, and accepted/generated command IDs
through typed collections. Unknown feature bits stay intact.
`SupportsClientOffer` requires the video feature and the complete advertised
offer, response, ICE, and End command path; it does not claim media compatibility
or certification of the reported revision. Start refreshes these capabilities
before sending commands and returns an explicit unsupported result if absent.
`ReadAllocatedVideoStreamsAsync` returns typed stream IDs, usage, codec,
frame-rate/resolution/bitrate bounds, key-frame interval, reference count, and
nullable watermark/OSD flags. Missing optional booleans remain null rather than
becoming false; unknown codec values remain explicit. Malformed entries,
reversed bounds, duplicate IDs, and reserved stream identifiers are errors.
**Stream allocation and deallocation are delegated to the caller.** Select
parameters and codecs from camera capabilities, allocate through Camera AV
Stream Management, then pass the returned stream ID. The session never frees a
caller-owned stream. Release it after closing the session, including on failure.
The preview does not implement automatic codec/stream selection.
`MatterCameraIceServer` provides owned typed URI arrays, independent optional
username/credential presence, and a nullable certificate-authority ID. Explicit
empty credentials and CAID zero stay present. Use `TryStartWithIceServersAsync`
to include the caller's configuration in ProvideOffer, and configure the external
WebRTC receiver with the same servers before generating its offer. An empty
server list omits the field; null lists and null entries are validation errors.
The original start APIs still omit servers and retain the LAN-only default.
Each operation accepts at most four servers, each with one to four STUN/STUNS or
TURN/TURNS URIs of at most 512 UTF-8 bytes; usernames and credentials are capped
at 256 bytes. URI controls/whitespace and embedded NULs are rejected. URL validity
and server connectivity must also be checked by the selected media engine.
The SDK does not connect to an ICE server itself or configure receiver trust
from CAID. Applications must verify support for their selected receiver.
Server values are not persisted or logged; applications retain ownership of
their supplied objects and must avoid logging their contents. Temporary native
credential copies are cleared after the command. No remote-access or TURN
interoperability claim is made.

`MatterCameraSession` exposes a local `Handle`, separate from the protocol
session identity in signals. `MatterCameraSignal.ProtocolSessionId` is nullable:
null means no authenticated protocol identity is known, while zero remains a
valid bound identifier. Use one `WaitForSignalAsync` consumer per session;
early answers and ICE remain queued until consumed. A polling timeout does not
end the session. `SendCandidatesAsync` sends typed ICE batches, and `CloseAsync`
attempts remote End before releasing the local endpoint. Close is idempotent.
Canceled or failed starts attempt bounded remote cleanup when an authenticated
protocol identity is known. Controller shutdown also attempts End before
stopping the native event loop and invalidates local sessions without deleting
commissioning storage.

The host currently permits one active session per controller. Operations are
serialized. `GetStateAsync` reports signaling-only snapshots: Negotiating until
the provider response and answer are both received, Established after both,
Failed after a native terminal failure, and Closed after remote End, local
release, or controller shutdown. Closing reports Ending while owned cleanup is
in progress. Reading state does not consume queued signals or grant access.
The session object is created only after the provider offer response, so creation
failures are returned by start rather than exposed as a live session. A snapshot
can become stale immediately; keep consuming signals for ordered notifications.
Established never means video is connected or playing. Each session receives a
fresh requestor endpoint to reject stale
messages. Explicitly await `CloseAsync`: releasing the WinRT object is not a
remote hangup guarantee. Discovery has a 120-second aggregate deadline, provider
commands and remote End have 30-second deadlines, and signal waits accept
1-30000 milliseconds. `TryStartWithAllocatedVideoStreamAsync` uses the same
preflight and signaling path but returns a typed `MatterCameraStartResult`.
Failure includes the stage (validation, descriptor, privacy, stream selection,
or signaling), category, HRESULT, exact native Matter status when present,
retryability, and a diagnostic message. Failure never supplies a session;
success never supplies a failure. Cancellation remains WinRT cancellation,
not a successful result. Retryable means retry after resolving the transient
condition, not an automatic retry. The original throwing API remains available.
`TrySendCandidatesAsync` and `TryCloseAsync` return `MatterCameraOperationResult`
with the same diagnostics at validation, ICE-candidate, and session-end stages.
Success has no failure. The throwing APIs remain available. Both close variants
finish bounded cleanup even if the caller cancels its wait; they release local
resources before reporting a remote End failure. The throwing discovery API
also remains available.

The [receiver sample](samples/ControllerApp/README.md#camera-receiver-development)
uses trusted WebView2 content to receive video only. It reports playback only
after a decoded frame and matching negotiated codec, never after successful
signaling. It offers only the selected stream codec when WebView2 advertises it.
This remains a
development preview: live-camera transport/playback, complete product cluster
composition, packaged consumption, and interoperability acceptance are not
verified.

For coordinated SDK development, both native build stages accept
`-MatterSdkRoot <path>` to build against an explicit working Matter SDK checkout
without changing the pinned submodule. The archive stage also accepts
`-BuildCameraSignaling` for the SDK's optional native signaling components.
Use the same SDK path and architecture in both stages. Default package builds
continue to use the pinned submodule; no development override is a released
package compatibility guarantee.

```powershell
.\tools\build-matter.ps1 -Architecture x64 -MatterSdkRoot C:\src\matterforwindows -BuildCameraSignaling
.\tools\build-winrt.ps1 -Architecture x64 -MatterSdkRoot C:\src\matterforwindows
.\tools\test-camera-discovery.ps1 -Architecture x64 -MatterSdkRoot C:\src\matterforwindows
.\tools\test-camera-nativeaot.ps1 -MatterSdkRoot C:\src\matterforwindows
node --test .\tools\test-camera-receiver.cjs
```

Build a local dual-architecture development package without changing the SDK
pin or overwriting the published preview version:

```powershell
.\tools\build.ps1 -Architecture all -MatterSdkRoot C:\src\matterforwindows `
    -BuildCameraSignaling     -Version 0.2.0-camera.13
.\tools\test-camera-nativeaot.ps1 -MatterSdkRoot C:\src\matterforwindows `
    -PackageVersion 0.2.0-camera.13
```

Explicit development build options cannot be combined with `-SkipBuild`, which
cannot establish the provenance of existing outputs. This is a local test
package, not a released SDK pin or an interoperability guarantee.
The NativeAOT package mode exercises the NuGet projection inputs and native
asset selection instead of directly referencing the working WinMD/DLL.
Package native assets are copied regardless of timestamps so switching from a
development DLL to a package cannot leave stale native code beside newer
metadata. The camera additions, including the nullable protocol ID, change preview WinRT interfaces; deploy the
matching new DLL and WinMD together rather than substituting old preview binaries.

The C++ projection tests exercise typed numeric and structured collections,
bridge-versus-camera classification, and opt-in controller close/reopen. The
x64 NativeAOT client uses a test-only native fixture to exercise typed descriptor
collections across the DLL boundary and runs real controller activation and
close/reopen, typed allocated streams, nullable flags, and structured failures.
Receiver tests use simulated peer connections, including early ICE,
stale callbacks, payload/queue limits, and 100 local start/stop cycles. None of
these tests establish live-device discovery, actual WebRTC media, or 100 camera
sessions. Run ARM64 executables on an ARM64 host.

## Clone and build

```powershell
git clone --recurse-submodules https://github.com/dotMorten/matter.winrt
cd matter.winrt
.\tools\build.ps1
```

The native build has two explicit stages. The first uses the Matter SDK's own
GN/Ninja build inside the submodule and produces one private controller SDK
archive:

```powershell
.\tools\build-matter.ps1 -Architecture x64
```

The second builds the C++/WinRT component directly with the MSVC and Windows
SDK tools, using the generated Matter headers and archive at their known
submodule-relative paths:

```powershell
.\tools\build-winrt.ps1 -Architecture x64
```

`build.ps1` runs both stages for x64 and ARM64 before packaging. The outer
repository is not a GN source tree and does not create directory junctions to
the SDK.

The build produces
`artifacts\Matter.Windows.Controller.0.1.0-preview.9.nupkg`. The package
contains a WinMD plus architecture-specific native DLLs for `win-x64` and
`win-arm64`.

Build the sample:

```powershell
dotnet build .\samples\ControllerApp\MatterControllerApp.csproj `
    -p:Platform=ARM64 -p:PlatformTarget=ARM64
```

The sample initializes its persisted controller fabric when the app launches
and closes it with the window. Its landing page presents known devices as a
grid and provides an empty-state call to action when none are commissioned.
**Add device** opens a manual sharing-code, camera QR-scanning, or Bluetooth LE
workflow; after connection, the app suggests the device-reported name and lets
the user edit it before finishing. Selecting a device opens its details page
with independently controllable rows for every discovered On/Off endpoint,
Level controls, and generic attribute queries.
Devices that already contain this controller fabric but are missing from the
local node index can be verified and restored by node ID without repeating
AddNOC.

The packaged sample also provides one shared **Matter Controls** Windows
widget. On a device details page, turn on **Matter Controls widget** for any
endpoint that exposes the On/Off cluster. Turn the switch off again to remove
that endpoint. Open the Windows Widgets board, choose **Add widgets**, and pin
**Matter Controls** once; every selected switch then appears in that medium or
large widget and can be toggled without opening the app. Windows does not
permit apps to pin widgets programmatically. The widget provider is an MSIX
app extension, so it is not available from the unpackaged sample profile.

This is a development preview. Operational credentials are file-backed and
test device attestation is enabled by default.
