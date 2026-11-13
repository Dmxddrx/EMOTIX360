using EMOTIX360.App.Views;
using Microsoft.Extensions.DependencyInjection;

namespace EMOTIX360.App;

public partial class AppShell : Shell
{
    public AppShell(IServiceProvider services)
    {
        InitializeComponent();

        // Resolve pages from MAUI dependency injection when Shell creates a tab.
        // This allows each page to receive its ViewModel through its constructor.
        HomeShellContent.ContentTemplate =
            new DataTemplate(() => services.GetRequiredService<HomePage>());

        DeviceShellContent.ContentTemplate =
            new DataTemplate(() => services.GetRequiredService<DevicePage>());

        SettingsShellContent.ContentTemplate =
            new DataTemplate(() => services.GetRequiredService<SettingsPage>());
    }
}
