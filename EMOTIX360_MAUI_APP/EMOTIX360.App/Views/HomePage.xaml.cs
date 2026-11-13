using Microsoft.Maui.Controls;
using EMOTIX360.App.ViewModels;

namespace EMOTIX360.App.Views;

public partial class HomePage : ContentPage
{
    public HomePage(HomeViewModel viewModel)
    {
        InitializeComponent();
        BindingContext = viewModel;
    }
}
