using EMOTIX360.App.Services;
using EMOTIX360.App.ViewModels;
using EMOTIX360.App.Views;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;

namespace EMOTIX360.App;

public static class MauiProgram
{
    public static MauiApp CreateMauiApp()
    {
        var builder = MauiApp.CreateBuilder();

        builder.UseMauiApp<App>();

        // BLE transport and protocol services are singletons so all pages
        // observe the same connection and status stream.
        builder.Services.AddSingleton<BluetoothService>();
        builder.Services.AddSingleton<DeviceCommandService>();
        builder.Services.AddSingleton<DeviceStatusService>();

        // Shared view-model state is retained while navigating between tabs.
        builder.Services.AddSingleton<HomeViewModel>();
        builder.Services.AddSingleton<DeviceViewModel>();

        // Pages are resolved by AppShell's DataTemplate factories.
        builder.Services.AddSingleton<HomePage>();
        builder.Services.AddSingleton<DevicePage>();
        builder.Services.AddSingleton<SettingsPage>();
        builder.Services.AddSingleton<AppShell>();

#if DEBUG
        builder.Logging.AddDebug();
#endif

        return builder.Build();
    }
}
