# Matter WinRT controller sample

This diagnostic WinUI 3 app consumes the local `Matter.Windows.Controller`
NuGet package. It initializes and closes the controller with the application
window. The landing page shows known devices in a grid. The add-device dialog
accepts a manual multi-admin sharing code, scans a Matter QR code with a
selected camera-backed barcode scanner, or commissions over Bluetooth LE. After
connection it suggests the device-reported name and allows editing before the
device appears in the list. Selecting a device opens generic attribute state,
Basic Information, independently controllable rows for every On/Off endpoint,
and available Level Control commands.

If commissioning reports that the NOC or fabric already exists, the app
automatically verifies operational access with the attempted node ID and
restores the device to the local node index.

To add a device that is already owned by another Matter controller, open the
device's commissioning window in that controller and paste its manual sharing
code, paste its `MT:` QR setup payload, or select **Scan QR code** and point the
camera preview at it. The sample uses that code for multi-admin on-network
commissioning, then persists the assigned node and can read Basic Information,
On/Off, and Level Control state.

## Windows widget

The packaged app registers one shared **Matter Controls** widget. To add a
switch:

1. Open the device details page and select the endpoint that exposes On/Off.
2. Turn on **Matter Controls widget** in the On/Off card. Turn it off to
   remove that endpoint later.
3. Open Windows Widgets, choose **Add widgets**, and pin **Matter Controls**.

Windows requires the user to pin a widget and does not expose an API for the
app to do so. Pin the widget only once; all selected On/Off endpoints appear
in the same widget. The widget supports medium and large sizes, refreshes the
current switch state when activated, and can issue On/Off toggles while the
main app is closed. Connection failures are shown in the widget and the action
can be retried.

Windows Widgets requires the packaged MSIX profile. It is unavailable from the
unpackaged profile because that profile does not register the widget provider
app extension.

Create the package from the repository root:

```powershell
.\tools\build.ps1
```

Build or run the native ARM64 app:

```powershell
dotnet build .\samples\ControllerApp\MatterControllerApp.csproj `
    -p:Platform=ARM64 -p:PlatformTarget=ARM64

winapp run .\samples\ControllerApp\MatterControllerApp.csproj `
    --arch arm64
```

For an unpackaged build, add
`-p:WindowsPackageType=None -p:Unpackaged=true`. The project includes
registration-free WinRT manifest entries and copies the architecture-matched
component beside the app.

## Camera receiver development

The receiver is an opt-in development path, not part of the released NuGet
package. Build matched SDK and WinRT outputs with the commands in the repository
README, including `-BuildCameraSignaling`, then compile the sample against that
checkout rather than the published package:

```powershell
dotnet build .\samples\ControllerApp\MatterControllerApp.csproj `
    -p:Platform=x64 -p:Unpackaged=true `
    -p:MatterCameraDevelopmentRoot=C:\src\matter.winrt
```

Alternatively, build against a locally produced camera package rather than
direct development outputs:

```powershell
dotnet build .\samples\ControllerApp\MatterControllerApp.csproj `
    -p:Platform=x64 -p:Unpackaged=true `
    -p:MatterControllerPackageVersion=0.2.0-camera.20 -p:MatterCameraDevelopment=true
```

The package version property defaults to the existing published preview, and
camera UI remains opt-in. For an MSIX build omit `-p:Unpackaged=true`.
To produce an unsigned local MSIX, also set `GenerateAppxPackageOnBuild=true`,
`AppxPackageSigningEnabled=false`, and `AppxBundle=Never`. Packaging does not
verify launch, live media, or hardware interoperability.

```powershell
.\tools\test-camera-msix.ps1 -PackagePath C:\path\receiver.msix `
    -ComponentDll .\out\win-winrt-x64\Matter.Windows.Controller.dll
```

This checks receiver content, camera activation registrations, and the exact
native DLL in the package. The sample includes camera class registrations in
both its unpackaged application manifest and MSIX manifest.
The `.20` SDK adds typed camera audio protocol APIs and versioned session
selection metadata. The receiver provides a separate WebRTC audio engine:
video-only is the default, listening is explicit, and full-duplex push-to-talk
requires a held button/key and Windows/WebView microphone permission. The SDK
itself does not capture or decode media. Physical audibility and representative
hardware acceptance remain unverified.

Launch the resulting executable with `--camera`, select a commissioned device,
and open **Camera receiver (development)**. Discover its camera endpoints and
select **Allocate H.264 LiveView (640x480, 30 fps)**, then **Start receiver**.
The sample requests 500000-2000000 bits per second and a 1000 ms key-frame
interval; unsupported defaults fail explicitly. SDK callers can configure
`MatterCameraVideoStreamOptions` for other supported H.264 LiveView bounds.
Alternatively, read allocated streams and choose an **advertised allocated
LiveView stream** whose reference you already own.
The receiver reads advertised cluster revisions, features, and command lists
before offering a stream selection, and refuses incomplete video/client-offer
capabilities. The SDK refreshes these checks again at start.
Stop, window close, and controller shutdown finish a pending sample allocation,
end the signaling session, and deallocate the sample-owned stream exactly once.
Externally allocated streams are never deallocated by the sample: stop the
receiver before deallocating your owned stream. An uncertain allocation/release
disables further camera starts/allocations for that node on the shared controller,
including replacement receiver windows; it never guesses IDs or blindly repeats
deallocation of a potentially reused stream ID.
Proven local encoding failures report Validation and do not disable further
allocation as an uncertain remote outcome. Unknown outcomes after dispatch
retain the ownership safeguards above; the sample does not automatically retry.
Explicit camera refusals such as DynamicConstraintError preserve their category
and native IM status without imposing the uncertain-allocation lockout. A
rejection transfers no owned stream; the sample does not deallocate one.
The stream's `ReferenceCount` measures active transport use. Zero is valid for
an unused allocation and does not prevent Start; it never establishes ownership
of a stream obtained only through metadata reads. SDK callers may configure the
reported G350 allocation profile (1920x1080, 30-120 fps, fixed 10000-bit/s bitrate,
4000-ms key-frame interval) through `MatterCameraVideoStreamOptions`; the sample's
default button does not silently switch to this profile or retry.

Select the explicit media mode before **Start receiver**. Video-only omits
AudioStreamID rather than requesting automatic selection with protocol null.
Listening allocates an owned camera microphone stream after fresh capability,
firmware, privacy and actual browser-codec checks. The adapter currently accepts
Opus mono/16-bit profiles at mutually supported sample rates, with an AV bandwidth
budget. It tries at most three advertised candidates, and only advances after
correlated DynamicConstraintError/ConstraintError refusals. Unknown outcomes,
resource exhaustion and command/state refusals are not format fallback signals.
No vendor quirks or global G350 profile are installed. A five-minute successful
profile hint never replaces fresh capability/privacy reads; firmware, capability,
mode/bandwidth changes and relevant failures invalidate it.

Talkback additionally requires compatible speaker formats and reported full-duplex
support; unsupported/unknown revisions and half-duplex fail explicitly before
audio allocation. A muted/zero-volume camera speaker is not implicitly changed.
Hold **Hold to talk**, Space, or Enter on that focused button only after transport
connects. Starting a talk-capable session does not activate a microphone. Browser
capture requests require a trusted explicit press, a current session token, an
active window, a fresh privacy check and native authorization. The host leaves
permission to Windows/WebView rather than granting it, and does not persist a
permission grant. Release, backgrounding, disconnect, permission revocation,
privacy, Stop and close stop local tracks; reconnect never resumes capture.
Late permission completion stops its tracks without transmitting. Permission
and sender activation waits are bounded. Half-duplex serialization and physical
camera-speaker interoperability are not claimed.

**Mute local playback**, local output volume and input/output device selection
are media-client controls, not camera microphone/speaker controls. Device
enumeration does not request capture. Unsupported/denied output selection and
autoplay denial are local playback conditions, not camera disconnections.
**Resume local playback** retries blocked playback explicitly. Capture and
outgoing-packet indicators are independent; packet counts are not proof that
the camera speaker is audible.

Stop closes browser media and the Matter session before attempting owned audio
and video release once. Partially completed audio preparation recovers the
returned allocation before releasing it. Unknown allocation, canceled signaling,
failed session cleanup or failed deallocation blocks new starts/allocations for
that node on the shared controller. The bounded uncertainty ledger fails closed
if full; closing/reopening a receiver window does not clear it. Reinitializing a
controller is not remote reconciliation and must not be used to bypass this
condition. Externally selected video streams remain externally owned.

The WebView2 page is mapped only to `https://matter-camera.local/receiver.html`.
This sample intentionally uses no ICE servers. The SDK's typed ICE-server input
is for external receivers that explicitly configure matching servers and trust;
it does not silently enable TURN or remote access in this receiver.
The bridge validates that origin and a random per-session token, bounds message
size and queue capacity, and denies other navigation, popups, downloads, and
permissions except the explicit, gated microphone request. Fabric credentials and native controller pointers never enter web
content. The peer connection intersects the selected camera stream codec with advertised
receiver video codecs before generating an offer, uses no ICE servers, and a
receive-only video transceiver. Audio is receive-only for listening, and sendrecv
only for the explicitly selected supported talk mode; capture still starts off.
Recording is not provided.
Successful signaling and a received media track are not playback: playback
requires a decoded video frame, a matching inbound codec from WebRTC stats,
and decoded dimensions inside the allocated bounds.
Unsupported/mismatched codecs or dimensions fail explicitly. Missing decoded
video fails after 30 seconds; decoded-frame progress stopping for ten seconds
also fails. Audio independently requires decoded, nonconcealed samples and
local playback-clock progress; decoded audio stalls are bounded as well.
Advertised software/hardware privacy attributes are subscribed, with one-second
polling as a fail-closed backstop. Reports terminate media promptly when received;
network/report latency is not an instantaneous remote guarantee. Malformed reports,
subscription failures and privacy-read failures stop media. Stop and capture
revocation bypass the browser's serialized signaling queue.
Start failures show structured stage/category, native status
when available, HRESULT, and retryability.
Discovery, candidate-send, and remote End failures use the same structured diagnostics.
The host closes the native session when stopping, closing the window, or
receiving terminal failure.

```powershell
node --test .\tools\test-camera-receiver.cjs
```

These tests simulate peer connections, permissions and media statistics, and
the NativeAOT tests exercise the production profile/ownership policies. They do
not prove physical audibility or permission-dialog behavior. Validate video,
audible camera-to-client listening, permitted push-to-talk audible at the camera,
no capture after release/backgrounding/reconnect/privacy, and empty owned
allocations after Stop on representative hardware. At least microphone-only and
full-duplex devices are needed; half-duplex remains explicitly unsupported.
Do not reset/re-pair devices or use externally owned allocations as cleanup targets.

The component is a development preview. It uses file-backed operational keys
and permits test device attestation by default.
