# Camera SDK reliability adoption

## Changes and compatibility

`0.2.0-preview.2` adopts the October 17, 2026 owner handoff against SDK base
`127d27d5da52ab7b1f0fd6b68b17b31ec0eaed97`. The native dependency remains
`0a3128f2fc3b36b152de3cf23ba8137c8925d973`. There are no public IDL changes.
Keep existing controller storage and fabric; no reset or re-pairing is needed
solely for these fixes. Do not mix private implementation objects from different
SDK builds in tests or applications.

Public generic attribute reads now forward WinRT cancellation to the native
read. Their original 30-second budget includes lifetime/node admission and
response waits. Cancellation closes the ReadClient and connection callbacks
under the CHIP stack lock before releasing the operation lease. A canceled
IAsyncInfo status alone is not proof that native work stopped.

Network operations use a shared runtime lifetime lease and an exclusive timed
node gate. This covers generic reads/events/writes/commands, subscription
establishment, typed operations and camera offer/candidate/end operations.
Different nodes can progress concurrently; the same node remains serialized.
Admission observes cancellation, shutdown and bounded deadlines at intervals
of at most 100 ms. This is not FIFO scheduling or a real-time guarantee.
The weak node-gate index removes expired entries, and each lease retains its
mutex until after unlocking it.

Shutdown refuses new/queued operations before taking the exclusive lifetime
barrier. Commissioning, node recovery's index update and removal also take that
barrier. Already-dispatched noncancelable operations keep their completion and
timeout semantics; shutdown can wait for them. Metadata and subscription lists
retain short controller locking. The lock order is lifetime, node, optional
camera-host serialization, then short controller/CHIP locks. No controller-wide
mutex covers ordinary network response waits.

Camera commands use the selected peer's gate and retain camera-host
serialization. Peer lookup and post-admission stale-handle validation cannot
authorize a replacement session. Privacy reads, absent/automatic/explicit
selection, allocated media bounds, and owned cleanup remain unchanged.

Attribute/event `CloseAsync` retains its existing background-thread execution.
Native cleanup holds a lifetime lease, but releases CHIP resources before
briefly locking the subscription list. The projected subscription stops
accepting new reports when closed; a handler already being delivered may finish.
Do not synchronously wait on asynchronous controller work inside a report
callback. Repeated native closure after shutdown is harmless.
The test-only friend is compiled only with `MATTER_WINRT_ADMISSION_TESTS`;
no shipping API or export is added.

## Allocation guidance, not a new SDK default

`MaxNetworkBandwidth` describes an advertised upper bound, not an assurance
that one stream using that entire ceiling can be allocated. A consumer may
first try a bounded bitrate candidate derived from its selected advertised
rate-distortion point while retaining negotiable, validated resolution bounds.
That point does not guarantee every resolution is encodable at its bitrate.

Only an explicit allocation `DynamicConstraintError` permits trying another
bounded, deduplicated candidate. Preserve stage, category, HRESULT and native
status. A timeout, connection failure, malformed readback, cancellation or
uncertain ownership does not permit blind allocation replay. Never adopt an
external allocation or release a stream not owned by the caller.

The handoff's paired G350 experiment accepted the advertised point candidate
and rejected the total network ceiling with `DynamicConstraintError`. Those
observations are compatibility evidence, not device-specific SDK defaults.
No bitrate, device ID, endpoint, model check, allocation retry policy, logging
scaffold or symbol-generation override from that investigation is introduced
into production here.

## Regression commands

Build the matched component and run the x64 native suite:

```powershell
.\tools\build.ps1 -Architecture all -BuildCameraSignaling
.\tools\test-camera-discovery.ps1 -Architecture x64
.\tools\test-camera-discovery.ps1 -Architecture arm64 -BuildOnly
.\tools\test-package-consumers.ps1 -PackageVersion 0.2.0-preview.2 `
    -OutputDirectory .\out\reliability-consumer-x64 -NativeAot -Run
.\tools\test-package-consumers.ps1 -PackageVersion 0.2.0-preview.2 `
    -OutputDirectory .\out\reliability-consumer-arm64 -Architecture arm64 -NativeAot
```

Use fresh package-consumer output directories. The public cancellation test
uses fresh unpaired temporary storage and an unresolved read, then checks
native release by completing controller shutdown within a two-second test
tolerance. It reopens the same test storage afterward. The same fixture
reproduced shutdown blockage against the unmodified base implementation.
All controller-bearing implementation objects in each run come from one build.

Private admission regressions cover unrelated-node progress, same-node
exclusion, deadlines, queued cancellation, repeated gate reuse, exclusive
lifetime exclusion, refusal during shutdown and waiting for active leases.
Attribute and event cleanup tests cover concurrent cleanup with an exclusive
lifetime change and repeated closure after shutdown.
Existing camera capability/TLV/ownership/failure and late-cancellation tests
remain in that suite. ARM64 native tests are build-only on an x64 host.

The handoff reports smoother physical video and multiple successful
Start/Stop/restart and close-without-Stop/reopen sequences. Counts and duration
were not recorded. These are user-reported observations of the local combined
SDK/consumer changes, not an instrumented endurance run or independent proof
that each improvement came from a particular fix. Its 381 managed consumer
tests and physical contention/allocation experiments are reported handoff
evidence, not SDK tests rerun here.

Real-camera contention, response/cancellation races on devices, media
endurance, actual microphone permission changes, ARM64 execution and physical
talkback remain separate hardware gates. In particular, synthetic outgoing
audio does not prove PC-to-camera audibility. Actual published preview.14
compatibility remains subject to the outstanding baseline gate in
[combined SDK acceptance](combined-sdk-acceptance.md).
