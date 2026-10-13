#if MATTER_CAMERA_DEVELOPMENT
using Matter.Windows.Controller;

namespace MatterControllerApp.Camera;

internal static class CameraAllocationOwnership
{
    internal static bool IsOutcomeUncertain(MatterCameraFailure failure) =>
        failure.Stage == MatterCameraStage.StreamDeallocation ||
        (failure.Stage == MatterCameraStage.StreamAllocation &&
         failure.Category is MatterCameraFailureCategory.Timeout or
            MatterCameraFailureCategory.ConnectionFailure or MatterCameraFailureCategory.MalformedData or
            MatterCameraFailureCategory.Unknown);
}
#endif
