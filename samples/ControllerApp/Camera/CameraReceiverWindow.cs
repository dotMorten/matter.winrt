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
    private bool ownershipUncertain;

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
        controls.Children.Add(start);
        controls.Children.Add(stop);
        controls.Children.Add(status);
        root.Children.Add(controls);
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
        // A dispatched allocation must finish so Stop can recover and release its reference.
        var result = await new MatterControllerCamera(controller).TryAllocateVideoStreamAsync(
            nodeId, cameraEndpoint, new MatterCameraVideoStreamOptions());
        if (!result.Succeeded)
        {
            ownershipUncertain = result.Failure.Stage == MatterCameraStage.StreamDeallocation ||
                (result.Failure.Stage == MatterCameraStage.StreamAllocation &&
                 result.Failure.Category is MatterCameraFailureCategory.Timeout or
                    MatterCameraFailureCategory.ConnectionFailure or MatterCameraFailureCategory.MalformedData or
                    MatterCameraFailureCategory.Unknown);
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
        status.Text = "Owned stream allocated. Start the receiver; Stop ends the session and releases this reference.";
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
            browser.CoreWebView2.PermissionRequested += (_, permission) => permission.State = CoreWebView2PermissionState.Deny;
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

    private void OnStart(object sender, RoutedEventArgs args)
    {
        if (!ready || closing || stopTask is not null || allocationTask is not null || ownershipUncertain ||
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
        token = Guid.NewGuid().ToString("N");
        start.IsEnabled = false;
        status.Text = "Generating a video-only offer. Microphone and recording are disabled.";
        Send(new
        {
            type = "start", token, videoCodec = selectedCodec,
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
                else if (type == "media")
                {
                    string? state = message.GetProperty("state").GetString();
                    status.Text = state == "playing" ? "Live video frame decoded; playback established."
                        : $"Media connectivity failed: {state}. Signaling may still be established.";
                    if (state is not ("playing" or "disconnected" or "failed"))
                    {
                        throw new InvalidOperationException("Unexpected media state.");
                    }
                    if (state == "failed")
                    {
                        await StopAsync();
                    }
                }
                else if (type == "error")
                {
                    throw new InvalidOperationException(message.GetProperty("message").GetString() ?? "Receiver failed.");
                }
                else
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
                status.Text = $"Camera stage failed: {error.Message}";
                await StopAsync();
            }
        }
    }

    private async Task StartSessionAsync(string sdp, CancellationTokenSource owner, string ownerToken)
    {
        var result = await new MatterControllerCamera(controller).TryStartWithAllocatedVideoStreamAsync(
            nodeId, selectedEndpoint, selectedStream, sdp).AsTask(owner.Token);
        if (!result.Succeeded)
        {
            throw new InvalidOperationException(DescribeFailure(result.Failure));
        }
        var started = result.Session;
        if (sessionCancellation != owner || owner.IsCancellationRequested)
        {
            var cleanup = await started.TryCloseAsync();
            if (!cleanup.Succeeded)
            {
                throw new InvalidOperationException(DescribeFailure(cleanup.Failure));
            }
            return;
        }
        session = started;
        status.Text = "Offer accepted; waiting for answer and ICE. Playback is not established.";
        signalTask = ReceiveSignalsAsync(started, ownerToken, owner.Token);
        privacyTask = MonitorPrivacyAsync(selectedEndpoint, owner.Token);
    }

    private async Task MonitorPrivacyAsync(ushort cameraEndpoint, CancellationToken cancellation)
    {
        try
        {
            var camera = new MatterControllerCamera(controller);
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
    }

    private async Task StopCoreAsync()
    {
        var cancellation = sessionCancellation;
        sessionCancellation = null;
        cancellation?.Cancel();
        if (ready)
        {
            try
            {
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
                    status.Text = $"Local receiver stopped; remote cleanup failed: {DescribeFailure(result.Failure)}";
                }
            }
        }
        catch (Exception error)
        {
            status.Text = $"Local receiver stopped; remote cleanup failed: {error.Message}";
        }
        finally
        {
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
                        status.Text = $"Owned stream release failed; do not blindly retry: {DescribeFailure(result.Failure)}";
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

    private void Send(object message) =>
        browser.CoreWebView2.PostWebMessageAsJson(JsonSerializer.Serialize(message));
}
#endif
