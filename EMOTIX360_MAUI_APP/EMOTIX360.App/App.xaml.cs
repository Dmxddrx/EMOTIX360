namespace EMOTIX360.App;

public partial class App : Application
{
    private readonly AppShell _appShell;

    public App(AppShell appShell)
    {
        InitializeComponent();

        // EMOTIX360 uses a dark dashboard by default.
        UserAppTheme = AppTheme.Dark;

        _appShell = appShell;
    }

    protected override Window CreateWindow(IActivationState? activationState)
    {
        return new Window(_appShell);
    }
}
