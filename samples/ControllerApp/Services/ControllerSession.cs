using Matter.Windows.Controller;

namespace MatterControllerApp.Services;

public sealed class ControllerSession
{
    public MatterController? Controller { get; private set; }
    public string? InitializationError { get; private set; }

    public event EventHandler<string>? CommissioningProgress;
    public event EventHandler? NodesChanged;
    public event EventHandler? StateChanged;

    public bool IsInitialized => Controller is not null;

    public async Task InitializeAsync()
    {
        if (Controller is not null)
        {
            return;
        }

        try
        {
            ControllerOptions options = new()
            {
                StoragePath = Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                    "MatterControllerApp"),
                AllowTestAttestation = true
            };
            Controller = await MatterController.CreateAsync(options);
            Controller.CommissioningProgress += OnCommissioningProgress;
            InitializationError = null;
            NodesChanged?.Invoke(this, EventArgs.Empty);
        }
        catch (Exception exception)
        {
            InitializationError = exception.Message;
        }
        finally
        {
            StateChanged?.Invoke(this, EventArgs.Empty);
        }
    }

    public async Task<CommissionedNode> CommissionOnNetworkAsync(
        string setupCode,
        uint setupPinCode,
        ushort longDiscriminator)
    {
        MatterController controller = RequireController();
        CommissionedNode node = await controller.CommissionOnNetworkAsync(new OnNetworkCommissioningParameters
        {
            NodeId = AllocateNodeId(controller),
            SetupCode = setupCode.Trim(),
            SetupPinCode = setupPinCode,
            LongDiscriminator = longDiscriminator
        });
        NodesChanged?.Invoke(this, EventArgs.Empty);
        return node;
    }

    public async Task<CommissionedNode> CommissionBleAsync(
        uint setupPinCode,
        ushort longDiscriminator)
    {
        MatterController controller = RequireController();
        CommissionedNode node = await controller.CommissionBleAsync(new BleCommissioningParameters
        {
            NodeId = AllocateNodeId(controller),
            SetupPinCode = setupPinCode,
            LongDiscriminator = longDiscriminator
        });
        NodesChanged?.Invoke(this, EventArgs.Empty);
        return node;
    }

    public async Task RemoveNodeAsync(ulong nodeId)
    {
        await RequireController().RemoveNodeAsync(nodeId);
        NodesChanged?.Invoke(this, EventArgs.Empty);
    }

    public async Task CloseAsync()
    {
        if (Controller is null)
        {
            return;
        }

        Controller.CommissioningProgress -= OnCommissioningProgress;
        await Controller.CloseAsync();
        Controller = null;
        NodesChanged?.Invoke(this, EventArgs.Empty);
        StateChanged?.Invoke(this, EventArgs.Empty);
    }

    public MatterController RequireController() =>
        Controller ?? throw new InvalidOperationException("Initialize the controller on the Connect page first.");

    private void OnCommissioningProgress(MatterController sender, CommissioningProgressEventArgs args) =>
        CommissioningProgress?.Invoke(this, $"{args.Stage}: {args.Message}");

    private static ulong AllocateNodeId(MatterController controller)
    {
        HashSet<ulong> existing = controller.CommissionedNodes.Select(node => node.NodeId).ToHashSet();
        for (ulong candidate = 1; candidate != 0; candidate++)
        {
            if (!existing.Contains(candidate))
            {
                return candidate;
            }
        }
        throw new InvalidOperationException("No Matter node identifiers are available.");
    }
}
