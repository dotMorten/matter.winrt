#if MATTER_CAMERA_DEVELOPMENT
using Matter.Windows.Controller;
using System.Runtime.CompilerServices;

namespace MatterControllerApp.Camera;

internal static class CameraAllocationOwnership
{
    private sealed class ControllerFailures
    {
        internal readonly HashSet<ulong> Nodes = [];
        internal bool CapacityExhausted;
    }

    private static readonly ConditionalWeakTable<MatterController, ControllerFailures> failures = new();

    internal static bool IsNodeUncertain(MatterController controller, ulong node) =>
        failures.TryGetValue(controller, out var value) && (value.CapacityExhausted || value.Nodes.Contains(node));

    internal static void MarkNodeUncertain(MatterController controller, ulong node)
    {
        var value = failures.GetValue(controller, static _ => new ControllerFailures());
        if (value.Nodes.Count >= 64 && !value.Nodes.Contains(node))
        {
            value.CapacityExhausted = true;
            return;
        }
        value.Nodes.Add(node);
    }

    internal static bool IsOutcomeUncertain(MatterCameraFailure failure) =>
        failure.Stage == MatterCameraStage.StreamDeallocation ||
        (failure.Stage == MatterCameraStage.StreamAllocation &&
         failure.Category is MatterCameraFailureCategory.Timeout or
            MatterCameraFailureCategory.ConnectionFailure or MatterCameraFailureCategory.MalformedData or
            MatterCameraFailureCategory.Unknown);
}
#endif
