
namespace EMOTIX360.App.Views;

public partial class HomePage : ContentPage
{
    public HomePage()
    {
        InitializeComponent();
    }

    private async void OnConnectDeviceClicked(
        object sender,
        EventArgs e)
    {
        await Shell.Current.GoToAsync("//device");
    }
}
