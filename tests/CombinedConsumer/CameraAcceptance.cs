using Matter.Windows.Controller;

internal static class CameraAcceptance
{
    internal static async Task RunAsync()
    {
        BaselineAcceptance.Require(MatterControllerCamera.IsSignalingSupported, "Camera signaling was not built");
        var absent = new MatterCameraStreamSelection(MatterCameraStreamSelectionKind.Absent, null);
        var automatic = new MatterCameraStreamSelection(MatterCameraStreamSelectionKind.AutoSelect, null);
        var zero = new MatterCameraStreamSelection(MatterCameraStreamSelectionKind.ExplicitId, 0);
        BaselineAcceptance.Require(absent.StreamId is null && automatic.StreamId is null && zero.StreamId == 0,
            "Tagged camera selection or valid zero ID changed");
        _ = new MatterCameraVideoStreamOptions();
        _ = new MatterCameraAudioStreamOptions
        {
            AudioCodec = 0, Channels = 1, SampleRate = 48000, BitRate = 64000, BitDepth = 24
        };
        _ = new MatterCameraIceCandidate("candidate:test", "0", 0);
        _ = new MatterCameraIceServer(new[] { "stun:example.invalid" }, false, "", false, "", null);
        string storage = Path.Combine(Path.GetTempPath(), $"matter-combined-{Guid.NewGuid():N}");
        MatterController? controller = null;
        try
        {
            controller = await MatterController.CreateAsync(new ControllerOptions
            {
                StoragePath = storage, EnableCameraSignaling = true
            });
            _ = new MatterControllerCommissioning(controller);
            var discovery = new MatterControllerCameraDiscovery(controller);
            _ = new MatterControllerCamera(controller);
            await controller.CloseAsync();
            var result = await discovery.TryDiscoverCamerasAsync(1);
            BaselineAcceptance.Require(!result.Succeeded && result.Failure is not null,
                "Camera discovery on a closed shared controller synthesized success");
        }
        finally
        {
            if (controller is not null)
            {
                await controller.CloseAsync();
            }
            if (Directory.Exists(storage))
            {
                Directory.Delete(storage, recursive: true);
            }
        }
        Console.WriteLine("PASS combined package camera activation and shared-controller structured API coverage");
    }

    internal static async Task CompileCameraOperationsAsync(MatterController controller, ulong node, ushort endpoint)
    {
        var camera = new MatterControllerCamera(controller);
        await new MatterControllerCameraDiscovery(controller).DiscoverCamerasAsync(node);
        await camera.ReadCapabilitiesAsync(node, endpoint);
        await camera.ReadAudioCapabilitiesAsync(node, endpoint);
        var video = await camera.AllocateVideoStreamAsync(node, endpoint, new MatterCameraVideoStreamOptions());
        var audio = await camera.AllocateAudioStreamAsync(node, endpoint, new MatterCameraAudioStreamOptions());
        var session = await camera.StartSessionAsync(node, endpoint,
            new MatterCameraStreamSelection(MatterCameraStreamSelectionKind.ExplicitId, video.StreamId),
            new MatterCameraStreamSelection(MatterCameraStreamSelectionKind.ExplicitId, audio.StreamId),
            "v=0", Array.Empty<MatterCameraIceServer>());
        await session.CloseAsync();
        await camera.DeallocateAudioStreamAsync(node, endpoint, audio.StreamId);
        await camera.DeallocateVideoStreamAsync(node, endpoint, video.StreamId);
    }
}
