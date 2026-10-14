#if MATTER_CAMERA_DEVELOPMENT
using Matter.Windows.Controller;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.Web.WebView2.Core;
using System.Text;
using System.Text.Json;
using System.Threading.Channels;

namespace MatterControllerApp.Camera;

public sealed class CameraReceiverWindow : Window
{
    private static readonly HashSet<CameraReceiverWindow> openWindows = [];
    private const string ReceiverUri = "https://matter-camera.local/receiver.html";
    private readonly MatterController controller;
    private readonly ulong nodeId;
    private readonly WebView2 browser = new();
    private readonly NumberBox endpoint = new() { Header = "Discovered camera endpoint", Minimum = 0, Maximum = 65534 };
    private readonly ComboBox stream = new() { Header = "Advertised allocated LiveView stream" };
    private readonly TextBlock status = new() { Text = "Discover camera endpoints before starting." };
    private readonly Button start = new() { Content = "Start receiver", IsEnabled = false };
    private readonly ComboBox mode = new() { Header = "Explicit media mode", SelectedIndex = 0 };
    private readonly TextBlock audioStatus = new() { Text = "Local microphone off.", TextWrapping = TextWrapping.Wrap };
    private readonly TextBlock audioDiagnostics = new() { TextWrapping = TextWrapping.Wrap };
    private readonly CameraAudioProfiles audioProfiles = new();
    private readonly List<string> audioAttempts = [];
    private readonly Channel<string> messages = Channel.CreateBounded<string>(new BoundedChannelOptions(32)
    {
        SingleReader = true,
        SingleWriter = true,
        FullMode = BoundedChannelFullMode.Wait
    });
    private CancellationTokenSource? sessionCancellation;
    private MatterCameraSession? session;
    private Task? signalTask;
    private Task? privacyTask;
    private Task? startTask;
    private Task? allocationTask;
    private Task? preparationTask;
    private Task? stopTask;
    private Task? closeTask;
    private ushort selectedEndpoint;
    private ushort selectedStream;
    private byte selectedCodec;
    private ushort? streamEndpoint;
    private string token = "";
    private bool ready;
    private bool loading;
    private bool closing;
    private bool closed;
    private ushort? ownedStream;
    private ushort ownedStreamEndpoint;
    private bool deallocationAttempted;
    private bool ownershipUncertain
    {
        get => CameraAllocationOwnership.IsNodeUncertain(controller, nodeId);
        set
        {
            if (value)
            {
                CameraAllocationOwnership.MarkNodeUncertain(controller, nodeId);
            }
        }
    }
    private MatterCameraAudioStream? ownedAudio;
    private bool audioReleaseAttempted;
    private bool selectedListening;
    private bool selectedTalk;
    private bool microphoneArmed;
    private bool activeWindow = true;
    private CameraAudioProfiles.Profile? selectedAudioProfile;

    public CameraReceiverWindow(MatterController controller, ulong nodeId)
    {
        this.controller = controller;
        this.nodeId = nodeId;
        Title = "Matter camera receiver development";
        Grid root = new() { RowSpacing = 12, Padding = new Thickness(16) };
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });
        StackPanel controls = new() { Spacing = 8 };
        Button discover = new() { Content = "Discover camera endpoints" };
        Button refreshStreams = new() { Content = "Read allocated video streams" };
        Button allocate = new() { Content = "Allocate H.264 LiveView (640x480, 30 fps)" };
        Button stop = new() { Content = "Stop" };
        controls.Children.Add(discover);
        controls.Children.Add(endpoint);
        controls.Children.Add(refreshStreams);
        controls.Children.Add(allocate);
        controls.Children.Add(stream);
        mode.Items.Add("Video only (no audio)");
        mode.Items.Add("Video and listening (receive only)");
        mode.Items.Add("Video, listening and explicit push-to-talk (full duplex)");
        mode.SelectedIndex = 0;
        controls.Children.Add(mode);
        controls.Children.Add(start);
        controls.Children.Add(stop);
        controls.Children.Add(status);
        controls.Children.Add(audioStatus);
        controls.Children.Add(audioDiagnostics);
        root.Children.Add(new ScrollViewer { Content = controls, MaxHeight = 360 });
        Grid.SetRow(browser, 1);
        root.Children.Add(browser);
        Content = root;
        root.Loaded += OnLoaded;
        discover.Click += async (_, _) =>
        {
            if (closing || sessionCancellation is not null || allocationTask is not null || ownedStream is not null ||
                stopTask is not null)
            {
                status.Text = "Stop the owned receiver and allocation before discovering another camera.";
                return;
            }
            try
            {
                var result = await new MatterControllerCameraDiscovery(controller).TryDiscoverCamerasAsync(nodeId);
                if (!result.Succeeded)
                {
                    throw new InvalidOperationException(DescribeFailure(result.Failure));
                }
                var cameras = result.Cameras;
                status.Text = cameras.Count == 0 ? "No camera cluster composition advertised."
                    : $"Camera endpoints: {string.Join(", ", cameras.Select(camera => camera.EndpointId))}";
                if (cameras.Count != 0)
                {
                    endpoint.Value = cameras[0].EndpointId;
                }
            }
            catch (Exception error)
            {
                status.Text = $"Discovery failed: {error.Message}";
            }
        };
        start.Click += OnStart;
        Activated += (_, activation) =>
        {
            activeWindow = activation.WindowActivationState != WindowActivationState.Deactivated;
            if (!activeWindow)
            {
                microphoneArmed = false;
                if (ready && sessionCancellation is not null)
                {
                    try
                    {
                        Send(new { type = "capture-stop", token });
                    }
                    catch (Exception error)
                    {
                        status.Text = $"Capture revocation bridge failed: {error.Message}";
                        _ = StopAsync();
                    }
                }
            }
        };
        allocate.Click += async (_, _) =>
        {
            if (sessionCancellation is not null || allocationTask is not null || stopTask is not null ||
                ownedStream is not null || ownershipUncertain || closing || !double.IsFinite(endpoint.Value) ||
                endpoint.Value < 0 || endpoint.Value > 65534 || endpoint.Value != Math.Truncate(endpoint.Value))
            {
                status.Text = "Select a camera endpoint and stop existing owned resources before allocating.";
                return;
            }
            allocate.IsEnabled = false;
            allocationTask = AllocateStreamAsync(checked((ushort)endpoint.Value));
            try
            {
                await allocationTask;
            }
            catch (Exception error)
            {
                status.Text = $"Allocation failed: {error.Message}";
            }
            finally
            {
                allocationTask = null;
                allocate.IsEnabled = !closing && !ownershipUncertain;
            }
        };
        refreshStreams.Click += async (_, _) =>
        {
            if (sessionCancellation is not null || allocationTask is not null || stopTask is not null || closing ||
                !double.IsFinite(endpoint.Value) || endpoint.Value < 0 || endpoint.Value > 65534 ||
                endpoint.Value != Math.Truncate(endpoint.Value))
            {
                status.Text = "Select a camera endpoint while the receiver is stopped.";
                return;
            }
            refreshStreams.IsEnabled = false;
            stream.Items.Clear();
            streamEndpoint = null;
            ushort requestedEndpoint = checked((ushort)endpoint.Value);
            try
            {
                var camera = new MatterControllerCamera(controller);
                var capabilities = await camera.ReadCapabilitiesAsync(nodeId, requestedEndpoint);
                if (!capabilities.SupportsClientOffer)
                {
                    throw new NotSupportedException("The endpoint does not advertise video and the required client-offer commands.");
                }
                var streams = await camera.ReadAllocatedVideoStreamsAsync(nodeId, requestedEndpoint);
                if (closing)
                {
                    return;
                }
                foreach (var advertised in streams.Where(value => value.IsLiveView))
                {
                    stream.Items.Add(new ComboBoxItem
                    {
                        Content = $"ID {advertised.StreamId}, codec {advertised.VideoCodec}, " +
                            $"{advertised.MinimumWidth}x{advertised.MinimumHeight}-{advertised.MaximumWidth}x{advertised.MaximumHeight}, " +
                            $"{advertised.MinimumFrameRate}-{advertised.MaximumFrameRate} fps",
                        Tag = advertised
                    });
                }
                streamEndpoint = requestedEndpoint;
                status.Text = stream.Items.Count == 0
                    ? "No allocated LiveView stream. Allocate a compatible stream before refreshing."
                    : $"Stream cluster revision {capabilities.StreamManagement.Revision}, " +
                        $"transport revision {capabilities.TransportProvider.Revision}. " +
                        "Choose an advertised stream; its codec is checked before sending an offer.";
            }
            catch (Exception error)
            {
                status.Text = $"Reading allocated streams failed: {error.Message}";
            }
            finally
            {
                refreshStreams.IsEnabled = !closing;
            }
        };
        stop.Click += async (_, _) => await StopAsync();
        AppWindow.Closing += async (_, args) =>
        {
            if (closed)
            {
                return;
            }
            args.Cancel = true;
            if (closing)
            {
                return;
            }
            await CloseReceiverAsync();
        };
        openWindows.Add(this);
    }

    public static async Task CloseAllAsync()
    {
        foreach (var window in openWindows.ToArray())
        {
            await window.CloseReceiverAsync();
        }
    }

    private Task CloseReceiverAsync() => closeTask ??= CloseReceiverCoreAsync();

    private async Task CloseReceiverCoreAsync()
    {
        if (closed)
        {
            return;
        }
        closing = true;
        await StopAsync();
        messages.Writer.TryComplete();
        browser.Close();
        closed = true;
        openWindows.Remove(this);
        Close();
    }

    private async Task AllocateStreamAsync(ushort cameraEndpoint)
    {
        status.Text = "Checking camera capabilities and privacy, then allocating an owned H.264 stream.";
        RequireCameraEndpoint(await new MatterControllerCamera(controller).ReadCapabilitiesAsync(nodeId, cameraEndpoint));
        if (closing || stopTask is not null)
        {
            return;
        }
        // A dispatched allocation must finish so Stop can recover and deallocate its returned ID.
        var result = await new MatterControllerCamera(controller).TryAllocateVideoStreamAsync(
            nodeId, cameraEndpoint, new MatterCameraVideoStreamOptions());
        if (!result.Succeeded)
        {
            ownershipUncertain = CameraAllocationOwnership.IsOutcomeUncertain(result.Failure);
            throw new InvalidOperationException(DescribeFailure(result.Failure));
        }
        var allocated = result.Stream;
        ownedStream = allocated.StreamId;
        ownedStreamEndpoint = cameraEndpoint;
        deallocationAttempted = false;
        if (closing || stopTask is not null)
        {
            return;
        }
        stream.Items.Clear();
        stream.Items.Add(new ComboBoxItem
        {
            Content = $"Owned H.264 stream {allocated.StreamId}, " +
                $"{allocated.MinimumWidth}x{allocated.MinimumHeight}, {allocated.MinimumFrameRate} fps",
            Tag = allocated
        });
        stream.SelectedIndex = 0;
        streamEndpoint = cameraEndpoint;
        status.Text = "Owned stream allocated. Start the receiver; Stop ends the session and deallocates this stream.";
    }

    private async void OnLoaded(object sender, RoutedEventArgs args)
    {
        if (loading || closing)
        {
            return;
        }
        loading = true;
        try
        {
            await browser.EnsureCoreWebView2Async();
            browser.CoreWebView2.SetVirtualHostNameToFolderMapping(
                "matter-camera.local", Path.Combine(AppContext.BaseDirectory, "Camera"),
                CoreWebView2HostResourceAccessKind.Deny);
            browser.CoreWebView2.Settings.AreDevToolsEnabled = false;
            browser.CoreWebView2.Settings.AreDefaultContextMenusEnabled = false;
            browser.CoreWebView2.Settings.IsStatusBarEnabled = false;
            browser.CoreWebView2.NavigationStarting += (_, navigation) => navigation.Cancel = navigation.Uri != ReceiverUri;
            browser.CoreWebView2.NewWindowRequested += (_, request) => request.Handled = true;
            browser.CoreWebView2.PermissionRequested += (_, permission) =>
            {
                bool trusted = Uri.TryCreate(permission.Uri, UriKind.Absolute, out var origin) &&
                    origin.GetLeftPart(UriPartial.Authority) == "https://matter-camera.local" &&
                    browser.CoreWebView2.Source == ReceiverUri;
                permission.SavesInProfile = false;
                permission.State = permission.PermissionKind == CoreWebView2PermissionKind.Microphone &&
                    trusted && microphoneArmed && activeWindow && selectedTalk && session is not null &&
                    sessionCancellation is not null && !closing
                    ? CoreWebView2PermissionState.Default : CoreWebView2PermissionState.Deny;
            };
            browser.CoreWebView2.DownloadStarting += (_, download) => download.Cancel = true;
            browser.CoreWebView2.WebMessageReceived += OnMessage;
            browser.CoreWebView2.NavigationCompleted += (_, navigation) =>
            {
                ready = navigation.IsSuccess;
                start.IsEnabled = ready && sessionCancellation is null && stopTask is null &&
                    allocationTask is null && !ownershipUncertain;
                if (!ready)
                {
                    status.Text = "Trusted receiver content could not load.";
                }
            };
            browser.Source = new Uri(ReceiverUri);
            await DrainMessagesAsync();
        }
        catch (Exception error)
        {
            status.Text = $"Receiver initialization failed: {error.Message}";
            await StopAsync();
        }
    }

    private async void OnStart(object sender, RoutedEventArgs args)
    {
        if (!ready || closing || stopTask is not null || allocationTask is not null || preparationTask is not null || ownershipUncertain ||
            sessionCancellation is not null ||
            !double.IsFinite(endpoint.Value) || endpoint.Value < 0 || endpoint.Value > 65534 ||
            endpoint.Value != Math.Truncate(endpoint.Value) || streamEndpoint != endpoint.Value ||
            stream.SelectedItem is not ComboBoxItem { Tag: MatterCameraVideoStream advertised })
        {
            status.Text = "Choose a camera endpoint and an already allocated LiveView video stream.";
            return;
        }
        if (ownedStream is not null && (advertised.StreamId != ownedStream || endpoint.Value != ownedStreamEndpoint))
        {
            status.Text = "Stop the owned allocation before selecting an externally allocated stream.";
            return;
        }
        sessionCancellation = new CancellationTokenSource();
        selectedEndpoint = checked((ushort)endpoint.Value);
        selectedStream = advertised.StreamId;
        selectedCodec = advertised.VideoCodec;
        selectedListening = mode.SelectedIndex > 0;
        selectedTalk = mode.SelectedIndex == 2;
        selectedAudioProfile = null;
        audioAttempts.Clear();
        microphoneArmed = false;
        token = Guid.NewGuid().ToString("N");
        start.IsEnabled = false;
        mode.IsEnabled = false;
        var owner = sessionCancellation;
        preparationTask = PrepareReceiverAsync(advertised, owner, token);
        try
        {
            await preparationTask;
        }
        catch (OperationCanceledException) when (owner.IsCancellationRequested)
        {
            // Stop waits for any dispatched allocation and releases the recovered ID.
        }
        catch (Exception error)
        {
            audioProfiles.Invalidate();
            status.Text = $"Receiver preparation failed: {error.Message}";
            await StopAsync();
        }
        finally
        {
            preparationTask = null;
        }
    }

    private async Task PrepareReceiverAsync(MatterCameraVideoStream advertised, CancellationTokenSource owner, string ownerToken)
    {
        RequireCameraEndpoint(await new MatterControllerCamera(controller).ReadCapabilitiesAsync(nodeId, selectedEndpoint)
            .AsTask(owner.Token));
        if (selectedListening)
        {
            var camera = new MatterControllerCamera(controller);
            var capabilities = await camera.ReadAudioCapabilitiesAsync(nodeId, selectedEndpoint).AsTask(owner.Token);
            audioDiagnostics.Text = CameraAudioProfiles.DescribeFormats(capabilities);
            var basic = await controller.GetBasicInformationCluster(nodeId, 0).ReadAsync().AsTask(owner.Token);
            string script = await browser.ExecuteScriptAsync($"JSON.stringify(RealtimeAudio.formats({(selectedTalk ? "true" : "false")}))")
                .AsTask().WaitAsync(TimeSpan.FromSeconds(5), owner.Token);
            using var formats = JsonDocument.Parse(JsonSerializer.Deserialize<string>(script)
                ?? throw new InvalidOperationException("Missing media-engine formats."));
            var candidates = audioProfiles.Prepare(selectedEndpoint, basic.SoftwareVersion, capabilities,
                formats.RootElement, selectedTalk, advertised.MaximumBitRate, DateTimeOffset.UtcNow);
            for (int attempt = 0; attempt < candidates.Count; ++attempt)
            {
                owner.Token.ThrowIfCancellationRequested();
                var candidate = candidates[attempt];
                audioStatus.Text = $"Audio candidate {attempt + 1}/{candidates.Count}: advertised Opus mono, " +
                    $"{candidate.SampleRate} Hz, source metadata {candidate.BitDepth} bit, {candidate.BitRate} bit/s. Microphone capture remains off.";
                audioAttempts.Add($"{candidate.SampleRate} Hz, {candidate.BitRate} bit/s: " +
                    (attempt == 0 ? "deterministic compatible/cached preference"
                        : "alternative advertised format after correlated parameter refusal"));
                var allocation = await camera.TryAllocateAudioStreamAsync(nodeId, selectedEndpoint, candidate.Options());
                if (allocation.Succeeded)
                {
                    ownedAudio = allocation.Stream;
                    audioReleaseAttempted = false;
                    selectedAudioProfile = candidate;
                    break;
                }
                audioProfiles.Invalidate();
                audioAttempts.Add(DescribeFailure(allocation.Failure));
                ownershipUncertain |= CameraAllocationOwnership.IsOutcomeUncertain(allocation.Failure);
                if (!CameraAudioProfiles.CanTryAnother(allocation.Failure) || attempt + 1 == candidates.Count)
                {
                    throw new InvalidOperationException(DescribeFailure(allocation.Failure));
                }
                audioStatus.Text = $"Correlated refusal: {DescribeFailure(allocation.Failure)}. " +
                    "Trying the next compatible sample-rate/voice-bitrate candidate.";
            }
            audioStatus.Text = $"Camera microphone muted: {capabilities.MicrophoneMuted}; " +
                $"camera speaker muted: {capabilities.SpeakerMuted}. Local capture off; remote controls unchanged.";
            audioDiagnostics.Text = CameraAudioProfiles.DescribeFormats(capabilities) +
                $" Speaker snapshot mute={capabilities.SpeakerMuted}, volume={capabilities.SpeakerVolume}, " +
                $"bounds={capabilities.SpeakerMinimumVolume}-{capabilities.SpeakerMaximumVolume}; " +
                $"endpoint={selectedEndpoint}, video stream={selectedStream}, audio stream={ownedAudio?.StreamId}, " +
                $"codec={ownedAudio?.AudioCodec}, channels={ownedAudio?.Channels}, rate={ownedAudio?.SampleRate}, " +
                $"bitrate={ownedAudio?.BitRate}, source depth={ownedAudio?.BitDepth}, transport references={ownedAudio?.ReferenceCount}; " +
                "camera playback unverified.";
        }
        owner.Token.ThrowIfCancellationRequested();
        if (sessionCancellation != owner)
        {
            return;
        }
        status.Text = selectedListening ? "Generating explicit AV offer. Local microphone remains off."
            : "Generating a video-only offer. Microphone and recording are disabled.";
        Send(new
        {
            type = "start", token = ownerToken, videoCodec = selectedCodec, listen = selectedListening, talk = selectedTalk,
            audio = ownedAudio is null ? null : new
            {
                codec = ownedAudio.AudioCodec == 0 ? "audio/opus" : "unsupported",
                channels = ownedAudio.Channels, sampleRate = ownedAudio.SampleRate,
                bitRate = ownedAudio.BitRate, bitDepth = ownedAudio.BitDepth
            },
            minimumWidth = advertised.MinimumWidth, maximumWidth = advertised.MaximumWidth,
            minimumHeight = advertised.MinimumHeight, maximumHeight = advertised.MaximumHeight
        });
    }

    private void OnMessage(CoreWebView2 sender, CoreWebView2WebMessageReceivedEventArgs args)
    {
        if (closing)
        {
            return;
        }
        if (args.Source != ReceiverUri || Encoding.UTF8.GetByteCount(args.WebMessageAsJson) > 24576 ||
            !messages.Writer.TryWrite(args.WebMessageAsJson))
        {
            status.Text = "Receiver bridge rejected an origin, oversized message, or queue overflow.";
            _ = StopAsync();
        }
    }

    private async Task DrainMessagesAsync()
    {
        await foreach (string payload in messages.Reader.ReadAllAsync())
        {
            try
            {
                using JsonDocument document = JsonDocument.Parse(payload);
                var message = document.RootElement;
                if (sessionCancellation is null || message.GetProperty("token").GetString() != token)
                {
                    continue;
                }
                string? type = message.GetProperty("type").GetString();
                if (type == "offer")
                {
                    if (session is not null)
                    {
                        throw new InvalidOperationException("An offer has already been sent for this receiver.");
                    }
                    string sdp = message.GetProperty("sdp").GetString() ?? throw new InvalidOperationException("Missing SDP.");
                    if (Encoding.UTF8.GetByteCount(sdp) > 16384)
                    {
                        throw new InvalidOperationException("SDP exceeds the supported bounds.");
                    }
                    startTask = StartSessionAsync(sdp, sessionCancellation, token);
                    await startTask;
                    startTask = null;
                }
                else if (type == "ice")
                {
                    if (session is null)
                    {
                        throw new InvalidOperationException("ICE arrived before the offer was accepted.");
                    }
                    string candidate = message.GetProperty("candidate").GetString() ?? "";
                    string mid = message.GetProperty("mid").ValueKind == JsonValueKind.Null
                        ? "" : message.GetProperty("mid").GetString() ?? "";
                    int lineIndex = message.GetProperty("lineIndex").ValueKind == JsonValueKind.Null
                        ? -1 : message.GetProperty("lineIndex").GetInt32();
                    var result = await session.TrySendCandidatesAsync(new[] { new MatterCameraIceCandidate(candidate, mid, lineIndex) })
                        .AsTask(sessionCancellation.Token);
                    if (!result.Succeeded)
                    {
                        throw new InvalidOperationException(DescribeFailure(result.Failure));
                    }
                }
                else if (type == "error")
                {
                    throw new InvalidOperationException(message.GetProperty("message").GetString() ?? "Receiver failed.");
                }
                else if (!await CameraReceiverMediaBridge.RouteAsync(message, HandleMediaAsync, AuthorizeCaptureAsync,
                    () => microphoneArmed = false, HandleAudioState, HandleTalkDiagnostics))
                {
                    throw new InvalidOperationException("Receiver failed or sent an unsupported bridge message.");
                }
            }
            catch (OperationCanceledException)
            {
                if (stopTask is null)
                {
                    status.Text = "Camera operation canceled.";
                    await StopAsync();
                }
            }
            catch (Exception error)
            {
                audioProfiles.Invalidate();
                status.Text = $"Camera stage failed: {error.Message}";
                await StopAsync();
            }
        }
    }

    private async Task HandleMediaAsync(string state)
    {
        status.Text = state == "playing" ? "Live video frame decoded; playback established."
            : $"Media connectivity failed: {state}. Signaling may still be established.";
        if (state is "failed" or "disconnected")
        {
            audioProfiles.Invalidate();
            await StopAsync();
        }
    }

    private async Task AuthorizeCaptureAsync(long press)
    {
        var owner = sessionCancellation;
        string ownerToken = token;
        if (!selectedTalk || !activeWindow || session is null || owner is null)
        {
            throw new InvalidOperationException("Unexpected microphone authorization request.");
        }
        if (await new MatterControllerCamera(controller).IsPrivacyEnabledAsync(nodeId, selectedEndpoint).AsTask(owner.Token))
        {
            throw new InvalidOperationException("Camera privacy enabled; microphone capture is forbidden.");
        }
        if (sessionCancellation != owner || token != ownerToken || !activeWindow || owner.IsCancellationRequested)
        {
            return;
        }
        microphoneArmed = true;
        Send(new { type = "capture-authorized", token = ownerToken, press });
    }

    private void HandleAudioState(string state)
    {
        if (!selectedListening)
        {
            throw new InvalidOperationException("Unexpected audio in video-only mode.");
        }
        if (state is "capture-on" or "outgoing" or "input-signal-observed" && (!selectedTalk || !microphoneArmed))
        {
            throw new InvalidOperationException("Unexpected local microphone capture.");
        }
        if (state is "capture-off" or "permission-denied" or "permission-timeout" or "capture-timeout" or
            "capture-unavailable" or "talk-unsupported" or "capture-pending")
        {
            microphoneArmed = false;
        }
        audioStatus.Text = $"Audio: {state}. Camera talkback playback remains unverified; RTP progress is not audibility.";
        if (state == "playing" && selectedAudioProfile is not null)
        {
            audioProfiles.Remember(selectedAudioProfile, DateTimeOffset.UtcNow);
        }
    }

    private void HandleTalkDiagnostics(JsonElement evidence)
    {
        if (!selectedTalk || !microphoneArmed)
        {
            throw new InvalidOperationException("Talkback diagnostics without authorized capture.");
        }
        audioStatus.Text = $"Capture={evidence.GetProperty("captureEnabled")}; " +
            $"input={evidence.GetProperty("inputSelection")}, energy available={evidence.GetProperty("inputSignalAvailable")}, " +
            $"signal observed={evidence.GetProperty("inputSignalObserved")}, energy delta={evidence.GetProperty("inputEnergyDelta")}; " +
            $"direction={evidence.GetProperty("direction")}, codec={evidence.GetProperty("codec")}; " +
            $"sender active={evidence.GetProperty("encodingActive")}, track attached={evidence.GetProperty("trackAttached")}; " +
            $"RTP deltas packets={evidence.GetProperty("packetsDelta")}, bytes={evidence.GetProperty("bytesDelta")}; " +
            "camera playback UNVERIFIED.";
    }

    private async Task StartSessionAsync(string sdp, CancellationTokenSource owner, string ownerToken)
    {
        owner.Token.ThrowIfCancellationRequested();
        MatterCameraStartResult result;
        try
        {
            var camera = new MatterControllerCamera(controller);
            result = selectedListening
                ? await camera.TryStartSessionAsync(nodeId, selectedEndpoint,
                    new MatterCameraStreamSelection(MatterCameraStreamSelectionKind.ExplicitId, selectedStream),
                    new MatterCameraStreamSelection(MatterCameraStreamSelectionKind.ExplicitId,
                        ownedAudio?.StreamId ?? throw new InvalidOperationException("Missing owned audio allocation.")),
                    sdp, Array.Empty<MatterCameraIceServer>()).AsTask(owner.Token)
                : await camera.TryStartWithAllocatedVideoStreamAsync(nodeId, selectedEndpoint, selectedStream, sdp).AsTask(owner.Token);
        }
        catch (OperationCanceledException)
        {
            ownershipUncertain = true;
            throw;
        }
        if (!result.Succeeded)
        {
            audioProfiles.Invalidate();
            ownershipUncertain |= result.Failure.Category is MatterCameraFailureCategory.Timeout or
                MatterCameraFailureCategory.ConnectionFailure or MatterCameraFailureCategory.MalformedData or
                MatterCameraFailureCategory.Unknown;
            throw new InvalidOperationException(DescribeFailure(result.Failure));
        }
        var started = result.Session;
        if (sessionCancellation != owner || owner.IsCancellationRequested)
        {
            var cleanup = await started.TryCloseAsync();
            if (!cleanup.Succeeded)
            {
                ownershipUncertain = true;
                throw new InvalidOperationException(DescribeFailure(cleanup.Failure));
            }
            return;
        }
        session = started;
        audioDiagnostics.Text += $" Resolved protocol session={started.ProtocolSessionId}, " +
            $"video={started.VideoStreamId}, audio={started.AudioStreamId}.";
        status.Text = "Offer accepted; waiting for answer and ICE. Playback is not established.";
        signalTask = ReceiveSignalsAsync(started, ownerToken, owner.Token);
        privacyTask = MonitorPrivacyAsync(selectedEndpoint, owner.Token);
    }

    private async Task MonitorPrivacyAsync(ushort cameraEndpoint, CancellationToken cancellation)
    {
        List<AttributeSubscription> subscriptions = [];
        try
        {
            var camera = new MatterControllerCamera(controller);
            var capabilities = await camera.ReadCapabilitiesAsync(nodeId, cameraEndpoint).AsTask(cancellation);
            foreach (uint attribute in new uint[] { 20, 21 }.Where(capabilities.StreamManagement.Attributes.Contains))
            {
                cancellation.ThrowIfCancellationRequested();
                // Recover subscriptions created after Stop before closing their native handles.
                var subscription = await controller.SubscribeAttributeAsync(nodeId,
                    new AttributePath(cameraEndpoint, 0x551, attribute), 0, 5);
                subscriptions.Add(subscription);
                subscription.ReportReceived += (_, report) =>
                {
                    if (cancellation.IsCancellationRequested)
                    {
                        return;
                    }
                    bool malformed = !report.Value.Data.TryGetValue("value", out var value) || value is not bool;
                    if (malformed || value is true)
                    {
                        DispatcherQueue.TryEnqueue(async () =>
                        {
                            if (!cancellation.IsCancellationRequested)
                            {
                                microphoneArmed = false;
                                audioProfiles.Invalidate();
                                status.Text = malformed ? "Malformed privacy report; stopping media."
                                    : "Privacy report enabled; stopping media and owned allocations.";
                                await StopAsync();
                            }
                        });
                    }
                };
            }
            while (!cancellation.IsCancellationRequested)
            {
                if (await camera.IsPrivacyEnabledAsync(nodeId, cameraEndpoint).AsTask(cancellation))
                {
                    cancellation.ThrowIfCancellationRequested();
                    status.Text = "Camera privacy is enabled; stopping the owned receiver and allocation.";
                    DispatcherQueue.TryEnqueue(async () => await StopAsync());
                    return;
                }
                await Task.Delay(1000, cancellation);
            }
        }
        catch (OperationCanceledException) when (cancellation.IsCancellationRequested)
        {
            // Stop owns session End and any sample-owned deallocation.
        }
        catch (Exception error)
        {
            status.Text = $"Privacy monitoring failed: {error.Message}";
            DispatcherQueue.TryEnqueue(async () => await StopAsync());
        }
        finally
        {
            foreach (var subscription in subscriptions)
            {
                try
                {
                    await subscription.CloseAsync();
                }
                catch (Exception error)
                {
                    ownershipUncertain = true;
                    status.Text = $"Privacy subscription cleanup failed: {error.Message}";
                }
            }
        }
    }

    private async Task ReceiveSignalsAsync(MatterCameraSession ownedSession, string ownerToken, CancellationToken cancellation)
    {
        try
        {
            while (!cancellation.IsCancellationRequested)
            {
                MatterCameraSignal signal;
                try
                {
                    signal = await ownedSession.WaitForSignalAsync(30000).AsTask(cancellation);
                }
                catch (Exception error) when (error.HResult == unchecked((int)0x800705B4) && !cancellation.IsCancellationRequested)
                {
                    continue;
                }
                cancellation.ThrowIfCancellationRequested();
                if (signal.Kind == MatterCameraSignalKind.Answer)
                {
                    Send(new { type = "answer", token = ownerToken, sdp = signal.Sdp });
                    var state = await ownedSession.GetStateAsync();
                    cancellation.ThrowIfCancellationRequested();
                    status.Text = $"Signaling state: {state}. Playback requires a decoded video frame.";
                }
                else if (signal.Kind == MatterCameraSignalKind.Candidates)
                {
                    Send(new
                    {
                        type = "ice",
                        token = ownerToken,
                        candidates = signal.Candidates.Select(candidate => new
                        {
                            candidate = candidate.Candidate,
                            sdpMid = candidate.Mid.Length == 0 ? null : candidate.Mid,
                            sdpMLineIndex = candidate.LineIndex < 0 ? (int?)null : candidate.LineIndex
                        }).ToArray()
                    });
                }
                else
                {
                    audioProfiles.Invalidate();
                    status.Text = signal.Kind == MatterCameraSignalKind.End ? $"Camera ended session: {signal.EndReason}."
                        : $"Native signaling failed: 0x{signal.NativeError:x}.";
                    DispatcherQueue.TryEnqueue(async () => await StopAsync());
                    return;
                }
            }
        }
        catch (OperationCanceledException) when (cancellation.IsCancellationRequested)
        {
            // StopAsync owns cancellation and remote cleanup.
        }
        catch (Exception error)
        {
            status.Text = $"Signal delivery failed: {error.Message}";
            DispatcherQueue.TryEnqueue(async () => await StopAsync());
        }
    }

    private Task StopAsync()
    {
        if (stopTask is not null)
        {
            return stopTask;
        }
        stopTask = StopCoreAsync();
        return FinishStopAsync(stopTask);
    }

    private async Task FinishStopAsync(Task cleanup)
    {
        await cleanup;
        stopTask = null;
        start.IsEnabled = ready && !closing && !ownershipUncertain;
        mode.IsEnabled = !closing;
    }

    private async Task StopCoreAsync()
    {
        microphoneArmed = false;
        var cancellation = sessionCancellation;
        sessionCancellation = null;
        cancellation?.Cancel();
        if (ready)
        {
            try
            {
                if (preparationTask is not null)
                {
                    try
                    {
                        await preparationTask;
                    }
                    catch (OperationCanceledException)
                    {
                        // A dispatched audio allocation completes before cancellation is observed.
                    }
                    catch (Exception error)
                    {
                        status.Text = $"Audio preparation failed during cleanup: {error.Message}";
                    }
                }
                Send(new { type = "stop", token });
            }
            catch (Exception error)
            {
                status.Text = $"Receiver bridge stop failed; continuing native cleanup: {error.Message}";
                ready = false;
                try
                {
                    browser.Close();
                }
                catch (Exception closeError)
                {
                    status.Text = $"Receiver close also failed; continuing native cleanup: {closeError.Message}";
                }
            }
        }
        var ownedSession = session;
        session = null;
        try
        {
            if (allocationTask is not null)
            {
                try
                {
                    await allocationTask;
                }
                catch (Exception error)
                {
                    status.Text = $"Allocation failed during cleanup: {error.Message}";
                }
            }
            if (startTask is not null)
            {
                try
                {
                    await startTask;
                }
                catch (OperationCanceledException)
                {
                    // The start path retires the canceled native session.
                }
                catch (Exception error)
                {
                    status.Text = $"Camera start failed during cleanup: {error.Message}";
                }
                startTask = null;
            }
            if (signalTask is not null)
            {
                await signalTask;
                signalTask = null;
            }
            if (privacyTask is not null)
            {
                await privacyTask;
                privacyTask = null;
            }
            if (ownedSession is not null)
            {
                var result = await ownedSession.TryCloseAsync();
                if (!result.Succeeded)
                {
                    ownershipUncertain = true;
                    status.Text = $"Local receiver stopped; remote cleanup failed: {DescribeFailure(result.Failure)}";
                }
            }
        }
        catch (Exception error)
        {
            ownershipUncertain = true;
            status.Text = $"Local receiver stopped; remote cleanup failed: {error.Message}";
        }
        finally
        {
            if (ownedAudio is not null && !audioReleaseAttempted)
            {
                audioReleaseAttempted = true;
                try
                {
                    var result = await new MatterControllerCamera(controller).TryDeallocateAudioStreamAsync(
                        nodeId, selectedEndpoint, ownedAudio.StreamId);
                    if (result.Succeeded)
                    {
                        ownedAudio = null;
                    }
                    else
                    {
                        ownershipUncertain = true;
                        audioProfiles.Invalidate();
                        audioStatus.Text = $"Owned audio release failed; no blind retry: {DescribeFailure(result.Failure)}";
                    }
                }
                catch (Exception error)
                {
                    ownershipUncertain = true;
                    audioProfiles.Invalidate();
                    audioStatus.Text = $"Owned audio release outcome uncertain: {error.Message}";
                }
            }
            if (ownedStream is ushort streamId && !deallocationAttempted)
            {
                deallocationAttempted = true;
                try
                {
                    var result = await new MatterControllerCamera(controller).TryDeallocateVideoStreamAsync(
                        nodeId, ownedStreamEndpoint, streamId);
                    if (result.Succeeded)
                    {
                        ownedStream = null;
                        stream.Items.Clear();
                        streamEndpoint = null;
                    }
                    else
                    {
                        ownershipUncertain = true;
                        status.Text = $"Owned stream deallocation failed; do not blindly retry: {DescribeFailure(result.Failure)}";
                    }
                }
                catch (Exception error)
                {
                    ownershipUncertain = true;
                    status.Text = $"Owned stream release outcome is uncertain: {error.Message}";
                }
            }
            cancellation?.Dispose();
        }
    }

    private static string DescribeFailure(MatterCameraFailure failure) =>
        $"{failure.Stage}: {failure.Category}, HRESULT 0x{failure.HResult:x8}" +
        (failure.HasNativeError ? $", Matter 0x{failure.NativeError:x8}" : "") +
        $", retryable: {failure.Retryable}. {failure.Message}";

    private static void RequireCameraEndpoint(MatterCameraCapabilities capabilities)
    {
        if (!capabilities.Descriptor.DeviceTypes.Any(type => type.DeviceTypeId == 0x0142 && type.Revision is 1 or 2))
        {
            throw new NotSupportedException("This adapter requires a discovered Matter Camera device type, revision 1 or 2.");
        }
    }

    private void Send(object message) =>
        browser.CoreWebView2.PostWebMessageAsJson(JsonSerializer.Serialize(message));
}
#endif
