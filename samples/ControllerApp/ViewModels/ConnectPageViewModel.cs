using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using MatterControllerApp.Services;
using Microsoft.UI.Dispatching;

namespace MatterControllerApp.ViewModels;

public partial class ConnectPageViewModel : ObservableObject
{
    private readonly ControllerSession session;
    private readonly DispatcherQueue dispatcherQueue = DispatcherQueue.GetForCurrentThread();

    public ConnectPageViewModel(ControllerSession session)
    {
        this.session = session;
        session.CommissioningProgress += OnCommissioningProgress;
        session.StateChanged += OnStateChanged;
        UpdateControllerStatus();
    }

    private string sharingCode = string.Empty;
    public string SharingCode { get => sharingCode; set => SetProperty(ref sharingCode, value); }

    private uint setupPinCode = 20202021;
    public uint SetupPinCode { get => setupPinCode; set => SetProperty(ref setupPinCode, value); }

    private ushort longDiscriminator = 3840;
    public ushort LongDiscriminator
    {
        get => longDiscriminator;
        set => SetProperty(ref longDiscriminator, value);
    }

    private string status = "Initializing controller";
    public string Status { get => status; set => SetProperty(ref status, value); }

    private bool isBusy;
    public bool IsBusy { get => isBusy; set => SetProperty(ref isBusy, value); }

    [RelayCommand]
    private Task ConnectWithSharingCodeAsync() =>
        RunAsync("Commissioning with sharing code", async () =>
        {
            if (string.IsNullOrWhiteSpace(SharingCode))
            {
                throw new InvalidOperationException("Paste the sharing code from the device's current controller.");
            }
            await App.ControllerInitialization;
            await session.CommissionOnNetworkAsync(SharingCode, SetupPinCode, LongDiscriminator);
        });

    [RelayCommand]
    private Task CommissionBleAsync() =>
        RunAsync("Commissioning over Bluetooth LE", async () =>
        {
            await App.ControllerInitialization;
            await session.CommissionBleAsync(SetupPinCode, LongDiscriminator);
        });

    private async Task RunAsync(string operation, Func<Task> action)
    {
        if (IsBusy)
        {
            return;
        }

        IsBusy = true;
        Status = operation;
        try
        {
            await action();
            if (Status == operation)
            {
                Status = $"{operation} complete";
            }
        }
        catch (Exception exception)
        {
            Status = $"{operation} failed: {exception.Message}";
        }
        finally
        {
            IsBusy = false;
        }
    }

    private void OnCommissioningProgress(object? sender, string message)
    {
        if (dispatcherQueue.HasThreadAccess)
        {
            Status = message;
        }
        else
        {
            dispatcherQueue.TryEnqueue(() => Status = message);
        }
    }

    private void OnStateChanged(object? sender, EventArgs args)
    {
        if (dispatcherQueue.HasThreadAccess)
        {
            UpdateControllerStatus();
        }
        else
        {
            dispatcherQueue.TryEnqueue(UpdateControllerStatus);
        }
    }

    private void UpdateControllerStatus()
    {
        Status = session.IsInitialized
            ? "Controller ready"
            : session.InitializationError is null
                ? "Initializing controller"
                : $"Controller initialization failed: {session.InitializationError}";
    }
}
