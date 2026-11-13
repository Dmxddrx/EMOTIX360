# HomePage.xaml binding patch

Keep your existing 506-line HomePage design. Do not replace the page with a new layout.
Apply these small edits to `Views/HomePage.xaml` so the existing controls bind to `HomeViewModel`.

## A. Header connection indicator

In the top-right connection badge:

1. Change the status `Ellipse` Fill from `#F2A65A` to:

```xml
Fill="{Binding ConnectionIndicatorColor}"
```

2. Change the status label text from `OFFLINE` to:

```xml
Text="{Binding ConnectionText}"
```

## B. Connection card

Change the `Not connected` label to:

```xml
Text="{Binding ConnectionDescription}"
```

Change the `CONNECT DEVICE` button to:

```xml
<Button
    Text="{Binding ConnectButtonText}"
    Command="{Binding OpenDeviceCommand}"
    BackgroundColor="#397FE8"
    TextColor="White"
    FontAttributes="Bold"
    CharacterSpacing="1"
    FontSize="13"
    HeightRequest="52"
    CornerRadius="14" />
```

Remove the old `Clicked="OnConnectDeviceClicked"` attribute.

## C. Device overview values

Within the DEVICE OVERVIEW section, bind the existing labels as follows:

- `AWAITING CONNECTION` → `Text="{Binding OverviewStatusText}"`
- The `--` label under ACTIVE MODE → `Text="{Binding ActiveModeText}"`
- The `--` label under BRIGHTNESS → `Text="{Binding BrightnessText}"`
- The `--` label under WI-FI → `Text="{Binding WifiText}"`
- The `--` label under MOTION SENSOR → `Text="{Binding MotionText}"`
- The bottom hint `Connect your device to control its animations.` → `Text="{Binding DashboardMessage}"`

Be careful to change the `--` labels in their corresponding cards, not all occurrences globally.

## D. Make each existing animation tile selectable

Keep each existing `Border` and its styling. Add a `Border.GestureRecognizers` section inside each animation tile's `Border`, before its `VerticalStackLayout` or `Label` child.

Use these gesture recognizers for the seven tiles:

```xml
<Border.GestureRecognizers>
    <TapGestureRecognizer
        Command="{Binding SelectAnimationCommand}"
        CommandParameter="IDLE" />
</Border.GestureRecognizers>
```

Use the matching `CommandParameter` for each tile:

| Tile | CommandParameter |
|---|---|
| IDLE | `IDLE` |
| SAND CLOCK | `SAND_CLOCK` |
| WATER | `WATER` |
| SAND GRAVITY | `SAND_GRAVITY` |
| HEART | `HEART` |
| RAIN | `RAIN` |
| WAVES | `WAVES` |

## E. Add a brightness slider and apply button

Inside the DEVICE OVERVIEW `VerticalStackLayout`, immediately after the two-row overview `Grid`, insert:

```xml
<VerticalStackLayout Spacing="8" Margin="0,4,0,0">
    <Grid ColumnDefinitions="*,Auto">
        <Label
            Text="MATRIX BRIGHTNESS"
            FontSize="10"
            FontAttributes="Bold"
            CharacterSpacing="1"
            TextColor="#8495AE" />
        <Label
            Grid.Column="1"
            Text="{Binding BrightnessSliderText}"
            FontSize="11"
            FontAttributes="Bold"
            TextColor="#E5BD70" />
    </Grid>

    <Slider
        Minimum="0"
        Maximum="15"
        Value="{Binding BrightnessValue, Mode=TwoWay}"
        MinimumTrackColor="#5796FF"
        MaximumTrackColor="#26364A"
        ThumbColor="#F2F5FC" />

    <Button
        Text="APPLY BRIGHTNESS"
        Command="{Binding ApplyBrightnessCommand}"
        BackgroundColor="#263C58"
        TextColor="#DDE9FF"
        FontAttributes="Bold"
        FontSize="11"
        HeightRequest="44"
        CornerRadius="12" />
</VerticalStackLayout>
```

The slider only changes its local value. The Apply button writes one GATT command, avoiding a burst of BLE writes while the slider is being dragged.

## F. HomePage.xaml.cs

Replace the code-behind with the `Views/HomePage.xaml.cs` supplied in this integration bundle. It receives `HomeViewModel` through dependency injection.
