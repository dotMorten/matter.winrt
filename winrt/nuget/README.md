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

Camera APIs are experimental; signaling is capability-gated by
`MatterControllerCamera.IsSignalingSupported`. Enable
`ControllerOptions.EnableCameraSignaling` before creating a camera controller.
Typed discovery and allocated-video-stream metadata do not prove playback.
`TryDiscoverCamerasAsync` separates successful empty camera collections from
structured discovery failures, preserving known native status.
`ReadCapabilitiesAsync` provides typed cluster revisions, feature maps, and
attribute/command lists. Video start requires advertised video and the complete
client-offer command path, not a manufacturer name or assumed endpoint.
Video-only start omits AudioStreamID; protocol null would request automatic
audio selection. Camera-signaling development builds require the matching
native media-selection implementation. Additive `IMatterControllerCamera3`
provides typed audio capabilities, allocated-stream metadata, LiveView
allocation/release, explicit video/audio selection and remote microphone/speaker
mute/volume writes. It requires Camera device type 0x0142 (revision 1-2), stream
management/provider revision 2 and the camera provider/requestor composition.
Non-camera and incompatible endpoints remain unsupported.
Audio options have no usable default: first intersect the actual media engine's
formats with advertised codecs/channels/sample rates/bit depths. ReferenceCount
zero is valid. Only this controller's successful audio allocation grants release
ownership; external or automatically selected streams are never deallocated.
Unknown allocation or release outcomes block further allocation on that endpoint,
and releases are dispatched at most once. No retries or privacy changes are made.
Remote volume uses advertised bounds and never unmutes. Remote camera controls
are not local playback/capture controls.
`StartSessionAsync` and its Try counterpart distinguish Absent (omitted),
AutoSelect (protocol null), and ExplicitId (numeric, including zero) independently
for each medium. `IMatterCameraSession2` retains requested selections and resolved
nullable media/protocol IDs without changing the existing session interface.
Close local media and signaling before releasing owned streams, and finish
cleanup before controller shutdown. Listening/talkback media-engine integration,
profile negotiation and physical sound acceptance remain pending; this package
does not activate microphone capture or claim bidirectional interoperability.
`AllocateVideoStreamAsync` and `TryAllocateVideoStreamAsync` request a typed
H.264 LiveView stream with configurable, validated bounds. Options default to
640x480, 30 fps, 500000-2000000 bits per second, and a 1000 ms key-frame interval.
Successful allocation transfers cleanup responsibility for the returned stream
ID to the application. The advertised ReferenceCount measures transport use,
not allocation ownership; zero is valid before a transport uses the stream.
Explicitly close the session, then deallocate the owned stream exactly once with
`DeallocateVideoStreamAsync` or `TryDeallocateVideoStreamAsync`. Existing start
APIs never deallocate caller-owned streams. Cancellation/validation failure
deallocates a known owned stream; missing IDs or failed cleanup can leave
ownership uncertain. Do not blindly repeat deallocation of a potentially reused ID.
Explicit IM refusals preserve DynamicConstraintError, ConstraintError or
CommandRejected categories and the exact native code. These are not unknown
allocation outcomes or proof of a decoder defect, and never trigger automatic
retries or release of unowned streams.
New allocation and privacy APIs use additive `IMatterControllerCamera2`;
the original camera interface remains binary compatible.
`IsPrivacyEnabledAsync` reads advertised software/hardware privacy with a
30-second aggregate deadline; errors never become a false privacy snapshot.
Signal protocol IDs are nullable: null means the identity is not yet known;
zero is a valid bound ID, not an absent-value sentinel.
`MatterCameraIceServer` exposes owned, bounded URI arrays and preserves optional
credentials and CAID. `TryStartWithIceServersAsync` passes caller-supplied STUN/TURN
configuration to Matter signaling. Configure the external receiver with matching
servers and supported trust; the SDK does not connect to these servers or persist
their credentials. Original start APIs and the sample remain server-free.
`TryStartWithAllocatedVideoStreamAsync` returns structured failure diagnostics
without reducing native status to HRESULT alone. Cancellation is still WinRT
cancellation.
`TrySendCandidatesAsync` and `TryCloseAsync` return structured operation results
for ICE and bounded End cleanup; the throwing counterparts remain available.
`GetStateAsync` reports signaling-only negotiation, establishment, ending,
closure, and failure. Established is not decoded playback.
The package does not include a decoder or claim live-camera interoperability;
the repository contains an opt-in WebView2 receiver sample.

For multi-admin commissioning, request a sharing code from the device's
existing controller and set `OnNetworkCommissioningParameters.SetupCode` to
the manual code or `MT:` QR payload before calling `CommissionOnNetworkAsync`.
