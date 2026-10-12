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
    -p:MatterControllerPackageVersion=0.2.0-camera.13 -p:MatterCameraDevelopment=true
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

Launch the resulting executable with `--camera`, select a commissioned device,
and open **Camera receiver (development)**. Discover its camera endpoints, read
allocated video streams, and choose an **advertised allocated LiveView stream**.
The receiver reads advertised cluster revisions, features, and command lists
before offering a stream selection, and refuses incomplete video/client-offer
capabilities. The SDK refreshes these checks again at start.
Allocation parameters,
camera/receiver codec compatibility, and later deallocation belong to the
caller; the receiver does not invent stream IDs or free externally owned
streams. Explicitly stop the receiver before deallocating its stream.

The WebView2 page is mapped only to `https://matter-camera.local/receiver.html`.
This sample intentionally uses no ICE servers. The SDK's typed ICE-server input
is for external receivers that explicitly configure matching servers and trust;
it does not silently enable TURN or remote access in this receiver.
The bridge validates that origin and a random per-session token, bounds message
size and queue capacity, and denies other navigation, popups, downloads, and
permissions. Fabric credentials and native controller pointers never enter web
content. The peer connection intersects the selected camera stream codec with advertised
receiver video codecs before generating an offer, uses no ICE servers, and a
receive-only video transceiver; microphone capture and recording are disabled.
Successful signaling and a received media track are not playback: playback
requires a decoded video frame and a matching inbound codec from WebRTC stats.
Unsupported or mismatched codecs fail explicitly. Missing decoded video fails
after 30 seconds. Start failures show structured stage/category, native status
when available, HRESULT, and retryability.
Discovery, candidate-send, and remote End failures use the same structured diagnostics.
The host closes the native session when stopping, closing the window, or
receiving terminal failure.

```powershell
node --test .\tools\test-camera-receiver.cjs
```

These tests simulate peer connections. Real camera playback and interoperation
have not been established; the sample is not evidence of release readiness.

The component is a development preview. It uses file-backed operational keys
and permits test device attestation by default.
