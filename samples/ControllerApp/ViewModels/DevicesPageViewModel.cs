using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Matter.Windows.Controller;
using MatterControllerApp.Services;
using Microsoft.UI.Dispatching;
using System.Collections;
using System.Collections.ObjectModel;
using System.Globalization;

namespace MatterControllerApp.ViewModels;

public partial class DevicesPageViewModel : ObservableObject
{
    private readonly ControllerSession session;
    private readonly DispatcherQueue dispatcherQueue = DispatcherQueue.GetForCurrentThread();

    public DevicesPageViewModel(ControllerSession session)
    {
        this.session = session;
        session.NodesChanged += OnNodesChanged;
    }

    public ObservableCollection<CommissionedNode> CommissionedNodes { get; } = [];

    private CommissionedNode? selectedNode;
    public CommissionedNode? SelectedNode
    {
        get => selectedNode;
        set
        {
            if (SetProperty(ref selectedNode, value) && value is not null)
            {
                Status = $"Selected node {value.NodeId}.";
            }
        }
    }

    private ushort endpointId = 1;
    public ushort EndpointId { get => endpointId; set => SetProperty(ref endpointId, value); }

    private string clusterId = "6";
    public string ClusterId { get => clusterId; set => SetProperty(ref clusterId, value); }

    private string attributeId = "0";
    public string AttributeId { get => attributeId; set => SetProperty(ref attributeId, value); }

    private bool isOn;
    public bool IsOn { get => isOn; set => SetProperty(ref isOn, value); }

    private double level = 128;
    public double Level { get => level; set => SetProperty(ref level, value); }

    private string deviceInformation = "Select a persisted node and read its capabilities.";
    public string DeviceInformation
    {
        get => deviceInformation;
        set => SetProperty(ref deviceInformation, value);
    }

    private string queryResult = "No generic attribute queried.";
    public string QueryResult { get => queryResult; set => SetProperty(ref queryResult, value); }

    private string status = "Open the Connect page to initialize the controller.";
    public string Status { get => status; set => SetProperty(ref status, value); }

    private bool isBusy;
    public bool IsBusy { get => isBusy; set => SetProperty(ref isBusy, value); }

    [RelayCommand]
    public void RefreshNodes()
    {
        ulong? selectedId = SelectedNode?.NodeId;
        CommissionedNodes.Clear();
        if (!session.IsInitialized)
        {
            Status = "Open the Connect page to initialize the controller.";
            return;
        }

        foreach (CommissionedNode node in session.RequireController().CommissionedNodes)
        {
            CommissionedNodes.Add(node);
        }
        SelectedNode = CommissionedNodes.FirstOrDefault(node => node.NodeId == selectedId) ??
            CommissionedNodes.FirstOrDefault();
        Status = $"{CommissionedNodes.Count} persisted device(s).";
    }

    [RelayCommand]
    private Task ReadDeviceAsync() =>
        RunAsync("Reading device capabilities", async () =>
        {
            MatterController controller = session.RequireController();
            ulong id = ParseNodeId();
            List<string> details = [];

            try
            {
                BasicInformation information = await controller.GetBasicInformationCluster(id, 0).ReadAsync();
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
                IsOn = await controller.GetOnOffCluster(id, EndpointId).ReadAsync();
                details.Add($"On/Off: {(IsOn ? "On" : "Off")}");
            }
            catch (Exception exception)
            {
                details.Add($"On/Off unavailable on endpoint {EndpointId}: {exception.Message}");
            }

            try
            {
                Level = await controller.GetLevelControlCluster(id, EndpointId).ReadCurrentLevelAsync();
                details.Add($"Current level: {Level:0}");
            }
            catch (Exception exception)
            {
                details.Add($"Level Control unavailable on endpoint {EndpointId}: {exception.Message}");
            }

            DeviceInformation = string.Join("\n", details);
        });

    [RelayCommand]
    private Task ReadAttributeAsync() =>
        RunAsync("Reading generic attribute", async () =>
        {
            AttributePath path = new(EndpointId, ParseIdentifier(ClusterId, "Cluster ID"), ParseIdentifier(AttributeId, "Attribute ID"));
            AttributeValue value = await session.RequireController().ReadAttributeAsync(ParseNodeId(), path);
            QueryResult = FormatValue(value.Data);
        });

    [RelayCommand]
    private Task ToggleAsync() =>
        RunAsync("Toggling On/Off", async () =>
        {
            OnOffCluster cluster = session.RequireController().GetOnOffCluster(ParseNodeId(), EndpointId);
            await cluster.ToggleAsync();
            IsOn = await cluster.ReadAsync();
        });

    [RelayCommand]
    private Task SetLevelAsync() =>
        RunAsync("Setting level", async () =>
        {
            byte target = checked((byte)Math.Round(Level));
            await session.RequireController().GetLevelControlCluster(ParseNodeId(), EndpointId)
                .MoveToLevelAsync(target, 0, 0, 0);
        });

    [RelayCommand]
    private Task RemoveNodeAsync() =>
        RunAsync("Removing node", async () =>
        {
            await session.RemoveNodeAsync(ParseNodeId());
            RefreshNodes();
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
            Status = $"{operation} complete";
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

    private ulong ParseNodeId() =>
        SelectedNode?.NodeId ?? throw new InvalidOperationException("Select a persisted device.");

    private static uint ParseIdentifier(string text, string name)
    {
        NumberStyles style = text.StartsWith("0x", StringComparison.OrdinalIgnoreCase)
            ? NumberStyles.HexNumber
            : NumberStyles.Integer;
        string value = style == NumberStyles.HexNumber ? text[2..] : text;
        return uint.TryParse(value, style, CultureInfo.InvariantCulture, out uint result)
            ? result
            : throw new InvalidOperationException($"{name} must be a decimal or 0x-prefixed identifier.");
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
                field => $"{childIndent}{field.Key}: {FormatValue(field.Value, depth + 1)}")) + $"\n{indent}}}";
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
        return Convert.ToString(value, CultureInfo.InvariantCulture) ?? value.ToString() ?? string.Empty;
    }

    private void OnNodesChanged(object? sender, EventArgs args)
    {
        if (dispatcherQueue.HasThreadAccess)
        {
            RefreshNodes();
        }
        else
        {
            dispatcherQueue.TryEnqueue(RefreshNodes);
        }
    }
}
