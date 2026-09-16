using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Matter.Windows.Controller;
using Microsoft.UI.Dispatching;
using System.Collections.ObjectModel;

namespace MatterControllerApp.ViewModels;

public partial class MainPageViewModel : ObservableObject
{
    private readonly DispatcherQueue dispatcherQueue = DispatcherQueue.GetForCurrentThread();
    private MatterController? controller;

    private string storagePath =
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "MatterControllerApp");
    public string StoragePath { get => storagePath; set => SetProperty(ref storagePath, value); }

    private string nodeId = "1";
    public string NodeId { get => nodeId; set => SetProperty(ref nodeId, value); }

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

    private ushort endpointId = 1;
    public ushort EndpointId { get => endpointId; set => SetProperty(ref endpointId, value); }

    private string status = "Controller is closed";
    public string Status { get => status; set => SetProperty(ref status, value); }

    private bool isBusy;
    public bool IsBusy { get => isBusy; set => SetProperty(ref isBusy, value); }

    private bool isOn;
    public bool IsOn { get => isOn; set => SetProperty(ref isOn, value); }

    private double level = 128;
    public double Level { get => level; set => SetProperty(ref level, value); }

    private string deviceInformation = "No device information loaded";
    public string DeviceInformation
    {
        get => deviceInformation;
        set => SetProperty(ref deviceInformation, value);
    }

    public ObservableCollection<CommissionedNode> CommissionedNodes { get; } = [];

    [RelayCommand]
    private async Task InitializeAsync()
    {
        if (controller is not null)
        {
            Status = "Controller ready";
            return;
        }

        await RunAsync("Initializing controller", async () =>
        {
            ControllerOptions options = new()
            {
                StoragePath = Path.GetFullPath(StoragePath),
                AllowTestAttestation = true
            };
            controller = await MatterController.CreateAsync(options);
            controller.CommissioningProgress += OnCommissioningProgress;
            RefreshCommissionedNodes();
            Status = "Controller ready";
        });
    }

    [RelayCommand]
    private async Task CommissionOnNetworkAsync()
    {
        await RunAsync("Commissioning over the network", async () =>
        {
            MatterController current = RequireController();
            OnNetworkCommissioningParameters parameters = new()
            {
                NodeId = ParseNodeId(),
                SetupCode = SharingCode.Trim(),
                SetupPinCode = SetupPinCode,
                LongDiscriminator = LongDiscriminator
            };
            await current.CommissionOnNetworkAsync(parameters);
            RefreshCommissionedNodes();
        });
    }

    [RelayCommand]
    private async Task CommissionBleAsync()
    {
        await RunAsync("Commissioning over Bluetooth LE", async () =>
        {
            MatterController current = RequireController();
            BleCommissioningParameters parameters = new()
            {
                NodeId = ParseNodeId(),
                SetupPinCode = SetupPinCode,
                LongDiscriminator = LongDiscriminator
            };
            await current.CommissionBleAsync(parameters);
            RefreshCommissionedNodes();
        });
    }

    [RelayCommand]
    private async Task RefreshDeviceAsync()
    {
        await RunAsync("Reading device state", async () =>
        {
            MatterController current = RequireController();
            ulong nodeId = ParseNodeId();
            List<string> details = [];

            try
            {
                BasicInformation information = await current.GetBasicInformationCluster(nodeId, 0).ReadAsync();
                details.Add(
                    $"{information.VendorName} {information.ProductName}\n" +
                    $"VID 0x{information.VendorId:X4}, PID 0x{information.ProductId:X4}\n" +
                    $"Serial {information.SerialNumber}\nSoftware {information.SoftwareVersionString}");
            }
            catch (Exception exception)
            {
                details.Add($"Basic Information unavailable: {exception.Message}");
            }

            try
            {
                IsOn = await current.GetOnOffCluster(nodeId, EndpointId).ReadAsync();
                details.Add($"On/Off: {(IsOn ? "On" : "Off")}");
            }
            catch (Exception exception)
            {
                details.Add($"On/Off unavailable on endpoint {EndpointId}: {exception.Message}");
            }

            try
            {
                Level = await current.GetLevelControlCluster(nodeId, EndpointId).ReadCurrentLevelAsync();
                details.Add($"Current level: {Level:0}");
            }
            catch (Exception exception)
            {
                details.Add($"Level Control unavailable on endpoint {EndpointId}: {exception.Message}");
            }

            DeviceInformation = string.Join("\n", details);
        });
    }

    [RelayCommand]
    private async Task ToggleAsync()
    {
        await RunAsync("Toggling On/Off", async () =>
        {
            OnOffCluster cluster = RequireController().GetOnOffCluster(ParseNodeId(), EndpointId);
            await cluster.ToggleAsync();
            IsOn = await cluster.ReadAsync();
        });
    }

    [RelayCommand]
    private async Task SetLevelAsync()
    {
        await RunAsync("Setting level", async () =>
        {
            byte level = checked((byte)Math.Round(Level));
            await RequireController().GetLevelControlCluster(ParseNodeId(), EndpointId)
                .MoveToLevelAsync(level, 0, 0, 0);
        });
    }

    [RelayCommand]
    private async Task CloseAsync()
    {
        if (controller is null)
        {
            return;
        }

        await RunAsync("Closing controller", async () =>
        {
            controller.CommissioningProgress -= OnCommissioningProgress;
            await controller.CloseAsync();
            controller = null;
            CommissionedNodes.Clear();
            Status = "Controller is closed";
        });
    }

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

    private MatterController RequireController() =>
        controller ?? throw new InvalidOperationException("Initialize the controller first.");

    private ulong ParseNodeId() =>
        ulong.TryParse(NodeId, out ulong value) && value != 0
            ? value
            : throw new InvalidOperationException("Node ID must be a nonzero unsigned integer.");

    private void RefreshCommissionedNodes()
    {
        CommissionedNodes.Clear();
        foreach (CommissionedNode node in RequireController().CommissionedNodes)
        {
            CommissionedNodes.Add(node);
        }
    }

    private void OnCommissioningProgress(MatterController sender, CommissioningProgressEventArgs args)
    {
        string message = $"{args.Stage}: {args.Message}";
        if (dispatcherQueue.HasThreadAccess)
        {
            Status = message;
        }
        else
        {
            dispatcherQueue.TryEnqueue(() => Status = message);
        }
    }
}
