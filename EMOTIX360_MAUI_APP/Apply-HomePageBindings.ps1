[CmdletBinding()]
param(
    [string]$HomePagePath = (Join-Path (Get-Location).Path 'EMOTIX360.App\Views\HomePage.xaml')
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $HomePagePath)) {
    throw "HomePage.xaml was not found: $HomePagePath`nRun this script from the EMOTIX360_MAUI_APP solution directory or pass -HomePagePath with the full path."
}

function Replace-ExactlyOnce {
    param(
        [string]$Text,
        [string]$Old,
        [string]$New,
        [string]$Description
    )

    $count = ([regex]::Matches($Text, [regex]::Escape($Old))).Count
    if ($count -ne 1) {
        throw "Expected exactly one occurrence for '$Description', found $count. No file was written. Check the current HomePage.xaml and apply HomePage-XAML-Bindings.md manually if the page has changed."
    }

    return $Text.Replace($Old, $New)
}

$content = Get-Content -LiteralPath $HomePagePath -Raw

# Header connection indicator.
$content = Replace-ExactlyOnce $content 'Fill="#F2A65A"' 'Fill="{Binding ConnectionIndicatorColor}"' 'connection indicator fill'
$content = Replace-ExactlyOnce $content 'Text="OFFLINE"' 'Text="{Binding ConnectionText}"' 'connection indicator text'

# Main connection card.
$content = Replace-ExactlyOnce $content 'Text="Not connected"' 'Text="{Binding ConnectionDescription}"' 'connection description'
$content = Replace-ExactlyOnce $content 'Text="CONNECT DEVICE"' 'Text="{Binding ConnectButtonText}"' 'connect button text'
$content = Replace-ExactlyOnce $content '                        Clicked="OnConnectDeviceClicked"' '' 'old connect click handler'

# Device overview header and dashboard hint.
$content = Replace-ExactlyOnce $content 'Text="AWAITING CONNECTION"' 'Text="{Binding OverviewStatusText}"' 'overview status'
$content = Replace-ExactlyOnce $content 'Text="Connect your device to control its animations."' 'Text="{Binding DashboardMessage}"' 'dashboard message'

# Replace the correct '--' placeholder inside each identified card only.
$cardBindings = @(
    @{ Start = '<!-- ACTIVE MODE -->'; End = '<!-- BRIGHTNESS -->'; Binding = 'ActiveModeText'; Label = 'active mode' },
    @{ Start = '<!-- BRIGHTNESS -->'; End = '<!-- WIFI STATUS -->'; Binding = 'BrightnessText'; Label = 'brightness' },
    @{ Start = '<!-- WIFI STATUS -->'; End = '<!-- MOTION SENSOR -->'; Binding = 'WifiText'; Label = 'Wi-Fi' },
    @{ Start = '<!-- MOTION SENSOR -->'; End = '</Grid>'; Binding = 'MotionText'; Label = 'motion sensor' }
)

foreach ($card in $cardBindings) {
    $start = $content.IndexOf($card.Start, [StringComparison]::Ordinal)
    if ($start -lt 0) { throw "Could not find $($card.Label) section. No file was written." }

    if ($card.End -eq '</Grid>') {
        $end = $content.IndexOf($card.End, $start, [StringComparison]::Ordinal)
    }
    else {
        $end = $content.IndexOf($card.End, $start + $card.Start.Length, [StringComparison]::Ordinal)
    }
    if ($end -lt 0) { throw "Could not find the end of $($card.Label) section. No file was written." }

    $section = $content.Substring($start, $end - $start)
    $placeholderCount = ([regex]::Matches($section, [regex]::Escape('Text="--"'))).Count
    if ($placeholderCount -ne 1) {
        throw "Expected one '--' placeholder in $($card.Label) section, found $placeholderCount. No file was written."
    }

    $replacement = 'Text="{Binding ' + $card.Binding + '}"'
    $section = $section.Replace('Text="--"', $replacement)
    $content = $content.Substring(0, $start) + $section + $content.Substring($end)
}

# Attach tap commands to the seven existing animation Border tiles.
$animationModes = @('IDLE', 'SAND CLOCK', 'WATER', 'SAND GRAVITY', 'HEART', 'RAIN', 'WAVES')
foreach ($displayName in $animationModes) {
    $needle = 'Text="' + $displayName + '"'
    $labelPosition = $content.IndexOf($needle, [StringComparison]::Ordinal)
    if ($labelPosition -lt 0) { throw "Could not find animation tile '$displayName'. No file was written." }

    # Find the nearest actual Border start tag (not Border.StrokeShape).
    $prefix = $content.Substring(0, $labelPosition)
    $borderMatches = [regex]::Matches($prefix, '<Border(?=[\s>])')
    if ($borderMatches.Count -eq 0) { throw "Could not locate Border for '$displayName'. No file was written." }

    $borderStart = $borderMatches[$borderMatches.Count - 1].Index
    $borderEnd = $content.IndexOf('</Border>', $labelPosition, [StringComparison]::Ordinal)
    if ($borderEnd -lt 0) { throw "Could not locate end of Border for '$displayName'. No file was written." }

    $borderSection = $content.Substring($borderStart, $borderEnd - $borderStart)
    if ($borderSection.Contains('Border.GestureRecognizers')) {
        throw "A gesture recognizer already exists in the '$displayName' tile. No file was written to avoid duplicating it."
    }

    $labelStart = $content.LastIndexOf('<Label', $labelPosition, [StringComparison]::Ordinal)
    if ($labelStart -lt $borderStart) { throw "Could not locate Label for '$displayName'. No file was written." }

    $wireName = $displayName.Replace(' ', '_')
    $indent = '                        '
    $gesture = @"
$indent<Border.GestureRecognizers>
$indent    <TapGestureRecognizer
$indent        Command="{Binding SelectAnimationCommand}"
$indent        CommandParameter="$wireName" />
$indent</Border.GestureRecognizers>

"@
    $content = $content.Insert($labelStart, $gesture)
}

# Insert brightness controls before the end of DEVICE OVERVIEW.
$animationMarker = '<!-- ANIMATION MODES -->'
$animationPosition = $content.IndexOf($animationMarker, [StringComparison]::Ordinal)
if ($animationPosition -lt 0) { throw 'Could not locate ANIMATION MODES section. No file was written.' }

$overviewClosing = $content.LastIndexOf('</VerticalStackLayout>', $animationPosition, [StringComparison]::Ordinal)
if ($overviewClosing -lt 0) { throw 'Could not locate end of DEVICE OVERVIEW. No file was written.' }

$overviewSectionStart = $content.LastIndexOf('<!-- DEVICE OVERVIEW -->', $overviewClosing, [StringComparison]::Ordinal)
if ($overviewSectionStart -lt 0) { throw 'Could not locate DEVICE OVERVIEW section. No file was written.' }
if ($content.Substring($overviewSectionStart, $overviewClosing - $overviewSectionStart).Contains('BrightnessSliderText')) {
    throw 'Brightness controls already appear in DEVICE OVERVIEW. No file was written to avoid duplicates.'
}

$brightnessBlock = @'
                <VerticalStackLayout Spacing="8" Margin="0,4,0,0">
                    <Grid ColumnDefinitions="*,Auto">
                        <Label Text="MATRIX BRIGHTNESS"
                               FontSize="10" FontAttributes="Bold"
                               CharacterSpacing="1" TextColor="#8495AE" />
                        <Label Grid.Column="1"
                               Text="{Binding BrightnessSliderText}"
                               FontSize="11" FontAttributes="Bold"
                               TextColor="#E5BD70" />
                    </Grid>
                    <Slider Minimum="0" Maximum="15"
                            Value="{Binding BrightnessValue, Mode=TwoWay}"
                            MinimumTrackColor="#5796FF"
                            MaximumTrackColor="#26364A"
                            ThumbColor="#F2F5FC" />
                    <Button Text="APPLY BRIGHTNESS"
                            Command="{Binding ApplyBrightnessCommand}"
                            BackgroundColor="#263C58"
                            TextColor="#DDE9FF"
                            FontAttributes="Bold" FontSize="11"
                            HeightRequest="44" CornerRadius="12" />
                </VerticalStackLayout>

'@
$content = $content.Insert($overviewClosing, $brightnessBlock)

# Write only after all validation and replacements succeeded.
$backupPath = "$HomePagePath.before-ble.bak"
if (-not (Test-Path -LiteralPath $backupPath)) {
    Copy-Item -LiteralPath $HomePagePath -Destination $backupPath
}

[System.IO.File]::WriteAllText(
    $HomePagePath,
    $content,
    [System.Text.UTF8Encoding]::new($false)
)

Write-Host 'HomePage.xaml BLE bindings applied successfully.' -ForegroundColor Green
Write-Host "Backup: $backupPath" -ForegroundColor Cyan
Write-Host 'Next: replace Views\HomePage.xaml.cs with the DI-enabled code-behind from this bundle.' -ForegroundColor Yellow
