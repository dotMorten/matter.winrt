using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using MatterControllerApp.Pages;
using WinUIEx;

// To learn more about WinUI, the WinUI project structure,
// and more about our project templates, see: http://aka.ms/winui-project-info.

namespace MatterControllerApp;

/// <summary>
/// The application window. This hosts a Frame that displays pages. Add your
/// UI and logic to MainPage.xaml / MainPage.xaml.cs instead of here so you
/// can use Page features such as navigation events and the Loaded lifecycle.
/// </summary>
public sealed partial class MainWindow : Window
{
    public MainWindow()
    {
        InitializeComponent();

        ExtendsContentIntoTitleBar = true;
        SetTitleBar(AppTitleBar);

        AppWindow.SetIcon("Assets/AppIcon.ico");
        WindowManager windowManager = WindowManager.Get(this);
        windowManager.Width = 1200;
        windowManager.Height = 800;
        windowManager.MinWidth = 900;
        windowManager.MinHeight = 640;
        windowManager.PersistenceId = "MatterControllerMainWindow";
        AppWindow.Closing += OnWindowClosing;

        MainNavigation.SelectedItem = ConnectNavigationItem;
        RootFrame.Navigate(typeof(ConnectPage));
    }

    private bool closeControllerComplete;

    private async void OnWindowClosing(Microsoft.UI.Windowing.AppWindow sender,
                                       Microsoft.UI.Windowing.AppWindowClosingEventArgs args)
    {
        if (closeControllerComplete)
        {
            return;
        }

        args.Cancel = true;
        try
        {
            await App.ControllerSession.CloseAsync();
        }
        finally
        {
            closeControllerComplete = true;
            Close();
        }
    }

    private void OnSelectionChanged(NavigationView sender, NavigationViewSelectionChangedEventArgs args)
    {
        Type pageType = (args.SelectedItemContainer?.Tag as string) switch
        {
            "devices" => typeof(DevicesPage),
            _ => typeof(ConnectPage)
        };
        if (RootFrame.CurrentSourcePageType != pageType)
        {
            RootFrame.Navigate(pageType);
        }
    }
}
