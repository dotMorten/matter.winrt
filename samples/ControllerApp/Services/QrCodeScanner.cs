using Windows.Devices.Enumeration;
using Windows.Devices.PointOfService;
using Windows.Security.Cryptography;

namespace MatterControllerApp.Services;

public sealed class QrCodeScanner
{
    public async Task<IReadOnlyList<QrCamera>> GetCamerasAsync()
    {
        string selector = BarcodeScanner.GetDeviceSelector(PosConnectionTypes.Local);
        DeviceInformationCollection devices = await DeviceInformation.FindAllAsync(selector);
        List<QrCamera> cameras = [];
        foreach (DeviceInformation device in devices)
        {
            using BarcodeScanner? scanner = await BarcodeScanner.FromIdAsync(device.Id);
            if (!string.IsNullOrWhiteSpace(scanner?.VideoDeviceId))
            {
                DeviceInformation camera =
                    await DeviceInformation.CreateFromIdAsync(scanner.VideoDeviceId);
                string displayName = string.IsNullOrWhiteSpace(camera.Name)
                    ? device.Name
                    : camera.Name;
                cameras.Add(new QrCamera(device.Id, displayName));
            }
        }

        return cameras;
    }

    public async Task<string> ScanAsync(
        string scannerId,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(scannerId);
        using BarcodeScanner? scanner = await BarcodeScanner.FromIdAsync(scannerId);
        if (scanner is null || string.IsNullOrWhiteSpace(scanner.VideoDeviceId))
        {
            throw new InvalidOperationException(
                "The selected camera is no longer available.");
        }

        using ClaimedBarcodeScanner? claimedScanner = await scanner.ClaimScannerAsync();
        if (claimedScanner is null)
        {
            throw new InvalidOperationException("The camera barcode scanner could not be claimed.");
        }

        TaskCompletionSource<string> completion =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        int scannerClosed = 0;
        bool previewShown = false;
        bool softwareTriggerStarted = false;
        Task? previewTask = null;

        void OnDataReceived(
            ClaimedBarcodeScanner sender,
            BarcodeScannerDataReceivedEventArgs args)
        {
            string value = CryptographicBuffer.ConvertBinaryToString(
                BinaryStringEncoding.Utf8,
                args.Report.ScanDataLabel);
            completion.TrySetResult(value.TrimEnd('\0', '\r', '\n'));
        }

        void OnClosed(
            ClaimedBarcodeScanner sender,
            ClaimedBarcodeScannerClosedEventArgs args)
        {
            Interlocked.Exchange(ref scannerClosed, 1);
            completion.TrySetCanceled();
        }

        claimedScanner.IsDecodeDataEnabled = true;
        claimedScanner.IsDisabledOnDataReceived = true;
        claimedScanner.DataReceived += OnDataReceived;
        claimedScanner.Closed += OnClosed;
        using CancellationTokenRegistration cancellation =
            cancellationToken.Register(() => completion.TrySetCanceled(cancellationToken));

        try
        {
            cancellationToken.ThrowIfCancellationRequested();
            await claimedScanner.EnableAsync();
            cancellationToken.ThrowIfCancellationRequested();
            if (scanner.Capabilities.IsSoftwareTriggerSupported)
            {
                await claimedScanner.StartSoftwareTriggerAsync();
                softwareTriggerStarted = true;
            }
            cancellationToken.ThrowIfCancellationRequested();
            previewTask = claimedScanner.ShowVideoPreviewAsync().AsTask();
            previewShown = true;
            Task firstCompleted = await Task.WhenAny(completion.Task, previewTask);
            if (firstCompleted == previewTask)
            {
                await previewTask;
                completion.TrySetCanceled();
            }
            return await completion.Task;
        }
        finally
        {
            claimedScanner.DataReceived -= OnDataReceived;
            claimedScanner.Closed -= OnClosed;
            if (Volatile.Read(ref scannerClosed) == 0)
            {
                if (previewShown)
                {
                    claimedScanner.HideVideoPreview();
                    await previewTask!;
                }
                if (softwareTriggerStarted)
                {
                    await claimedScanner.StopSoftwareTriggerAsync();
                }
                if (claimedScanner.IsEnabled)
                {
                    await claimedScanner.DisableAsync();
                }
            }
        }
    }
}

public sealed record QrCamera(string ScannerId, string DisplayName);
