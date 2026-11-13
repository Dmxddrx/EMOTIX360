
namespace EMOTIX360.App;

public partial class App : Application
{
    public App()
    {
        InitializeComponent();

        // EMOTIX360 uses a dark dashboard by default.
        UserAppTheme = AppTheme.Dark;
    }

    // Create the main application window.
    // AppShell remains the root navigation interface.
    protected override Window CreateWindow(
        IActivationState? activationState)
    {
        return new Window(new AppShell());
    }
}
