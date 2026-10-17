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

BLE network commissioning can report native stage transitions, elapsed time,
transport changes, retries, and structured failures through the additive
`MatterControllerCommissioning` class. Its `ProgressChanged` handlers run in
order on a thread-pool thread rather than the UI thread. The returned
`MatterCommissioningResult` distinguishes success, timeout, the failed stage,
and common failure categories without requiring log parsing. Canceling the
returned `IAsyncOperation` stops native pairing and completes the operation in
the canceled state.

`MatterNetworkInterfaceProvider.GetEligibleNetworkInterfacesAsync` enumerates
interfaces that can participate in operational Matter discovery. Pass a
`MatterNetworkInterfaceSelection` to choose automatic routing, prefer one
interface with fallback, or require one interface without fallback. Interface
identifiers are valid only while the adapter exists; enumerate again before a
later commissioning operation. Interface selection is captured when
commissioning starts and applies only to operational DNS-SD, address
resolution, CASE, and commissioning-complete traffic. It does not affect BLE.

```csharp
var commissioning = new MatterControllerCommissioning(controller);
commissioning.ProgressChanged += (_, progress) =>
    Console.WriteLine($"{progress.Stage}: {progress.DiagnosticMessage}");

var selection = new MatterNetworkInterfaceSelection(
    MatterNetworkInterfaceSelectionMode.Automatic, 0);
MatterCommissioningResult result =
    await commissioning.CommissionBleAsync(parameters, selection);
if (!result.Succeeded)
{
    throw new InvalidOperationException(result.DiagnosticMessage);
}
```

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

The `windows-camera-commissioning-compatible` branch combines the camera topic
through `455b62d` with structured commissioning and per-operation interface
selection. The next development preview is `0.2.0-preview.2`, including
[read cancellation and per-node admission fixes](docs/camera-sdk-reliability.md).
CI produces the
dual-architecture package and its SHA256 as downloadable GitHub Actions
artifacts; it does not publish to nuget.org. See
[combined SDK acceptance and migration](docs/combined-sdk-acceptance.md) for the
declared native dependency, package-consumer fixtures, and outstanding release
gates. The actual published `0.1.0-preview.14` WinMD is required for acceptance;
passing an older preview comparison is not a substitute.

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
Existing video-only start APIs now omit AudioStreamID using the native tagged
selection API. Present protocol null requests automatic audio selection and is
not used as a substitute for no audio. This camera preview requires the matching
native SDK media-selection implementation when built with signaling enabled;
typed audio protocol APIs are described below. A separate media engine is still
required for listening or talkback; the SDK never activates a microphone.
`ReadCapabilitiesAsync` exposes the endpoint Descriptor and both camera clusters'
revisions, raw feature maps, attribute IDs, and accepted/generated command IDs
through typed collections. Unknown feature bits stay intact.

### Typed camera audio protocol development

`IMatterControllerCamera3` adds audio capability/readback, allocation/release,
explicit-media session start, and remote microphone/speaker mute/volume APIs.
Existing camera interfaces and their video-only audio omission are preserved.
This is a **camera-specific protocol adapter**, not a universal microphone or
speaker API. Its new operations require Camera device type `0x0142` at revision
1 or 2, Camera AV Stream Management and WebRTC Transport Provider servers at
the current generated revision 2, and a WebRTC Transport Requestor client.
Other device types, future revisions, missing features and missing commands
fail explicitly before allocation or signaling. Legacy discovery still reports
cluster composition, and is not sufficient authorization for this audio adapter.

`ReadAudioCapabilitiesAsync` returns raw microphone/speaker codec, channel,
sample-rate and bit-depth lists, nullable advertised remote mute/volume bounds
and TwoWayTalkSupport, camera cluster metadata, privacy and network bandwidth.
Missing optional controls remain null; unknown codec/talk values are preserved.
Numeric capability lists also retain unusable entries (for example `[24,0]`)
instead of inventing a replacement or discarding the raw evidence. The tolerant
policy permits only usable, advertised entries: zero rates/depths are never
selectable, and an all-invalid microphone does not support listening. Invalid or
absent speaker formats gate talkback independently, not otherwise valid listening.
The sample reports direction, attribute, field and raw offending values as
warnings. Wrong TLV types, missing required fields and numeric overflow remain
errors with microphone/speaker attribute context.
`SupportsListening` identifies the candidate audio protocol path with a known
codec, **not media-engine compatibility or audible playback**. Intersect these
formats with the actual media engine before allocating; TwoWayTalkSupport alone
does not establish bidirectional audio or authorize microphone capture.
Camera allocation bit depth describes source metadata, not browser-managed PCM
precision. Opus RTP negotiates a compressed codec and a 48 kHz clock; receiving a
24-bit allocation does not assert 24-bit browser capture or camera speaker output.

`MatterCameraAudioStreamOptions` deliberately has no usable default profile.
Supply a negotiated Opus or AAC-LC codec, 1-8 channels, nonzero sample rate and
bitrate, and 8/16/24/32 bit depth. Allocation verifies the advertised microphone
format, LiveView usage, bandwidth, privacy, response ID and exact readback
metadata. Readback preserves transport ReferenceCount, including zero.
The throwing and Try allocation methods use the same owned-ID rollback and
cancellation cleanup as video. There are no automatic allocation retries.

Audio ownership is shared by camera wrappers on the same controller and scoped
by node/endpoint/returned ID. One audio allocation may be pending or owned per
endpoint, with at most eight tracked endpoints per controller. Known correlated
refusals release the pending slot; an unknown outcome or failed deallocation
blocks another allocation on that endpoint. Metadata reads, explicit selection
and automatic selection do **not** acquire cleanup ownership. Audio release
rejects unowned, wrong-peer/endpoint and already-attempted IDs without dispatch.
Stop local media, close the signaling session, and call
`DeallocateAudioStreamAsync` or `TryDeallocateAudioStreamAsync` exactly once for
an allocation created by this controller. Finish this cleanup before closing the
controller. Do not reopen a controller to work around uncertain remote ownership.

`StartSessionAsync`/`TryStartSessionAsync` require two immutable
`MatterCameraStreamSelection` objects: Absent omits the protocol field,
AutoSelect sends protocol null, and ExplicitId sends the nonreserved numeric ID
(including zero). Both absent is invalid. Provide a bounded SDP offer and an
explicit collection of zero to four ICE servers. Selected LiveView formats,
capabilities and privacy are checked before signaling and after the response;
failed validation closes the owned session, never externally selected streams.
`IMatterCameraSession2` preserves requested selections separately from nullable
resolved video/audio IDs and the protocol ID, including protocol ID zero and
metadata after close. Canceling startup also closes a late-created session.

Remote mute/volume writes require the corresponding advertised camera feature,
format and attribute. Volumes use the device's ordered min/max bounds, not a
universal percentage; writing volume never unmutes. Privacy prevents unmuting.
These controls are distinct from local output mute, local capture mute and
microphone permission. They never change camera privacy.

The SDK adds protocol and ownership APIs, not a media engine. The opt-in sample
now has a separate receive-only Opus renderer, capability/profile negotiation
and explicit permission-gated full-duplex push-to-talk. Video-only remains the
default; half-duplex and physical audibility acceptance are not claimed.
See the sample README for controls, cleanup and hardware acceptance limitations.
The current normative specification is unavailable;
the revision gate is based on checked-in generated definitions/server behavior,
not a certification claim.

### Video allocation and signaling compatibility

`SupportsClientOffer` requires the video feature and the complete advertised
offer, response, ICE, and End command path; it does not claim media compatibility
or certification of the reported revision. Start refreshes these capabilities
before sending commands and returns an explicit unsupported result if absent.
`ReadAllocatedVideoStreamsAsync` returns typed stream IDs, usage, codec,
frame-rate/resolution/bitrate bounds, key-frame interval, reference count, and
nullable watermark/OSD flags. Missing optional booleans remain null rather than
becoming false; unknown codec values remain explicit. Malformed entries,
reversed bounds, duplicate IDs, and reserved stream identifiers are errors.
`AllocateVideoStreamAsync` and `TryAllocateVideoStreamAsync` provide typed
**H.264 LiveView allocation** through Camera AV Stream Management.
`MatterCameraVideoStreamOptions` defaults to exact 640x480 at 30 fps,
500000-2000000 bits per second, and a 1000 ms key-frame interval. Applications
may set nonzero, ordered resolution/frame-rate/bitrate bounds and a key-frame
interval before allocation; options are snapshotted before background work.
Preflight checks advertised video/allocation commands, privacy, sensor limits,
minimum viewport, and LiveView usage. The returned stream must match the
request. `ReferenceCount` reports transport usage, not allocation existence:
a newly allocated stream with count zero is valid before signaling starts.
The SDK preserves that value, does not choose another codec, and does not
silently relax unsupported parameters.

A successful allocation transfers **cleanup responsibility for the returned
stream ID** to the caller, not a protocol transport-count increment.
Close the signaling session, then deallocate that owned stream exactly once with
`DeallocateVideoStreamAsync` or `TryDeallocateVideoStreamAsync`. Metadata reads
do not acquire ownership, and existing start APIs never release caller-owned
streams. Structured allocation results contain either a stream or a failure,
including allocation/deallocation stages and exact known native status.
Cancellation remains cancellation. A dispatched allocation is allowed to finish
so cancellation or metadata-validation failure can deallocate its known stream.
Missing/reserved response IDs or failed cleanup can leave remote ownership
uncertain: do not guess an ID or blindly repeat deallocation of a potentially
reused ID. A transport count of zero is not proof that an arbitrary stream is
owned by this caller; a positive count may prevent deallocation while in use.
Proven local command-encoding and controller-state failures remain at the
Validation stage; they do not indicate a remote allocation. The
StreamAllocation stage begins only at the connection/dispatch boundary, and
unknown outcomes there retain the same conservative ownership safeguards.
Explicit IM allocation refusals remain at StreamAllocation with
`DynamicConstraintError`, `ConstraintError`, or `CommandRejected` categories,
alongside existing access/unsupported/state/resource categories. The exact
native status is preserved; rejection is not an unknown allocation outcome.
The sample does not lock out allocation merely because a correlated rejection
was returned, but does not automatically retry. A later failed release still
has uncertain ownership regardless of its category.
Deallocation continues its bounded cleanup even if the asynchronous wait is
canceled. The original camera interface remains unchanged; allocation and
privacy methods use the additive `IMatterControllerCamera2` interface.

`IsPrivacyEnabledAsync` reads the current advertised software/hardware privacy
attributes with cancellation and a 30-second aggregate deadline. Either enabled
attribute returns true; absent attributes are not invented, and read or type
errors fail rather than returning false. This snapshot is not a subscription or
an instantaneous privacy-enforcement guarantee.
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
after a decoded frame, matching negotiated codec, and dimensions within the
allocation, never after successful signaling. It offers only the selected stream
codec when WebView2 advertises it, monitors decoded-frame progress, and polls
advertised privacy between bounded reads. The sample can allocate its own stream
and ends signaling before releasing that reference on Stop, window close, or
controller shutdown. Externally allocated streams remain caller-owned.
This remains a development preview: physical-camera transport/playback,
complete product cluster composition, installed-app execution, and
interoperability acceptance are not verified.

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
    -BuildCameraSignaling -Version 0.2.0-preview.2
.\tools\test-camera-nativeaot.ps1 -MatterSdkRoot C:\src\matterforwindows `
    -PackageVersion 0.2.0-preview.2
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
close/reopen, typed allocated streams, allocation options/results, the additive
camera interface, privacy input/lifecycle errors, nullable flags, and structured
failures. Allocation ownership tests cover 100 cancellation/release races and
preserve uncertain release failures without repeating deallocation.
The same test command also runs private production TLV-bridge regressions:
generated camera allocation round trips preserve distinct nested resolutions,
optional true/false flags, structures inside arrays, and following sibling
fields. Pre-dispatch diagnostic tests verify local encoding failures do not
become uncertain remote allocations. These tests add no public API or exports.
Command-response regressions cover correlated DynamicConstraintError,
ConstraintError and other IM refusals, and reject uncorrelated responses.
NativeAOT tests exercise the receiver's actual ownership policy for rejection
versus uncertain timeout across the projection boundary. Generated metadata
tests cover both defaults and the reported G350 1920x1080 / 30-120 fps /
10000-bit/s / 4000-ms allocation profile with zero, one and maximum transport
counts. Allocation validation and start selection accept zero without weakening
stream-ID, LiveView, codec, bounds, privacy or cleanup checks. This is regression
evidence, not successful camera signaling or playback.

Advertised sensor limits, minimum viewport, rate-distortion points and maximum
network bandwidth are not a promise that every combination inside those bounds
can be allocated. The upstream Linux camera delegate checks its available HAL
stream configurations and returns DynamicConstraintError when none match;
encoder/pixel-rate exhaustion is separately reported as ResourceExhausted.
These are upstream example behaviors, not evidence of a vendor camera's
implementation. When allocation is rejected, inspect stream usage priorities,
feature-gated watermark/OSD requirements, all allocated stream usages, and
the device's supported encoder tuples. A broad range using the total network
limit as a stream bitrate may be less compatible than a constrained tuple.
Do not change priorities, relax parameters, or release unowned streams
automatically. No allocation rejection establishes a WebRTC or decoder defect.
Receiver tests use simulated peer connections, including early ICE,
stale callbacks, decoded dimension bounds/progress, payload/queue limits, and
100 local start/stop cycles. None of
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
`artifacts\Matter.Windows.Controller.0.1.0-preview.10.nupkg`. The package
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
