using Matter.Windows.Controller;
using System.Threading.Tasks;
using Windows.Foundation.Collections;

namespace MatterControllerApp;

internal static class ApiSurfaceCompileChecks
{
    internal static async Task ExerciseGenericInteractionsAsync(MatterController controller, ulong nodeId)
    {
        var value = new PropertySet { ["value"] = true };
        var arguments = new PropertySet();
        var timed = new TimedInteractionOptions(5_000);
        var attribute = new AttributePath(1, 0x0006, 0x0000);
        var command = new CommandPath(1, 0x0006, 0x0001);
        var eventPath = new EventPath(1, 0x0003, 0x0000, false);

        await controller.ReadAttributeAsync(nodeId, attribute);
        await controller.WriteAttributeAsync(nodeId, attribute, value);
        await controller.WriteAttributeTimedAsync(nodeId, attribute, value, timed);
        await controller.InvokeCommandAsync(nodeId, command, arguments);
        await controller.InvokeCommandTimedAsync(nodeId, command, arguments, timed);

        AttributeSubscription attributeSubscription =
            await controller.SubscribeAttributeAsync(nodeId, attribute, 1, 60);
        await attributeSubscription.CloseAsync();

        await controller.ReadEventsAsync(nodeId, eventPath, 0);
        EventSubscription eventSubscription =
            await controller.SubscribeEventAsync(nodeId, eventPath, 0, 1, 60);
        await eventSubscription.CloseAsync();
    }

    internal static async Task ExerciseNetworkCommissioningAsync(MatterController controller)
    {
        var wiFi = new BleNetworkCommissioningParameters
        {
            NodeId = 1,
            SetupPinCode = 20202021,
            LongDiscriminator = 3840,
            WiFi = new WiFiNetworkCredentials("network", "passphrase")
        };
        await new MatterControllerNetworkCommissioning(controller).CommissionBleAsync(wiFi);

        var dataset = new Windows.Storage.Streams.Buffer(16) { Length = 16 };
        var thread = new BleNetworkCommissioningParameters
        {
            NodeId = 2,
            SetupPinCode = 20202021,
            LongDiscriminator = 3840,
            Thread = new ThreadNetworkCredentials(dataset)
        };
        await new MatterControllerNetworkCommissioning(controller).CommissionBleAsync(thread);
    }
}
