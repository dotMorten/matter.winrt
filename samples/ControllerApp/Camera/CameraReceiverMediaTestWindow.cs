#if MATTER_CAMERA_MEDIA_TEST
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.Web.WebView2.Core;
using System.Text.Json;

namespace MatterControllerApp.Camera;

internal sealed class CameraReceiverMediaTestWindow : Window
{
    private const string ReceiverUri = "https://matter-camera.local/receiver.html";
    private readonly WebView2 browser = new();
    private readonly string resultPath;
    private bool completed;
    private int permissionRequests;

    internal CameraReceiverMediaTestWindow(string resultPath)
    {
        this.resultPath = resultPath;
        Title = "Synthetic camera media test - no microphone or camera access";
        Content = browser;
        browser.Loaded += Run;
    }

    private async void Run(object sender, RoutedEventArgs args)
    {
        try
        {
            var environment = await CoreWebView2Environment.CreateWithOptionsAsync(null,
                Path.Combine(Path.GetDirectoryName(resultPath)!, "webview-profile"),
                new CoreWebView2EnvironmentOptions
                {
                    AdditionalBrowserArguments = "--autoplay-policy=no-user-gesture-required"
                });
            await browser.EnsureCoreWebView2Async(environment);
            browser.CoreWebView2.SetVirtualHostNameToFolderMapping("matter-camera.local",
                Path.Combine(AppContext.BaseDirectory, "Camera"), CoreWebView2HostResourceAccessKind.Deny);
            browser.CoreWebView2.Settings.AreDevToolsEnabled = false;
            browser.CoreWebView2.PermissionRequested += (_, permission) =>
            {
                ++permissionRequests;
                permission.State = CoreWebView2PermissionState.Deny;
                permission.SavesInProfile = false;
            };
            browser.CoreWebView2.NewWindowRequested += (_, request) => request.Handled = true;
            browser.CoreWebView2.DownloadStarting += (_, request) => request.Cancel = true;
            browser.CoreWebView2.NavigationStarting += (_, request) => request.Cancel = request.Uri != ReceiverUri;
            browser.CoreWebView2.WebMessageReceived += OnMessage;
            browser.CoreWebView2.NavigationCompleted += async (_, navigation) =>
            {
                try
                {
                    if (!navigation.IsSuccess)
                    {
                        throw new InvalidOperationException("Synthetic receiver content failed to load.");
                    }
                    string script = await File.ReadAllTextAsync(Path.Combine(AppContext.BaseDirectory,
                        "Camera", "camera-receiver-media-test.js"));
                    await browser.ExecuteScriptAsync(script);
                }
                catch (Exception error)
                {
                    await CompleteAsync(false, error.Message, null);
                }
            };
            browser.Source = new Uri(ReceiverUri);
        }
        catch (Exception error)
        {
            await CompleteAsync(false, error.Message, null);
        }
    }

    private async void OnMessage(CoreWebView2 sender, CoreWebView2WebMessageReceivedEventArgs args)
    {
        if (completed)
        {
            return;
        }
        try
        {
            if (args.Source != ReceiverUri || System.Text.Encoding.UTF8.GetByteCount(args.WebMessageAsJson) > 24576)
            {
                throw new InvalidOperationException("Invalid synthetic receiver bridge origin or bounds.");
            }
            using var message = JsonDocument.Parse(args.WebMessageAsJson);
            var root = message.RootElement;
            if (root.GetProperty("type").GetString() == "loopback-result")
            {
                await CompleteAsync(root.GetProperty("succeeded").GetBoolean(),
                    root.GetProperty("message").GetString() ?? "Missing test result.",
                    root.GetProperty("details").Clone());
            }
            else
            {
                await browser.ExecuteScriptAsync($"window.cameraMediaTest.accept({args.WebMessageAsJson});");
            }
        }
        catch (Exception error)
        {
            await CompleteAsync(false, error.Message, null);
        }
    }

    private async Task CompleteAsync(bool succeeded, string message, JsonElement? details)
    {
        if (completed)
        {
            return;
        }
        completed = true;
        succeeded &= permissionRequests == 0;
        Environment.ExitCode = succeeded ? 0 : 1;
        try
        {
            await File.WriteAllTextAsync(resultPath, JsonSerializer.Serialize(new
            {
                succeeded, message, details, permissionRequests, synthetic = true,
                physicalAudibilityValidated = false
            }));
        }
        finally
        {
            browser.Close();
            Close();
            Application.Current.Exit();
        }
    }
}
#endif
