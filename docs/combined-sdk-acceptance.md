# Combined camera and commissioning SDK

## Scope and dependency

Branch: `windows-camera-commissioning-compatible`, based on camera commit
`455b62d23e263467214475aba32691cd7edba21c`. Structured commissioning comes from
`da89940b62b74101f68a200a1511fbdd9abf8364` and follow-ups through `11e38c2`,
including the native interface-fallback dependency and consumer compile fix.
This is an integration reference, not proof of compatibility with preview.14.

The declared native submodule dependency is
`0a3128f2fc3b36b152de3cf23ba8137c8925d973` in
`richiemsft/matterforwindows`. This replaces the camera topic's
`3e4936e176f380de0f098284a3edca9c94945447` pin. Local builds use the clean sibling
checkout at that exact declared revision with `-MatterSdkRoot`; this is not a
different-revision development override. Fresh checkouts must initialize the
submodule and follow its Windows environment setup before building.

```powershell
git submodule update --init --recursive
.\tools\build.ps1 -Architecture all -BuildCameraSignaling
```

Package `0.2.0-camera-commissioning.1` includes matched x64/ARM64 DLLs, WinMD,
projection/build assets, activation metadata, and build provenance. Versions
cannot be overwritten. `build-info.json` records SDK/native commits, native pin,
dirty-source and override indicators, toolchain and DLL/WinMD/library hashes.
The adjacent `.nupkg.sha256` records the package hash. Any dirty-source or
different-revision build is development evidence, not a release build.

## Acceptance commands

```powershell
.\tools\test-package-consumers.ps1 -PackageVersion 0.2.0-camera-commissioning.1 `
    -OutputDirectory .\out\combined-consumer-x64 -NativeAot -Run
.\tools\test-package-consumers.ps1 -PackageVersion 0.2.0-camera-commissioning.1 `
    -OutputDirectory .\out\combined-consumer-arm64 -Architecture arm64 -NativeAot
.\tools\test-combined-compatibility.ps1 `
    -BaselinePackage <actual-published-preview.14.nupkg> `
    -CombinedPackage .\artifacts\Matter.Windows.Controller.0.2.0-camera-commissioning.1.nupkg `
    -OutputDirectory .\out\preview14-acceptance -MatterSdkRoot .\matterforwindows -Run
```

Each package-consumer run requires a fresh intermediate/output directory.
BaselineConsumer retains the generic, simple and structured commissioning
compile surface; CombinedConsumer adds camera APIs without excluding pairing.
Both use one package reference, not direct WinMD/DLL overrides. The combined
fixture activates commissioning and camera wrappers on the same controller.
Closed-controller failures are negative tests, not successful device pairing.
The fixture's temporary storage does not prove compatibility with a real
consumer's existing fabric.

The strict baseline gate requires package identity/version
`Matter.Windows.Controller/0.1.0-preview.14`. It persists hashes, decompiled
metadata and comparison results, including interface UUIDs and ordered methods.
The older preview.10 diagnostic is explicitly provisional.

## Validation matrix

| Gate | Status | Evidence or limitation |
| --- | --- | --- |
| Actual preview.14 API/ABI | BLOCKED | Package not found in local caches/checkouts, NuGet mirror, or the checked GitHub releases. Exact published WinMD is required; do not regenerate a substitute. |
| Preview.10 diagnostic | PASS, provisional | All baseline declarations preserved; x64/ARM64 WinMD equivalent; activation exports unchanged. Does not establish preview.14 compatibility. |
| x64 and ARM64 component builds | PASS | MSVC 14.51.36231, Windows SDK 10.0.26100.0, declared native revision. ARM64 component is build-only evidence. |
| Camera discovery/TLV/capability/ownership | PASS, x64 execution | Includes raw malformed capabilities, zero IDs/reference counts, explicit media selection, correlated refusals and ownership cleanup. |
| Commissioning evidence regression | PASS, x64 execution | Progress cannot synthesize completion; failed stage does not overwrite last completed stage; native error/stage survive failure. |
| Receiver tests | PASS, 42 synthetic tests | Permission denial/revocation, late capture cleanup, bounded sender waits, routing and redacted evidence. Not physical sound acceptance. |
| NuGet baseline/combined consumers | Pending | Build and activation runs must use the uniquely versioned package. |
| NativeAOT and installed deployment | Pending | Distinguish executed x64 and build-only ARM64. |
| Real native-host/WebView2 loopback | Pending | Synthetic media test through actual host; not a physical camera test. |
| Automatic/Prefer/Require real commissioning | UNTESTED | No BLE target/credentials or Ethernet-plus-Wi-Fi test topology supplied. Negative interface validation and enumeration are not successful routing evidence. |
| Wi-Fi/Thread progress, timeout/cancellation | PARTIAL | Source and negative regressions only; hardware operation matrix remains open. |
| Existing paired-node reads/subscriptions/commands and storage | UNTESTED | Requires real consumer storage and paired device; no reset/re-pairing performed or required by this additive design. |
| Physical G350 video/listening | Earlier evidence only | Worked in prior validation; not rerun against this combined package. |
| 24-bit Opus allocation/release | Earlier evidence only | Previously succeeded; synthetic source-depth cases retained. |
| PC-to-camera speaker playback | FAILED/UNVERIFIED, prior hardware evidence | Capture-on and outgoing RTP do not prove remote audibility. Combining API surfaces does not fix talkback. |

The related `matter-camera-audio-spec.md` was not found in the available
checkouts. No physical acceptance or certification claim is inferred from
synthetic tests.

## Migration

Replace the application's single `Matter.Windows.Controller` package reference
with the combined development version; remove direct metadata/DLL overrides.
Rebuild in fresh x64/ARM64 intermediate directories, ensuring the deployed
native DLL comes from the same package as the projection. For unpackaged apps,
merge the package's `winmd\Matter.Windows.Controller.manifest` activation
entries into the application's manifest; packaged apps must register the same
activatable classes in their MSIX.

Keep the existing controller, storage path, fabric and commissioning
implementation. Construct `MatterControllerCommissioning`,
`MatterControllerCameraDiscovery` and `MatterControllerCamera` over that
controller. Camera signaling remains opt-in through
`ControllerOptions.EnableCameraSignaling`; ordinary pairing defaults are
unchanged. Do not load another SDK/controller or reset/re-pair devices solely
for this update.

Interface selection is passed to the commissioning operation. Automatic uses
normal routing; Prefer permits the established native fallback; Require rejects
an unavailable/disconnected/non-IPv6/non-multicast interface without fallback.
Enumeration retains connection, multicast, IPv6 and virtual-interface
information; virtual classification follows the established name heuristic,
not an exhaustive Windows driver classification. Selection cannot overcome
actual network multicast/IPv6 restrictions. No authentication, privacy,
microphone permission, remote mute or credential-policy defaults change.
