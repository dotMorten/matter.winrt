using MatterControllerApp.ViewModels;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace MatterControllerApp.Pages;

public sealed partial class DevicesPage : Page
{
    public DevicesPageViewModel ViewModel { get; } = new(App.ControllerSession);

    public DevicesPage()
    {
        InitializeComponent();
        Loaded += OnLoaded;
    }

    private void OnLoaded(object sender, RoutedEventArgs args) => ViewModel.RefreshNodes();

    private async void OnRemoveNodeClicked(object sender, RoutedEventArgs args)
    {
        if (ViewModel.SelectedNode is null)
        {
            return;
        }

        ContentDialog confirmation = new()
        {
            XamlRoot = XamlRoot,
            Title = "Remove Matter node?",
            Content = $"Node {ViewModel.SelectedNode.NodeId} will be removed from this controller fabric.",
            PrimaryButtonText = "Remove",
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.Close
        };
        if (await confirmation.ShowAsync() == ContentDialogResult.Primary)
        {
            await ViewModel.RemoveNodeCommand.ExecuteAsync(null);
        }
    }
}
