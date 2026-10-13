#if MATTER_CAMERA_DEVELOPMENT
using Matter.Windows.Controller;
using System.Text.Json;

namespace MatterControllerApp.Camera;

internal sealed class CameraAudioProfiles
{
    internal sealed record Profile(uint SampleRate, uint BitRate)
    {
        internal MatterCameraAudioStreamOptions Options() => new()
        {
            AudioCodec = 0, Channels = 1, SampleRate = SampleRate, BitRate = BitRate, BitDepth = 16
        };
    }

    private string? successfulKey;
    private Profile? successfulProfile;
    private DateTimeOffset expires;
    internal string CurrentKey { get; private set; } = "";

    internal IReadOnlyList<Profile> Prepare(ushort endpoint, uint firmware, MatterCameraAudioCapabilities capabilities,
        JsonElement engine, bool talk, uint videoBitRate, DateTimeOffset now)
    {
        InvalidateIfExpired(now);
        var microphone = capabilities.Microphone;
        var speaker = capabilities.Speaker;
        if (!capabilities.SupportsListening || capabilities.PrivacyEnabled ||
            capabilities.Camera.StreamManagement.Revision != 2 || capabilities.Camera.TransportProvider.Revision != 2 ||
            microphone is null || microphone.MaximumChannels < 1 || !microphone.Codecs.Contains((byte)0) ||
            !microphone.BitDepths.Contains((byte)16) ||
            !engine.GetProperty("codecs").EnumerateArray().Any(value => value.GetString() == "audio/opus") ||
            !engine.GetProperty("bitDepths").EnumerateArray().Any(value => value.GetByte() == 16) ||
            engine.GetProperty("maximumChannels").GetByte() < 1)
        {
            throw new NotSupportedException("No compatible camera microphone / media-engine format, revision, or privacy state.");
        }
        if (talk && (capabilities.TwoWayTalkSupport != 2 || speaker is null || speaker.MaximumChannels < 1 ||
            !speaker.Codecs.Contains((byte)0) || !speaker.BitDepths.Contains((byte)16) ||
            capabilities.SpeakerMuted == true || capabilities.SpeakerVolume == 0))
        {
            throw new NotSupportedException("Talkback requires compatible, explicitly unmuted full-duplex speaker capabilities. Half-duplex is not validated.");
        }
        uint available = capabilities.MaximumNetworkBandwidth > videoBitRate
            ? (capabilities.MaximumNetworkBandwidth - videoBitRate) / (talk ? 2u : 1u) : 0;
        uint[] engineRates = engine.GetProperty("sampleRates").EnumerateArray().Select(value => value.GetUInt32()).ToArray();
        var candidates = new List<Profile>();
        foreach (uint rate in new uint[] { 48000, 24000, 16000, 12000, 8000 })
        {
            if (engineRates.Contains(rate) && microphone.SampleRates.Contains(rate) &&
                (!talk || speaker.SampleRates.Contains(rate)) && available >= 6000)
            {
                // Client Opus voice-quality budgets; device acceptance is still required.
                candidates.Add(new Profile(rate, Math.Min(available, Math.Min(64000u, rate * 2u))));
            }
        }
        CurrentKey = $"{endpoint}:{firmware}:{talk}:{videoBitRate}:{capabilities.MaximumNetworkBandwidth}:" +
            $"{capabilities.Camera.StreamManagement.FeatureMap}:{capabilities.TwoWayTalkSupport}:" +
            $"{microphone.MaximumChannels}:{string.Join(",", microphone.Codecs)}:" +
            $"{string.Join(",", microphone.SampleRates)}:{string.Join(",", microphone.BitDepths)}:" +
            $"{speaker?.MaximumChannels}:{string.Join(",", speaker?.Codecs ?? [])}:" +
            $"{string.Join(",", speaker?.SampleRates ?? [])}:{string.Join(",", speaker?.BitDepths ?? [])}:" +
            engine.GetRawText();
        if (successfulKey != CurrentKey)
        {
            Invalidate();
        }
        if (successfulProfile is not null && candidates.Remove(successfulProfile))
        {
            candidates.Insert(0, successfulProfile);
        }
        if (candidates.Count == 0)
        {
            throw new NotSupportedException("No advertised audio sample rate fits the media engine and AV bandwidth budget.");
        }
        return candidates.Take(3).ToArray();
    }

    internal void Remember(Profile profile, DateTimeOffset now)
    {
        successfulKey = CurrentKey;
        successfulProfile = profile;
        expires = now.AddMinutes(5);
    }

    internal void Invalidate()
    {
        successfulKey = null;
        successfulProfile = null;
    }

    private void InvalidateIfExpired(DateTimeOffset now)
    {
        if (now >= expires)
        {
            Invalidate();
        }
    }

    internal static bool CanTryAnother(MatterCameraFailure failure) =>
        failure.Stage == MatterCameraStage.StreamAllocation && failure.HasNativeError &&
        failure.Category is MatterCameraFailureCategory.DynamicConstraintError or MatterCameraFailureCategory.ConstraintError;
}
#endif
