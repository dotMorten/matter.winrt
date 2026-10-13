using Matter.Windows.Controller;
using System.Runtime.InteropServices;

internal static class Program
{
    [DllImport("CameraNativeAotFixture.dll", ExactSpelling = true)]
    private static extern int GetCameraDescriptor(out IntPtr value);
    [DllImport("CameraNativeAotFixture.dll", ExactSpelling = true)]
    private static extern int GetCameraVideoStream(out IntPtr value);
    [DllImport("CameraNativeAotFixture.dll", ExactSpelling = true)]
    private static extern int GetCameraSignal(int variant, out IntPtr value);
    [DllImport("CameraNativeAotFixture.dll", ExactSpelling = true)]
    private static extern int GetCameraOperationResult(int variant, out IntPtr value);
    [DllImport("CameraNativeAotFixture.dll", ExactSpelling = true)]
    private static extern int GetCameraDiscoveryResult(int variant, out IntPtr value);
    [DllImport("CameraNativeAotFixture.dll", ExactSpelling = true)]
    private static extern int GetCameraCapabilities(out IntPtr value);
    [DllImport("CameraNativeAotFixture.dll", ExactSpelling = true)]
    private static extern int GetRetainedCameraSession(IntPtr controller, out IntPtr value);
    [DllImport("CameraNativeAotFixture.dll", ExactSpelling = true)]
    private static extern int GetCameraVideoStreamAllocationResult(int variant, out IntPtr value);
    [DllImport("CameraNativeAotFixture.dll", ExactSpelling = true)]
    private static extern int GetCameraAudioFixture(int variant, out IntPtr value);

    private static async Task Main()
    {
        var absent = new MatterCameraStreamSelection(MatterCameraStreamSelectionKind.Absent, null);
        var automatic = new MatterCameraStreamSelection(MatterCameraStreamSelectionKind.AutoSelect, null);
        var explicitZero = new MatterCameraStreamSelection(MatterCameraStreamSelectionKind.ExplicitId, 0);
        Require(absent.Kind == MatterCameraStreamSelectionKind.Absent && absent.StreamId is null &&
            automatic.Kind == MatterCameraStreamSelectionKind.AutoSelect && automatic.StreamId is null &&
            explicitZero.StreamId == 0, "NativeAOT tagged selections and nullable zero ID");
        var audioOptions = new MatterCameraAudioStreamOptions { AudioCodec = 0, Channels = 2, SampleRate = 48000, BitRate = 64000, BitDepth = 16 };
        Require(audioOptions.AudioCodec == 0 && audioOptions.Channels == 2 && audioOptions.SampleRate == 48000 &&
            audioOptions.BitRate == 64000 && audioOptions.BitDepth == 16, "NativeAOT audio options activation/setters");
        Marshal.ThrowExceptionForHR(GetCameraAudioFixture(0, out var audioPointer));
        MatterCameraAudioStream audioStream;
        try { audioStream = WinRT.MarshalInspectable<MatterCameraAudioStream>.FromAbi(audioPointer); }
        finally { Marshal.Release(audioPointer); }
        Require(audioStream.StreamId == 0 && audioStream.IsLiveView && audioStream.AudioCodec == 0 &&
            audioStream.Channels == 2 && audioStream.SampleRate == 48000 && audioStream.BitRate == 64000 &&
            audioStream.BitDepth == 16 && audioStream.ReferenceCount == 0, "NativeAOT audio stream numeric fields/count zero");
        Marshal.ThrowExceptionForHR(GetCameraAudioFixture(1, out audioPointer));
        MatterCameraAudioCapabilities audioCapabilities;
        try { audioCapabilities = WinRT.MarshalInspectable<MatterCameraAudioCapabilities>.FromAbi(audioPointer); }
        finally { Marshal.Release(audioPointer); }
        Require(audioCapabilities.Microphone.MaximumChannels == 2 &&
            audioCapabilities.Microphone.Codecs.SequenceEqual(new byte[] { 0, 255 }) &&
            audioCapabilities.Microphone.SampleRates.SequenceEqual(new uint[] { 16000, 48000 }) &&
            audioCapabilities.Microphone.BitDepths.SequenceEqual(new byte[] { 16, 24 }) &&
            audioCapabilities.MicrophoneMuted == false && audioCapabilities.MicrophoneVolume == 20 &&
            audioCapabilities.Speaker is null && audioCapabilities.SpeakerMuted is null &&
            audioCapabilities.TwoWayTalkSupport is null && audioCapabilities.MaximumNetworkBandwidth == 128000,
            "NativeAOT nested audio collections and absent remote controls");
        for (int variant = 2; variant <= 3; ++variant)
        {
            Marshal.ThrowExceptionForHR(GetCameraAudioFixture(variant, out audioPointer));
            MatterCameraAudioStreamAllocationResult audioResult;
            try { audioResult = WinRT.MarshalInspectable<MatterCameraAudioStreamAllocationResult>.FromAbi(audioPointer); }
            finally { Marshal.Release(audioPointer); }
            Require(variant == 2 ? audioResult.Succeeded && audioResult.Stream.ReferenceCount == 0 && audioResult.Failure is null
                : !audioResult.Succeeded && audioResult.Stream is null &&
                  audioResult.Failure.Category == MatterCameraFailureCategory.DynamicConstraintError &&
                  audioResult.Failure.NativeError == 0x5cf && !audioResult.Failure.Retryable,
                "NativeAOT typed audio allocation success/rejection");
        }
        var streamOptions = new MatterCameraVideoStreamOptions();
        Require(streamOptions.MinimumWidth == 640 && streamOptions.MaximumWidth == 640 &&
            streamOptions.MinimumHeight == 480 && streamOptions.MaximumHeight == 480 &&
            streamOptions.MinimumFrameRate == 30 && streamOptions.MaximumFrameRate == 30 &&
            streamOptions.MinimumBitRate == 500000 && streamOptions.MaximumBitRate == 2000000 &&
            streamOptions.KeyFrameInterval == 1000, "NativeAOT video allocation options activation");
        streamOptions.MaximumWidth = 1280;
        Require(streamOptions.MaximumWidth == 1280, "NativeAOT typed allocation setters");
        MatterCameraFailureCategory[] allocationCategories =
        [
            MatterCameraFailureCategory.Timeout, MatterCameraFailureCategory.DynamicConstraintError,
            MatterCameraFailureCategory.ConstraintError, MatterCameraFailureCategory.CommandRejected,
            MatterCameraFailureCategory.DynamicConstraintError, MatterCameraFailureCategory.MalformedData,
            MatterCameraFailureCategory.ConnectionFailure, MatterCameraFailureCategory.Unknown
        ];
        uint[] rejectionCodes = [0x5cf, 0x587, 0x585];
        for (int variant = 0; variant < 9; ++variant)
        {
            Marshal.ThrowExceptionForHR(GetCameraVideoStreamAllocationResult(variant, out var allocationPointer));
            MatterCameraVideoStreamAllocationResult allocation;
            try
            {
                allocation = WinRT.MarshalInspectable<MatterCameraVideoStreamAllocationResult>.FromAbi(allocationPointer);
            }
            finally
            {
                Marshal.Release(allocationPointer);
            }
            Require(allocation.Succeeded == (variant == 0), "Allocation result success discrimination");
            Require(variant == 0 ? allocation.Stream.StreamId == 7 && allocation.Stream.IsLiveView &&
                    allocation.Stream.ReferenceCount == 0 && allocation.Failure is null
                : allocation.Stream is null && allocation.Failure.Stage ==
                    (variant == 5 ? MatterCameraStage.StreamDeallocation : MatterCameraStage.StreamAllocation) &&
                    allocation.Failure.Category == allocationCategories[variant - 1] && allocation.Failure.HasNativeError,
                "NativeAOT unused allocated stream or exact failure");
            if (variant > 0)
            {
                Require(MatterControllerApp.Camera.CameraAllocationOwnership.IsOutcomeUncertain(allocation.Failure) ==
                    (variant == 1 || variant >= 5),
                    "Receiver ownership lockout distinguishes refusal from timeout, malformed response, connection loss and failed release");
                if (variant is >= 2 and <= 4)
                {
                    Require(allocation.Failure.NativeError == rejectionCodes[variant - 2] && !allocation.Failure.Retryable,
                        "NativeAOT explicit IM allocation rejection preserves exact status without automatic retry");
                }
            }
        }
        Marshal.ThrowExceptionForHR(GetCameraDescriptor(out var pointer));
        MatterEndpointDescriptor descriptor;
        try
        {
            descriptor = WinRT.MarshalInspectable<MatterEndpointDescriptor>.FromAbi(pointer);
        }
        finally
        {
            Marshal.Release(pointer);
        }
        Require(descriptor.EndpointId == 2 && descriptor.HasCameraClusters, "Camera descriptor identity");
        Require(descriptor.PartsList.SequenceEqual(new ushort[] { 3, 4 }), "Numeric descriptor collection");
        Require(descriptor.ServerClusters.Count == 3 && descriptor.ClientClusters.Count == 0, "Server and empty client lists");
        Require(descriptor.DeviceTypes.Count == 1 && descriptor.DeviceTypes[0].DeviceTypeId == 0x0140 &&
            descriptor.DeviceTypes[0].Revision == 1, "Structured descriptor collection");
        Marshal.ThrowExceptionForHR(GetCameraVideoStream(out pointer));
        MatterCameraVideoStream stream;
        try
        {
            stream = WinRT.MarshalInspectable<MatterCameraVideoStream>.FromAbi(pointer);
        }
        finally
        {
            Marshal.Release(pointer);
        }
        Require(stream.StreamId == 7 && stream.VideoCodec == 0 && stream.MinimumWidth == 640 &&
            stream.MaximumHeight == 1080 && stream.MaximumBitRate == 4000000, "Typed allocated video stream");
        Require(stream.WatermarkEnabled == false && stream.OnScreenDisplayEnabled is null, "Optional stream booleans");
        Marshal.ThrowExceptionForHR(GetCameraCapabilities(out pointer));
        MatterCameraCapabilities capabilities;
        try
        {
            capabilities = WinRT.MarshalInspectable<MatterCameraCapabilities>.FromAbi(pointer);
        }
        finally
        {
            Marshal.Release(pointer);
        }
        Require(capabilities.SupportsVideo && capabilities.SupportsClientOffer &&
            capabilities.Descriptor.EndpointId == 2 && capabilities.StreamManagement.Revision == 2 &&
            capabilities.StreamManagement.FeatureMap == 0x80000002 &&
            capabilities.StreamManagement.Attributes.SequenceEqual(new uint[] { 1, 2 }) &&
            capabilities.TransportProvider.AcceptedCommands.Count == 3 &&
            capabilities.TransportProvider.GeneratedCommands.Count == 1, "NativeAOT advertised cluster capabilities");
        ushort?[] protocolIds = [null, 0, 42];
        for (int i = 0; i < protocolIds.Length; ++i)
        {
            Marshal.ThrowExceptionForHR(GetCameraSignal(i, out pointer));
            MatterCameraSignal signal;
            try
            {
                signal = WinRT.MarshalInspectable<MatterCameraSignal>.FromAbi(pointer);
            }
            finally
            {
                Marshal.Release(pointer);
            }
            Require(signal.SessionHandle == 5 && signal.Kind == MatterCameraSignalKind.Failure &&
                signal.ProtocolSessionId == protocolIds[i], "Absent, zero, and nonzero protocol identities");
        }
        for (int i = 0; i < 3; ++i)
        {
            Marshal.ThrowExceptionForHR(GetCameraDiscoveryResult(i, out pointer));
            MatterCameraDiscoveryResult result;
            try
            {
                result = WinRT.MarshalInspectable<MatterCameraDiscoveryResult>.FromAbi(pointer);
            }
            finally
            {
                Marshal.Release(pointer);
            }
            Require(result.Succeeded == (i != 2), "Discovery success discrimination");
            if (i == 2)
            {
                Require(result.Cameras is null && result.Failure.Stage == MatterCameraStage.Discovery &&
                    result.Failure.Category == MatterCameraFailureCategory.AccessDenied && result.Failure.HasNativeError,
                    "Access-denied discovery is not an empty camera list");
            }
            else
            {
                Require(result.Failure is null && result.Cameras.Count == (i == 0 ? 0 : 2),
                    "Empty and multiple typed discovery collections");
                if (i == 1)
                {
                    Require(result.Cameras.Select(camera => camera.EndpointId).SequenceEqual(new ushort[] { 2, 3 }) &&
                        result.Cameras.All(camera => camera.DeviceTypes[0].DeviceTypeId == 0x0140),
                        "NativeAOT discovery collection and structured device types");
                }
            }
        }
        var candidate = new MatterCameraIceCandidate("candidate:typed", "video", 0);
        var iceServer = new MatterCameraIceServer(new[] { "stun:lan.invalid:3478" }, true, "", true, "test-only-credential", 0);
        Require(iceServer.Urls.SequenceEqual(new[] { "stun:lan.invalid:3478" }) &&
            iceServer.HasUsername && iceServer.Username == "" && iceServer.HasCredential &&
            iceServer.CertificateAuthorityId == 0, "NativeAOT typed ICE server activation and optional presence");
        var absentServer = new MatterCameraIceServer(new[] { "stun:lan.invalid" }, false, "", false, "", null);
        Require(!absentServer.HasUsername && !absentServer.HasCredential && absentServer.CertificateAuthorityId is null,
            "NativeAOT absent ICE server metadata");
        for (int i = 0; i < 3; ++i)
        {
            Marshal.ThrowExceptionForHR(GetCameraOperationResult(i, out pointer));
            MatterCameraOperationResult result;
            try
            {
                result = WinRT.MarshalInspectable<MatterCameraOperationResult>.FromAbi(pointer);
            }
            finally
            {
                Marshal.Release(pointer);
            }
            Require(result.Succeeded == (i == 0), "Typed operation success discrimination");
            if (i == 0)
            {
                Require(result.Failure is null, "Successful operation has no failure");
            }
            else
            {
                Require(result.Failure.Stage == (i == 1 ? MatterCameraStage.IceCandidates : MatterCameraStage.SessionEnd) &&
                    result.Failure.HasNativeError && result.Failure.NativeError != 0 &&
                    result.Failure.Category == MatterCameraFailureCategory.Timeout && result.Failure.Retryable,
                    "Typed ICE and End failure diagnostics");
            }
        }
        Require(candidate.Candidate == "candidate:typed" && candidate.LineIndex == 0, "Typed candidate activation");
        var storage = Path.Combine(Path.GetTempPath(), $"matter-camera-aot-{Guid.NewGuid():N}");
        try
        {
            for (int i = 0; i < 2; ++i)
            {
                var options = new ControllerOptions { StoragePath = storage };
                Require(!options.EnableCameraSignaling, "Default camera opt-out");
                options.EnableCameraSignaling = true;
                var controller = await MatterController.CreateAsync(options);
                var controllerPointer = WinRT.MarshalInspectable<MatterController>.FromManaged(controller);
                MatterCameraSession retainedSession;
                try
                {
                    Marshal.ThrowExceptionForHR(GetRetainedCameraSession(controllerPointer, out pointer));
                    try
                    {
                        retainedSession = WinRT.MarshalInspectable<MatterCameraSession>.FromAbi(pointer);
                    }
                    finally
                    {
                        Marshal.Release(pointer);
                    }
                }
                finally
                {
                    Marshal.Release(controllerPointer);
                }
                Require(await retainedSession.GetStateAsync() == MatterCameraSessionState.Closed,
                    "A released session reports Closed");
                Require(retainedSession.RequestedVideoSelection.Kind == MatterCameraStreamSelectionKind.Absent &&
                    retainedSession.RequestedAudioSelection.Kind == MatterCameraStreamSelectionKind.AutoSelect &&
                    retainedSession.VideoStreamId is null && retainedSession.AudioStreamId == 0 &&
                    retainedSession.ProtocolSessionId == 0, "Versioned session metadata preserves requested selections and resolved/protocol zero IDs");
                var invalidCandidates = await retainedSession.TrySendCandidatesAsync(Array.Empty<MatterCameraIceCandidate>());
                Require(!invalidCandidates.Succeeded && invalidCandidates.Failure.Stage == MatterCameraStage.Validation &&
                    invalidCandidates.Failure.Category == MatterCameraFailureCategory.InvalidInput,
                    "NativeAOT candidate validation result");
                var ended = await retainedSession.TryCloseAsync();
                Require(ended.Succeeded && ended.Failure is null, "NativeAOT idempotent typed close");
                var discovery = new MatterControllerCameraDiscovery(controller);
                var invalidDiscovery = await discovery.TryDiscoverCamerasAsync(0);
                Require(!invalidDiscovery.Succeeded && invalidDiscovery.Cameras is null &&
                    invalidDiscovery.Failure.Stage == MatterCameraStage.Validation &&
                    invalidDiscovery.Failure.Category == MatterCameraFailureCategory.InvalidInput,
                    "NativeAOT discovery failure is not an empty success");
                var camera = new MatterControllerCamera(controller);
                var invalidAudio = await camera.TryAllocateAudioStreamAsync(1, 2, new MatterCameraAudioStreamOptions());
                Require(!invalidAudio.Succeeded && invalidAudio.Stream is null &&
                    invalidAudio.Failure.Stage == MatterCameraStage.Validation &&
                    invalidAudio.Failure.Category == MatterCameraFailureCategory.InvalidInput,
                    "Versioned audio API does not guess an unnegotiated format");
                var unownedAudio = await camera.TryDeallocateAudioStreamAsync(1, 2, 0);
                Require(!unownedAudio.Succeeded && unownedAudio.Failure.Category == MatterCameraFailureCategory.InvalidState &&
                    unownedAudio.Failure.Stage == MatterCameraStage.StreamDeallocation,
                    "SDK refuses remote deallocation of externally selected streams");
                var invalidSelection = await camera.TryStartSessionAsync(1, 2, absent, absent, "v=0\r\n", Array.Empty<MatterCameraIceServer>());
                Require(!invalidSelection.Succeeded && invalidSelection.Session is null &&
                    invalidSelection.Failure.Stage == MatterCameraStage.Validation &&
                    invalidSelection.Failure.Category == MatterCameraFailureCategory.InvalidInput,
                    "New session API rejects both media absent before dispatch");
                bool invalidPrivacyRejected = false;
                try
                {
                    await camera.IsPrivacyEnabledAsync(0, 2);
                }
                catch (Exception error) when (error.HResult == unchecked((int)0x80070057))
                {
                    invalidPrivacyRejected = true;
                }
                Require(invalidPrivacyRejected, "NativeAOT additive privacy interface rejects invalid nodes");
                var invalidAllocation = await camera.TryAllocateVideoStreamAsync(0, 2, streamOptions);
                Require(!invalidAllocation.Succeeded && invalidAllocation.Stream is null &&
                    invalidAllocation.Failure.Stage == MatterCameraStage.Validation &&
                    invalidAllocation.Failure.Category == MatterCameraFailureCategory.InvalidInput,
                    "NativeAOT additive allocation interface and validation");
                var invalidRelease = await camera.TryDeallocateVideoStreamAsync(0, 2, 7);
                Require(!invalidRelease.Succeeded &&
                    invalidRelease.Failure.Stage == MatterCameraStage.StreamDeallocation &&
                    invalidRelease.Failure.Category == MatterCameraFailureCategory.InvalidInput,
                    "NativeAOT typed deallocation validation");
                var failed = await camera.TryStartWithAllocatedVideoStreamAsync(0, 2, 1, "v=0\r\n");
                Require(!failed.Succeeded && failed.Session is null &&
                    failed.Failure.Stage == MatterCameraStage.Validation &&
                    failed.Failure.Category == MatterCameraFailureCategory.InvalidInput &&
                    !failed.Failure.HasNativeError && !failed.Failure.Retryable, "Structured NativeAOT start failure");
                await controller.CloseAsync();
                await controller.CloseAsync();
                Require(await retainedSession.GetStateAsync() == MatterCameraSessionState.Closed,
                    "Controller shutdown invalidates retained session state");
                var closedDiscovery = await discovery.TryDiscoverCamerasAsync(1);
                Require(!closedDiscovery.Succeeded && closedDiscovery.Cameras is null &&
                    closedDiscovery.Failure.Stage == MatterCameraStage.Discovery &&
                    closedDiscovery.Failure.Category == MatterCameraFailureCategory.InvalidState,
                    "NativeAOT retained discovery client cannot access closed controller");
                bool closedPrivacyRejected = false;
                try
                {
                    await camera.IsPrivacyEnabledAsync(1, 2);
                }
                catch (Exception error) when (error.HResult == unchecked((int)0x8000000e))
                {
                    closedPrivacyRejected = true;
                }
                Require(closedPrivacyRejected, "A closed controller cannot return a successful privacy snapshot");
                var closedRootAllocation = await camera.TryAllocateVideoStreamAsync(1, 0, streamOptions);
                Require(!closedRootAllocation.Succeeded &&
                    closedRootAllocation.Failure.Stage == MatterCameraStage.Descriptor &&
                    closedRootAllocation.Failure.Category == MatterCameraFailureCategory.InvalidState,
                    "Endpoint zero is valid and must reach controller-state validation");
                var closedRootRelease = await camera.TryDeallocateVideoStreamAsync(1, 0, 7);
                Require(!closedRootRelease.Succeeded &&
                    closedRootRelease.Failure.Category == MatterCameraFailureCategory.InvalidState,
                    "Deallocation must not arbitrarily reject endpoint zero");
            }
        }
        finally
        {
            if (Directory.Exists(storage))
            {
                Directory.Delete(storage, recursive: true);
            }
        }
        Console.WriteLine("PASS NativeAOT descriptors, discovery outcomes, streams, allocation options/results/rejections, receiver ownership policy, additive APIs, nullable values, ICE and End results, activation, close and reopen");
    }

    private static void Require(bool condition, string invariant)
    {
        if (!condition)
        {
            throw new InvalidOperationException(invariant);
        }
    }
}
