using Microsoft.Maui.Controls;
using EMOTIX360.App.ViewModels;

namespace EMOTIX360.App.Views;

public partial class DevicePage : ContentPage
{
    public DevicePage(DeviceViewModel viewModel)
    {
        InitializeComponent();
        BindingContext = viewModel;
    }
}
