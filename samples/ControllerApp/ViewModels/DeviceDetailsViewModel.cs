using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Matter.Windows.Controller;
using MatterControllerApp.Models;
using MatterControllerApp.Services;
using System.Collections;
using System.Globalization;

namespace MatterControllerApp.ViewModels;

public partial class DeviceDetailsViewModel : ObservableObject
{
    private readonly ControllerSession session;
    private readonly WidgetSelectionStore widgetSelectionStore = new();

    public DeviceDetailsViewModel(ControllerSession session)
    {
        this.session = session;
    }

    private KnownDevice? device;
    public KnownDevice? Device
    {
        get => device;
        private set
        {
            if (SetProperty(ref device, value))
            {
                OnPropertyChanged(nameof(DisplayName));
            }
        }
    }

    public string DisplayName => Device?.DisplayName ?? "Device";

    private ushort endpointId = 1;
    public ushort EndpointId
    {
        get => endpointId;
        set
        {
            if (SetProperty(ref endpointId, value))
            {
                UpdateWidgetSelectionState();
            }
        }
    }

    private string clusterId = "6";
    public string ClusterId
    {
        get => clusterId;
        set => SetProperty(ref clusterId, value);
    }

    private string attributeId = "0";
    public string AttributeId
    {
        get => attributeId;
        set => SetProperty(ref attributeId, value);
    }

    private bool isOn;
    public bool IsOn
    {
        get => isOn;
        private set => SetProperty(ref isOn, value);
    }

    private double level = 128;
    public double Level
    {
        get => level;
        set => SetProperty(ref level, value);
    }

    private bool hasOnOff;
    public bool HasOnOff
    {
        get => hasOnOff;
        private set => SetProperty(ref hasOnOff, value);
    }

    private bool isInWidget;
    public bool IsInWidget
    {
        get => isInWidget;
        private set => SetProperty(ref isInWidget, value);
    }

    private bool hasLevelControl;
    public bool HasLevelControl
    {
        get => hasLevelControl;
        private set => SetProperty(ref hasLevelControl, value);
    }

    private string deviceInformation = "Reading device information…";
    public string DeviceInformation
    {
        get => deviceInformation;
        private set => SetProperty(ref deviceInformation, value);
    }

    private string queryResult = "No attribute has been queried.";
    public string QueryResult
    {
        get => queryResult;
        private set => SetProperty(ref queryResult, value);
    }

    private string status = "Loading device state…";
    public string Status
    {
        get => status;
        private set => SetProperty(ref status, value);
    }

    private bool isBusy;
    public bool IsBusy
    {
        get => isBusy;
        private set => SetProperty(ref isBusy, value);
    }

    public void Initialize(KnownDevice knownDevice)
    {
        Device = knownDevice;
        UpdateWidgetSelectionState();
    }

    public async Task LoadAsync()
    {
        if (Device is null || IsBusy)
        {
            return;
        }

        IsBusy = true;
        Status = "Reading device state…";
        List<string> details = [];

        try
        {
            MatterController controller = session.RequireController();
            try
            {
                BasicInformation information =
                    await controller.GetBasicInformationCluster(Device.NodeId, 0).ReadAsync();
                details.Add(
                    $"{information.VendorName} {information.ProductName}\n" +
                    $"VID 0x{information.VendorId:X4}, PID 0x{information.ProductId:X4}\n" +
                    $"Serial {information.SerialNumber}\n" +
                    $"Software {information.SoftwareVersionString}");
            }
            catch (Exception exception)
            {
                details.Add($"Basic Information unavailable: {exception.Message}");
            }

            try
            {
                IsOn = await controller.GetOnOffCluster(Device.NodeId, EndpointId).ReadAsync();
                HasOnOff = true;
                details.Add($"On/Off: {(IsOn ? "On" : "Off")}");
            }
            catch (Exception exception)
            {
                HasOnOff = false;
                details.Add($"On/Off unavailable on endpoint {EndpointId}: {exception.Message}");
            }

            try
            {
                Level = await controller.GetLevelControlCluster(Device.NodeId, EndpointId)
                    .ReadCurrentLevelAsync();
                HasLevelControl = true;
                details.Add($"Current level: {Level:0}");
            }
            catch (Exception exception)
            {
                HasLevelControl = false;
                details.Add($"Level Control unavailable on endpoint {EndpointId}: {exception.Message}");
            }

            DeviceInformation = string.Join("\n\n", details);
            Status = "Device state updated.";
        }
        catch (Exception exception)
        {
            DeviceInformation = $"Device state could not be read: {exception.Message}";
            Status = "Could not load the device.";
        }
        finally
        {
            IsBusy = false;
        }
    }

    [RelayCommand]
    private Task RefreshAsync() => LoadAsync();

    [RelayCommand]
    private Task ReadAttributeAsync() =>
        RunAsync("Reading attribute", async () =>
        {
            AttributePath path = new(
                EndpointId,
                ParseIdentifier(ClusterId, "Cluster ID"),
                ParseIdentifier(AttributeId, "Attribute ID"));
            AttributeValue value =
                await session.RequireController().ReadAttributeAsync(RequireNodeId(), path);
            QueryResult = FormatValue(value.Data);
        });

    [RelayCommand]
    private Task ToggleAsync() =>
        RunAsync("Toggling device", async () =>
        {
            OnOffCluster cluster =
                session.RequireController().GetOnOffCluster(RequireNodeId(), EndpointId);
            await cluster.ToggleAsync();
            IsOn = await cluster.ReadAsync();
        });

    [RelayCommand]
    private Task SetLevelAsync() =>
        RunAsync("Setting level", async () =>
        {
            byte target = checked((byte)Math.Round(Level));
            await session.RequireController().GetLevelControlCluster(RequireNodeId(), EndpointId)
                .MoveToLevelAsync(target, 0, 0, 0);
        });

    public Task SetWidgetSelectionAsync(bool include)
    {
        if (Device is null || !HasOnOff)
        {
            OnPropertyChanged(nameof(IsInWidget));
            return Task.CompletedTask;
        }

        if (include == IsInWidget)
        {
            return Task.CompletedTask;
        }

        try
        {
            if (include)
            {
                widgetSelectionStore.Add(new WidgetDevice(
                    Device.NodeId,
                    Device.FabricIndex,
                    EndpointId,
                    Device.DisplayName,
                    IsOn));
                Status =
                    $"{Device.DisplayName} was added to Matter Controls. " +
                    "Pin the widget from the Windows Widgets picker if needed.";
            }
            else
            {
                widgetSelectionStore.Remove(
                    Device.FabricIndex,
                    Device.NodeId,
                    EndpointId);
                Status = $"{Device.DisplayName} was removed from Matter Controls.";
            }
            IsInWidget = include;
        }
        catch (Exception exception)
        {
            Status = $"Could not update Matter Controls: {exception.Message}";
            OnPropertyChanged(nameof(IsInWidget));
        }

        return Task.CompletedTask;
    }

    public async Task<bool> RemoveAsync()
    {
        if (IsBusy)
        {
            return false;
        }

        IsBusy = true;
        Status = "Removing device…";
        try
        {
            await session.RemoveNodeAsync(RequireNodeId());
            Status = "Device removed.";
            return true;
        }
        catch (Exception exception)
        {
            Status = $"Could not remove the device: {exception.Message}";
            return false;
        }
        finally
        {
            IsBusy = false;
        }
    }

    private async Task RunAsync(string operation, Func<Task> action)
    {
        if (IsBusy)
        {
            return;
        }

        IsBusy = true;
        Status = $"{operation}…";
        try
        {
            await action();
            Status = $"{operation} complete.";
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

    private ulong RequireNodeId() =>
        Device?.NodeId ?? throw new InvalidOperationException("No device was selected.");

    private static uint ParseIdentifier(string text, string name)
    {
        NumberStyles style = text.StartsWith("0x", StringComparison.OrdinalIgnoreCase)
            ? NumberStyles.HexNumber
            : NumberStyles.Integer;
        string value = style == NumberStyles.HexNumber ? text[2..] : text;
        return uint.TryParse(value, style, CultureInfo.InvariantCulture, out uint result)
            ? result
            : throw new InvalidOperationException(
                $"{name} must be a decimal or 0x-prefixed identifier.");
    }

    private static string FormatValue(object? value, int depth = 0)
    {
        if (value is null)
        {
            return "null";
        }
        if (value is string text)
        {
            return $"\"{text}\"";
        }
        if (value is IEnumerable<KeyValuePair<string, object>> fields)
        {
            string indent = new(' ', depth * 2);
            string childIndent = new(' ', (depth + 1) * 2);
            return "{\n" + string.Join(",\n", fields.Select(
                field => $"{childIndent}{field.Key}: {FormatValue(field.Value, depth + 1)}")) +
                $"\n{indent}}}";
        }
        if (value is IEnumerable sequence)
        {
            List<string> items = [];
            foreach (object? item in sequence)
            {
                items.Add(FormatValue(item, depth + 1));
            }
            return $"[{string.Join(", ", items)}]";
        }
        return Convert.ToString(value, CultureInfo.InvariantCulture) ??
            value.ToString() ??
            string.Empty;
    }

    private void UpdateWidgetSelectionState()
    {
        IsInWidget = Device is not null &&
            widgetSelectionStore.Contains(Device.NodeId, EndpointId);
    }
}
