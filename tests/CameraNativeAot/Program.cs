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

    private static async Task Main()
    {
        var streamOptions = new MatterCameraVideoStreamOptions();
        Require(streamOptions.MinimumWidth == 640 && streamOptions.MaximumWidth == 640 &&
            streamOptions.MinimumHeight == 480 && streamOptions.MaximumHeight == 480 &&
            streamOptions.MinimumFrameRate == 30 && streamOptions.MaximumFrameRate == 30 &&
            streamOptions.MinimumBitRate == 500000 && streamOptions.MaximumBitRate == 2000000 &&
            streamOptions.KeyFrameInterval == 1000, "NativeAOT video allocation options activation");
        streamOptions.MaximumWidth = 1280;
        Require(streamOptions.MaximumWidth == 1280, "NativeAOT typed allocation setters");
        for (int variant = 0; variant < 2; ++variant)
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
            Require(variant == 0 ? allocation.Stream.StreamId == 7 && allocation.Stream.ReferenceCount == 1 && allocation.Failure is null
                : allocation.Stream is null && allocation.Failure.Stage == MatterCameraStage.StreamAllocation &&
                    allocation.Failure.Category == MatterCameraFailureCategory.Timeout && allocation.Failure.HasNativeError,
                "NativeAOT typed owned allocation or exact failure");
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
        Console.WriteLine("PASS NativeAOT descriptors, discovery outcomes, streams, allocation options/results, additive APIs, nullable values, ICE and End results, activation, close and reopen");
    }

    private static void Require(bool condition, string invariant)
    {
        if (!condition)
        {
            throw new InvalidOperationException(invariant);
        }
    }
}
