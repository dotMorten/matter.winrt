#if MATTER_CAMERA_DEVELOPMENT
using Matter.Windows.Controller;
using System.Text.Json;

namespace MatterControllerApp.Camera;

internal sealed class CameraAudioProfiles
{
    internal sealed record Profile(uint SampleRate, uint BitRate, byte BitDepth = 16)
    {
        internal MatterCameraAudioStreamOptions Options() => new()
        {
            AudioCodec = 0, Channels = 1, SampleRate = SampleRate, BitRate = BitRate, BitDepth = BitDepth
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
            microphone is null || microphone.MaximumChannels is < 1 or > 8 || !microphone.Codecs.Contains((byte)0) ||
            !engine.GetProperty("codecs").EnumerateArray().Any(value => value.GetString() == "audio/opus") ||
            engine.GetProperty("maximumChannels").GetByte() < 1)
        {
            throw new NotSupportedException("No compatible camera microphone / media-engine format, revision, or privacy state.");
        }
        if (talk && (capabilities.TwoWayTalkSupport != 2 || speaker is null || speaker.MaximumChannels is < 1 or > 8 ||
            !speaker.Codecs.Contains((byte)0) ||
            capabilities.SpeakerMuted == true || capabilities.SpeakerVolume == 0))
        {
            throw new NotSupportedException("Talkback requires compatible, explicitly unmuted full-duplex speaker capabilities. Half-duplex is not validated.");
        }
        uint available = capabilities.MaximumNetworkBandwidth > videoBitRate
            ? (capabilities.MaximumNetworkBandwidth - videoBitRate) / (talk ? 2u : 1u) : 0;
        uint[] engineRates = engine.GetProperty("sampleRates").EnumerateArray().Select(value => value.GetUInt32()).ToArray();
        // Source allocation metadata is not the browser's internal PCM precision.
        var engineDepths = engine.TryGetProperty("sourceBitDepths", out var sourceDepths)
            ? sourceDepths : engine.GetProperty("bitDepths");
        byte[] depths = engineDepths.EnumerateArray().Select(value => value.GetByte()).ToArray();
        byte depth = new byte[] { 16, 24, 32, 8 }.FirstOrDefault(value =>
            depths.Contains(value) && microphone.BitDepths.Contains(value) &&
            (!talk || speaker!.BitDepths.Contains(value)));
        if (depth == 0)
        {
            throw new NotSupportedException("No usable advertised source bit depth fits this mode. Raw microphone/speaker entries are retained; no replacement is invented.");
        }
        var candidates = new List<Profile>();
        foreach (uint rate in new uint[] { 48000, 24000, 16000, 12000, 8000 })
        {
            if (engineRates.Contains(rate) && microphone.SampleRates.Contains(rate) &&
                (!talk || speaker.SampleRates.Contains(rate)) && available >= 6000)
            {
                // Client Opus voice-quality budgets; device acceptance is still required.
                candidates.Add(new Profile(rate, Math.Min(available, Math.Min(64000u, rate * 2u)), depth));
            }
        }
        if (candidates.Count is > 0 and < 3)
        {
            var preferred = candidates[0];
            foreach (uint budget in new uint[] { 32000, 20000 })
            {
                if (budget < preferred.BitRate)
                {
                    candidates.Add(preferred with { BitRate = budget });
                }
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

    internal static string DescribeFormats(MatterCameraAudioCapabilities capabilities)
    {
        static string Describe(string direction, int attribute, MatterCameraAudioFormatCapabilities? formats)
        {
            if (formats is null)
            {
                return $"{direction} attribute {attribute}: unavailable.";
            }
            var warnings = new List<string>();
            if (formats.MaximumChannels is < 1 or > 8)
            {
                warnings.Add($"MaximumChannels={formats.MaximumChannels}");
            }
            if (!formats.Codecs.Any(value => value is 0 or 1))
            {
                warnings.Add($"Codecs=[{string.Join(",", formats.Codecs)}]: no supported codec");
            }
            if (formats.SampleRates.Contains(0u) || formats.SampleRates.Count == 0)
            {
                warnings.Add($"SampleRates=[{string.Join(",", formats.SampleRates)}]: unusable entries");
            }
            if (formats.BitDepths.Any(value => value is not (8 or 16 or 24 or 32)) || formats.BitDepths.Count == 0)
            {
                warnings.Add($"BitDepths=[{string.Join(",", formats.BitDepths)}]: unusable entries");
            }
            return $"{direction} attribute {attribute}: channels={formats.MaximumChannels}; " +
                $"codecs=[{string.Join(",", formats.Codecs)}]; rates=[{string.Join(",", formats.SampleRates)}]; " +
                $"source depths=[{string.Join(",", formats.BitDepths)}]. " +
                (warnings.Count == 0 ? "" : $"WARNING: {string.Join("; ", warnings)}; retained raw, never selected.");
        }
        return $"{Describe("Microphone", 7, capabilities.Microphone)} {Describe("Speaker", 8, capabilities.Speaker)}";
    }
}
#endif
