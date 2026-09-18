using MatterControllerApp.ViewModels;
using Microsoft.UI.Xaml.Controls;

namespace MatterControllerApp.Pages;

public sealed partial class ConnectPage : Page
{
    public ConnectPageViewModel ViewModel { get; } = new(App.ControllerSession);

    public ConnectPage()
    {
        InitializeComponent();
    }
}
