using Matter.Windows.Controller;

await BaselineAcceptance.RunAsync();
#if COMBINED_CONSUMER
await CameraAcceptance.RunAsync();
#endif
Console.WriteLine("PASS baseline package projection, constructors, enumeration and closed-controller commissioning failure");
if (args.Length == 2 && args[0] == "--result")
{
    using var stream = File.Create(args[1]);
    using var writer = new System.Text.Json.Utf8JsonWriter(stream);
    writer.WriteStartObject();
    writer.WriteBoolean("succeeded", true);
    writer.WriteString("packageFullName", Windows.ApplicationModel.Package.Current.Id.FullName);
    writer.WriteEndObject();
}

internal static class BaselineAcceptance
{
    internal static void Require(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }

    internal static async Task RunAsync()
    {
        Require((int)MatterNetworkInterfaceSelectionMode.Automatic == 0 &&
            (int)MatterNetworkInterfaceSelectionMode.PreferSpecifiedInterface == 1 &&
            (int)MatterNetworkInterfaceSelectionMode.RequireSpecifiedInterface == 2,
            "Published selection enum values changed");
        Require((int)MatterNetworkInterfaceType.Unknown == 0 && (int)MatterNetworkInterfaceType.WiFi == 1 &&
            (int)MatterNetworkInterfaceType.Ethernet == 2 && (int)MatterNetworkInterfaceType.Cellular == 3 &&
            (int)MatterNetworkInterfaceType.Thread == 4, "Published interface types changed");
        foreach (MatterNetworkInterfaceSelectionMode mode in Enum.GetValues<MatterNetworkInterfaceSelectionMode>())
        {
            ulong id = mode == MatterNetworkInterfaceSelectionMode.Automatic ? 0 : ulong.MaxValue;
            var selection = new MatterNetworkInterfaceSelection(mode, id);
            Require(selection.Mode == mode && selection.InterfaceId == id, "Selection constructor contract changed");
        }
        var interfaces = await MatterNetworkInterfaceProvider.GetEligibleNetworkInterfacesAsync();
        var dataset = new Windows.Storage.Streams.Buffer(16) { Length = 16 };
        Require(new ThreadNetworkCredentials(dataset).DatasetLength == 16, "Thread credential activation changed");
        foreach (var item in interfaces)
        {
            Require(item.Id != 0 && item.Name is not null, "Interface enumeration lost identifier/name");
            _ = (item.InterfaceIndex, item.Type, item.IsConnected, item.SupportsIpv6,
                item.SupportsMulticast, item.IsVirtual);
        }

        string storage = Path.Combine(Path.GetTempPath(), $"matter-baseline-{Guid.NewGuid():N}");
        MatterController? controller = null;
        try
        {
            controller = await MatterController.CreateAsync(new ControllerOptions { StoragePath = storage });
            var commissioning = new MatterControllerCommissioning(controller);
            _ = new MatterControllerNetworkCommissioning(controller);
            _ = new MatterControllerRecovery(controller);
            var parameters = new BleNetworkCommissioningParameters
            {
                NodeId = 1,
                SetupPinCode = 20202021,
                LongDiscriminator = 3840,
                WiFi = new WiFiNetworkCredentials("regression-no-device", "regression-password")
            };
            commissioning.ProgressChanged += (_, progress) =>
            {
                Require(progress.Stage != MatterCommissioningStage.Completed,
                    "Closed-controller commissioning emitted synthetic success");
            };
            await controller.CloseAsync();
            var result = await commissioning.CommissionBleAsync(parameters,
                new MatterNetworkInterfaceSelection(MatterNetworkInterfaceSelectionMode.Automatic, 0));
            Require(!result.Succeeded && result.Node is null && result.Outcome == MatterCommissioningOutcome.Failed &&
                !string.IsNullOrWhiteSpace(result.DiagnosticMessage), "Closed controller did not fail explicitly");
            var preferred = await commissioning.CommissionBleAsync(parameters,
                new MatterNetworkInterfaceSelection(MatterNetworkInterfaceSelectionMode.PreferSpecifiedInterface, ulong.MaxValue));
            Require(!preferred.Succeeded && preferred.Node is null,
                "An unavailable preferred interface produced synthetic commissioning success");
            bool requiredRejected = false;
            try
            {
                await commissioning.CommissionBleAsync(parameters,
                    new MatterNetworkInterfaceSelection(MatterNetworkInterfaceSelectionMode.RequireSpecifiedInterface, ulong.MaxValue));
            }
            catch (Exception exception) when (exception.HResult == unchecked((int)0x80070057) &&
                exception.Message.Contains("PreferredInterfaceNotFound", StringComparison.Ordinal))
            {
                requiredRejected = true;
            }
            Require(requiredRejected, "Require mode did not reject an unavailable interface before dispatch");
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
    }
}
