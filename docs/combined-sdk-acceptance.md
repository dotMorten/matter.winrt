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
The package builder runs each native/component build in a separate PowerShell
process so repeated x64/ARM64 MSVC setup cannot accumulate tool paths past
`cmd.exe`'s command-line limit.

```powershell
git submodule update --init --recursive
.\tools\build.ps1 -Architecture all -BuildCameraSignaling
```

Package `0.2.0-camera-commissioning.2` includes matched x64/ARM64 DLLs, WinMD,
projection/build assets, activation metadata, and build provenance. Versions
cannot be overwritten. `build-info.json` records SDK/native commits, native pin,
dirty-source and override indicators, toolchain and DLL/WinMD/library hashes.
New builds also record `nativeSourceDirty`, including native submodule changes;
`sourceDirty` includes either checkout, and dirty native sources count as a
development override even if their HEAD matches the declared pin.
The adjacent `.nupkg.sha256` records the package hash. Any dirty-source or
different-revision build is development evidence, not a release build.
Version `.1` is superseded: its provenance repeated architecture entries.
It is retained unchanged and must not be used as the acceptance artifact.

The package was built from clean SDK commit
`10f17e29476c938826430e66ddd2b9fb14f1f817`. Subsequent branch changes add consumer
activation/isolation tests and migration evidence, not different component
metadata or DLLs. The package is published to the local `artifacts` NuGet feed;
no remote release was published while preview.14 and hardware gates remain open.

| Artifact | SHA256 |
| --- | --- |
| `Matter.Windows.Controller.0.2.0-camera-commissioning.2.nupkg` | `919859F9BBA188886BFE900EBBB27DC707768D7C6098C7D638163334D0EEFAA1` |
| x64 DLL | `BDB5079E1E45B3FA9BEA84201BC2473D88B6036C07ECE93C9DCCEDB06444B6BB` |
| ARM64 DLL | `2980B993F918386352472582D7578D377E4AA6DAA54F033BAA5FFCC7BA18E6B3` |
| x64/reference WinMD | `5F4C501A2DC4DA8C4A611B4A0E77AE0C245C73AF60C463595A18C1FBC91BF8DC` |
| ARM64 WinMD | `786D793690E46E02805EEE49EBC6B579E9001B17DFFA3753C37EE89BB717821E` |

Architecture WinMD bytes can differ; their ordered interface/activation
contracts were compared in both directions before packaging. Each architecture's
exact WinMD is also included beside its DLL under `runtimes`.

## Standard development preview

The next package is `Matter.Windows.Controller/0.2.0-preview.1`. The Windows
workflow builds its matched x64/ARM64 assets, runs package-consumer and adopted
baseline checks, and uploads the `.nupkg` and `.nupkg.sha256` together as the
`Matter.Windows.Controller.0.2.0-preview.1` artifact. Download and extract
these files into the repository's `artifacts` directory to use the local
NuGet source. There is no automatic publication to nuget.org.

The hashes and validation matrix above describe the historical
`0.2.0-camera-commissioning.2` artifact, not the new preview. Read the new
package's checksum and `build-info.json` for its actual hashes and revisions.
A green ordinary CI run is not preview.14 or physical-device acceptance;
those outstanding gates remain explicit below.

The preview also retains pending-session cleanup ownership for video-only
startup cancellation and counts serialized operation-queue waits toward read
deadlines and cancellation. Native regressions cover late once-only session
cleanup and blocked read admission, without claiming real-device cleanup.

## Acceptance commands

```powershell
.\tools\test-package-consumers.ps1 -PackageVersion 0.2.0-preview.1 `
    -OutputDirectory .\out\combined-consumer-x64 -MatterSdkRoot .\matterforwindows -NativeAot -Run
.\tools\test-package-consumers.ps1 -PackageVersion 0.2.0-preview.1 `
    -OutputDirectory .\out\combined-consumer-arm64 -MatterSdkRoot .\matterforwindows -Architecture arm64 -NativeAot
.\tools\test-combined-compatibility.ps1 `
    -BaselinePackage <actual-published-preview.14.nupkg> `
    -CombinedPackage .\artifacts\Matter.Windows.Controller.0.2.0-preview.1.nupkg `
    -OutputDirectory .\out\preview14-acceptance -MatterSdkRoot .\matterforwindows -Run
.\tools\test-installed-consumer.ps1 `
    -PublishedConsumer .\out\combined-consumer-x64\CombinedConsumer\publish `
    -OutputDirectory .\out\combined-installed-package
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
CI runs package consumers for both architectures. Strict preview.14 acceptance
requires a workflow-dispatch baseline URL and SHA256; without them the workflow
explicitly records release acceptance as blocked. Older CI baselines are not
promoted to preview.14 evidence.

## Validation matrix

| Gate | Status | Evidence or limitation |
| --- | --- | --- |
| Actual preview.14 API/ABI | BLOCKED | Package not found in local caches/checkouts, NuGet mirror, or the checked GitHub releases. Exact published WinMD is required; do not regenerate a substitute. |
| Preview.10 diagnostic | PASS, provisional | 82 baseline declarations preserved against 143 current declarations; x64/ARM64 WinMD equivalent; activation exports unchanged. Persistent output: `artifacts\combined-preview10-provisional`. Does not establish preview.14 compatibility. |
| x64 and ARM64 component builds | PASS | MSVC toolset 14.51.36231/compiler 19.51.36257.0, Windows SDK 10.0.26100.0, declared native revision. ARM64 is build-only evidence. |
| Camera discovery/TLV/capability/ownership | PASS, x64 execution | Includes raw malformed capabilities, zero IDs/reference counts, explicit media selection, correlated refusals and ownership cleanup. |
| Commissioning evidence regression | PASS, x64 execution | Progress cannot synthesize completion; failed stage does not overwrite last completed stage; native error/stage survive failure. |
| Receiver tests | PASS, 42 synthetic tests | Permission denial/revocation, late capture cleanup, bounded sender waits, routing and redacted evidence. Not physical sound acceptance. |
| Actual OS microphone consent/revocation | UNTESTED | No live microphone consent session was opened. Synthetic denial/revocation tests are not an OS permission acceptance claim. |
| NuGet baseline/combined consumers | PASS | Original camera.22 package reproduced missing commissioning types. Combined package passes with exact consumed WinMD/DLL hash checks and isolated projections. x64 executed; ARM64 built only. |
| NativeAOT | PASS | Baseline and combined x64 fixtures executed; ARM64 fixtures compiled only. Camera capability/profile/ownership NativeAOT regression fixture also executed on x64. |
| Installed deployment | PASS, x64 development registration | Combined consumer ran with the expected installed Windows package identity and native camera/commissioning activation. Test registration removed afterward. Signed production deployment and ARM64 installation remain untested. |
| Real native-host/WebView2 loopback | PASS, x64 synthetic media | Video-only, listening and synthetic outgoing audio traversed production host routes and cleaned up tracks/peers. No microphone permission request; physical audibility explicitly false. Result: `out\combined-webview-final\result.json`. |
| Repository sample host builds and MSIX | PASS | x64/ARM64 WinUI host builds retain real structured commissioning and add the camera backend. x64 MSIX validates all camera and commissioning activation entries and the package-matched DLL. One pre-existing toolchain warning: no `mspdbcmf.exe` symbols package. |
| External product host integration | UNTESTED | No external host checkout/build command was supplied; sample acceptance does not establish another product's build or device behavior. |
| Interface enumeration and negative modes | PASS, x64 execution | Typed enumeration, enum values and constructors; Automatic/Prefer failures on a closed controller; Require rejects an unavailable interface before dispatch. Not successful operational routing evidence. |
| Automatic/Prefer/Require real commissioning | UNTESTED | No BLE target/credentials or Ethernet-plus-Wi-Fi test topology supplied. Negative interface validation and enumeration are not successful routing evidence. |
| Wi-Fi/Thread progress, timeout/cancellation | PARTIAL | Both credential types activate; native evidence and cancellation-before-dispatch regressions pass. Real provisioning, in-flight cancellation and operational timeouts remain hardware gates. |
| Full preview.14 commissioning stage/result semantics | UNVERIFIED | Reference implementation groups operational discovery/CASE in native stages; exact published contract and subsequent fixes must be checked before claiming complete stage behavior. |
| Existing paired-node reads/subscriptions/commands and storage | UNTESTED | Requires real consumer storage and paired device; no reset/re-pairing performed or required by this additive design. |
| Physical G350 video/listening | Earlier evidence only | Worked in prior validation; not rerun against this combined package. |
| 24-bit Opus allocation/release | Earlier evidence only | Previously succeeded; synthetic source-depth cases retained. |
| PC-to-camera speaker playback | FAILED/UNVERIFIED, prior hardware evidence | Capture-on and outgoing RTP do not prove remote audibility. Combining API surfaces does not fix talkback. |

The related `matter-camera-audio-spec.md` was not found in the available
checkouts. No physical acceptance or certification claim is inferred from
synthetic tests.
Machine-readable summary and copied loopback/installed results are in
`artifacts\combined-sdk-validation`.

## Migration

Replace the application's single `Matter.Windows.Controller` package reference
with `0.2.0-preview.1` from the local feed; remove direct metadata/DLL overrides.
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
