#if MATTER_CAMERA_DEVELOPMENT
using System.Text.Json;

namespace MatterControllerApp.Camera;

internal static class CameraReceiverMediaBridge
{
    internal static async Task<bool> RouteAsync(JsonElement message, Func<string, Task> media,
        Func<long, Task> requestCapture, Action releaseCapture, Action<string> audio, Action<JsonElement> diagnostics)
    {
        switch (message.GetProperty("type").GetString())
        {
            case "media":
                string state = message.GetProperty("state").GetString() ?? "";
                if (state is not ("playing" or "disconnected" or "failed"))
                {
                    throw new InvalidOperationException("Unexpected media state.");
                }
                await media(state);
                return true;
            case "capture-request":
                long press = message.GetProperty("press").GetInt64();
                if (press < 1 || press > 9007199254740991)
                {
                    throw new InvalidOperationException("Invalid microphone authorization sequence.");
                }
                await requestCapture(press);
                return true;
            case "capture-release":
                releaseCapture();
                return true;
            case "audio":
                string audioState = message.GetProperty("state").GetString() ?? "";
                if (audioState is not ("receiving" or "decoded" or "playing" or "playback-blocked" or
                    "muted" or "unmuted" or "capture-on" or "capture-off" or "outgoing" or "input-signal-observed" or
                    "permission-denied" or "permission-timeout" or "capture-timeout" or "capture-unavailable" or
                    "talk-unsupported" or "capture-pending"))
                {
                    throw new InvalidOperationException("Unexpected audio media state.");
                }
                audio(audioState);
                return true;
            case "talk-diagnostics":
                var evidence = message.GetProperty("evidence");
                if (evidence.ValueKind != JsonValueKind.Object ||
                    evidence.EnumerateObject().Any(field => field.Name is not ("captureEnabled" or "inputSelection" or
                        "inputSignalAvailable" or "inputSignalObserved" or "inputEnergyDelta" or "direction" or "codec" or
                        "encodingActive" or "trackAttached" or "packetsDelta" or "bytesDelta" or "cameraPlayback")) ||
                    !evidence.GetProperty("captureEnabled").GetBoolean() ||
                    evidence.GetProperty("cameraPlayback").GetString() != "unverified" ||
                    evidence.GetProperty("inputSelection").GetString() is not ("default" or "explicit") ||
                    evidence.GetProperty("direction").GetString() is not ("sendrecv" or "sendonly" or "recvonly" or "inactive" or "unnegotiated") ||
                    evidence.GetProperty("codec").GetString() is not ("audio/opus" or "unknown") ||
                    !double.IsFinite(evidence.GetProperty("inputEnergyDelta").GetDouble()) ||
                    evidence.GetProperty("inputEnergyDelta").GetDouble() < 0 ||
                    evidence.GetProperty("packetsDelta").GetInt64() is < 0 or > 9007199254740991 ||
                    evidence.GetProperty("bytesDelta").GetInt64() is < 0 or > 9007199254740991)
                {
                    throw new InvalidOperationException("Invalid redacted talkback evidence.");
                }
                _ = evidence.GetProperty("inputSignalAvailable").GetBoolean();
                _ = evidence.GetProperty("inputSignalObserved").GetBoolean();
                _ = evidence.GetProperty("encodingActive").GetBoolean();
                _ = evidence.GetProperty("trackAttached").GetBoolean();
                diagnostics(evidence);
                return true;
            default:
                return false;
        }
    }
}
#endif
