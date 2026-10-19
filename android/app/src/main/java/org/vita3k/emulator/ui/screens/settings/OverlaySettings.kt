package org.vita3k.emulator.ui.screens.settings

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Restore
import androidx.compose.material.icons.filled.TouchApp
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
import androidx.compose.material3.OutlinedButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import org.vita3k.emulator.ConnectedGamepad
import org.vita3k.emulator.R
import org.vita3k.emulator.data.EmulatorConfig
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.platform.LocalContext
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.vita3k.emulator.overlay.OverlayConfig
import org.vita3k.emulator.overlay.OverlayTheme
import org.vita3k.emulator.overlay.InputOverlay
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.material3.Checkbox
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.ui.Modifier
import androidx.compose.foundation.layout.fillMaxWidth

@Composable
@Composable
private fun OverlayButtonTuningRows() {
    val buttons = listOf(
        "Cross" to InputOverlay.ButtonType.BUTTON_CROSS,
        "Circle" to InputOverlay.ButtonType.BUTTON_CIRCLE,
        "Square" to InputOverlay.ButtonType.BUTTON_SQUARE,
        "Triangle" to InputOverlay.ButtonType.BUTTON_TRIANGLE,
        "Select" to InputOverlay.ButtonType.BUTTON_SELECT,
        "Start" to InputOverlay.ButtonType.BUTTON_START,
        "PS" to InputOverlay.ButtonType.BUTTON_PS,
        "L1" to InputOverlay.ButtonType.TRIGGER_L,
        "R1" to InputOverlay.ButtonType.TRIGGER_R,
        "L2" to InputOverlay.ButtonType.TRIGGER_L2,
        "R2" to InputOverlay.ButtonType.TRIGGER_R2,
        "L3" to InputOverlay.ButtonType.TRIGGER_L3,
        "R3" to InputOverlay.ButtonType.TRIGGER_R3,
        "D-pad" to InputOverlay.ButtonType.DPAD_UP,
        "Stick kiri" to InputOverlay.ButtonType.STICK_LEFT,
        "Stick kanan" to InputOverlay.ButtonType.STICK_RIGHT,
        "Combo 1" to InputOverlay.ButtonType.COMBO_1,
        "Combo 2" to InputOverlay.ButtonType.COMBO_2,
        "Combo 3" to InputOverlay.ButtonType.COMBO_3,
        "Combo 4" to InputOverlay.ButtonType.COMBO_4,
        "Combo 5" to InputOverlay.ButtonType.COMBO_5
    )
    buttons.forEach { (label, legacyId) -> OverlayButtonTuningRow(label, legacyId) }
}

@Composable
private fun OverlayButtonTuningRow(label: String, legacyId: Int) {
    val context = LocalContext.current
    var hidden by remember(legacyId) { mutableStateOf(OverlayTheme.isHidden(context, legacyId)) }
    var scale by remember(legacyId) { mutableStateOf(OverlayTheme.getScale(context, legacyId)) }
    Column(modifier = Modifier.fillMaxWidth()) {
        Row {
            Checkbox(
                checked = hidden,
                onCheckedChange = {
                    hidden = it
                    OverlayTheme.setHidden(context, legacyId, it)
                }
            )
            Text(text = if (hidden) "$label (disembunyikan)" else label)
        }
        Slider(
            value = scale,
            onValueChange = {
                scale = it
                OverlayTheme.setScale(context, legacyId, it)
            },
            valueRange = 0.5f..1.5f,
            enabled = !hidden
        )
        Text(text = "Skala ${(scale * 100).toInt()}%")
    }
}

internal fun OverlaySettingsSection(
    cfg: EmulatorConfig,
    overlayConfig: OverlayConfig,
    isPerApp: Boolean,
    connectedGamepads: List<ConnectedGamepad>,
    onUpdate: (EmulatorConfig.() -> Unit) -> Unit,
    onOverlayConfigChange: (OverlayConfig) -> Unit,
    onShowHelp: (SettingsHelpEntry) -> Unit,
    controllerConnected: Boolean = false,
    onStartControlsEditor: (() -> Unit)? = null,
    onResetControlsLayout: (() -> Unit)? = null
) {
    Column(verticalArrangement = Arrangement.spacedBy(16.dp)) {
        SettingsSectionCard(
            title = stringResource(R.string.settings_tab_controls),
            summary = null,
            help = null,
            onShowHelp = onShowHelp
        ) {
            OverlaySettingsRows(
                overlayConfig = overlayConfig,
                onOverlayConfigChange = onOverlayConfigChange,
                onShowHelp = onShowHelp,
                controllerConnected = controllerConnected,
                onStartControlsEditor = onStartControlsEditor,
                onResetControlsLayout = onResetControlsLayout
            )
        }

        if (!isPerApp) {
            ControllerMappingSections(
                cfg = cfg,
                connectedGamepads = connectedGamepads,
                onUpdate = onUpdate,
                onShowHelp = onShowHelp
            )
        }
    }
}

@Composable
internal fun OverlaySettingsRows(
    overlayConfig: OverlayConfig,
    onOverlayConfigChange: (OverlayConfig) -> Unit,
    onShowHelp: (SettingsHelpEntry) -> Unit,
    controllerConnected: Boolean = false,
    onStartControlsEditor: (() -> Unit)? = null,
    onResetControlsLayout: (() -> Unit)? = null
) {
    Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
        if (onStartControlsEditor != null || onResetControlsLayout != null) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                if (onStartControlsEditor != null) {
                    FilledTonalButton(
                        onClick = onStartControlsEditor,
                        enabled = overlayConfig.controlsVisible,
                        modifier = Modifier.weight(1f)
                    ) {
                        Icon(Icons.Default.TouchApp, contentDescription = null)
                        Spacer(modifier = Modifier.padding(horizontal = 4.dp))
                        androidx.compose.material3.Text(stringResource(R.string.emulation_controls_edit))
                    }
                }
                if (onResetControlsLayout != null) {
                    OutlinedButton(
                        onClick = onResetControlsLayout,
                        enabled = overlayConfig.controlsVisible,
                        modifier = Modifier.weight(1f)
                    ) {
                        Icon(Icons.Default.Restore, contentDescription = null)
                        Spacer(modifier = Modifier.padding(horizontal = 4.dp))
                        androidx.compose.material3.Text(stringResource(R.string.action_reset))
                    }
                }
            }
        }

        OverlayButtonTuningRows()
        val themeContext = LocalContext.current
        val themeScope = rememberCoroutineScope()
        var themeName by remember { mutableStateOf(OverlayTheme.getActive(themeContext)) }
        // The picker needs a ComponentActivity host; inside the game (SDLActivity) these rows are hidden.
        if (themeContext is androidx.activity.ComponentActivity) {
        val themePicker = rememberLauncherForActivityResult(ActivityResultContracts.GetContent()) { uri ->
            if (uri != null) {
                themeScope.launch(Dispatchers.IO) {
                    val name = OverlayTheme.importZip(themeContext, uri)
                    withContext(Dispatchers.Main) {
                        if (name != null) {
                            themeName = name
                            Toast.makeText(themeContext, "Tema \"$name\" diterapkan", Toast.LENGTH_SHORT).show()
                        } else {
                            Toast.makeText(themeContext, "Zip tema tidak berisi PNG yang valid", Toast.LENGTH_LONG).show()
                        }
                    }
                }
            }
        }
        SettingsActionRow(
            title = "Import tema tombol (zip)",
            value = if (themeName.isEmpty()) "Bawaan" else themeName,
            onClick = { themePicker.launch("application/zip") },
            help = null,
            onShowHelp = onShowHelp
        )
        SettingsActionRow(
            title = "Pakai tema bawaan",
            value = "Hapus tema yang dipilih",
            onClick = {
                OverlayTheme.setActive(themeContext, "")
                themeName = ""
            },
            help = null,
            onShowHelp = onShowHelp
        )
        }
        SettingsToggleRow(
            title = stringResource(R.string.settings_controls_show),
            checked = overlayConfig.controlsVisible,
            onCheckedChange = { onOverlayConfigChange(overlayConfig.withControlsVisible(it)) },
            help = SettingsHelpEntry(
                title = stringResource(R.string.settings_controls_show),
                body = stringResource(R.string.settings_controls_show_desc),
                scope = SettingsScope.Global
            ),
            onShowHelp = onShowHelp
        )
        SettingsToggleRow(
            title = stringResource(R.string.settings_controls_hide_on_controller),
            checked = overlayConfig.hideOverlayWhenControllerConnected,
            onCheckedChange = {
                onOverlayConfigChange(overlayConfig.copy(hideOverlayWhenControllerConnected = it))
            },
            summary = if (controllerConnected) {
                stringResource(R.string.settings_controls_controller_connected)
            } else {
                stringResource(R.string.settings_controls_controller_disconnected)
            },
            help = SettingsHelpEntry(
                title = stringResource(R.string.settings_controls_hide_on_controller),
                body = stringResource(R.string.settings_controls_hide_on_controller_desc),
                scope = SettingsScope.Global
            ),
            onShowHelp = onShowHelp
        )
        SettingsToggleRow(
            title = stringResource(R.string.settings_controls_l2r2),
            checked = overlayConfig.l2r2Visible,
            onCheckedChange = { onOverlayConfigChange(overlayConfig.withL2R2Visible(it)) },
            enabled = overlayConfig.controlsVisible,
            help = SettingsHelpEntry(
                title = stringResource(R.string.settings_controls_l2r2),
                body = stringResource(R.string.settings_controls_l2r2_desc),
                scope = SettingsScope.Global
            ),
            onShowHelp = onShowHelp
        )
        SettingsToggleRow(
            title = stringResource(R.string.settings_controls_l3r3),
            checked = overlayConfig.l3r3Visible,
            onCheckedChange = { onOverlayConfigChange(overlayConfig.withL3R3Visible(it)) },
            enabled = overlayConfig.controlsVisible,
            help = SettingsHelpEntry(
                title = stringResource(R.string.settings_controls_l3r3),
                body = stringResource(R.string.settings_controls_l3r3_desc),
                scope = SettingsScope.Global
            ),
            onShowHelp = onShowHelp
        )
        SettingsToggleRow(
            title = stringResource(R.string.settings_controls_touch_switch),
            checked = overlayConfig.touchSwitchVisible,
            onCheckedChange = { onOverlayConfigChange(overlayConfig.withTouchSwitchVisible(it)) },
            enabled = overlayConfig.controlsVisible,
            help = SettingsHelpEntry(
                title = stringResource(R.string.settings_controls_touch_switch),
                body = stringResource(R.string.settings_controls_touch_switch_desc),
                scope = SettingsScope.Global
            ),
            onShowHelp = onShowHelp
        )
        SettingsToggleRow(
            title = stringResource(R.string.settings_controls_hide_toggle),
            checked = overlayConfig.hideToggleVisible,
            onCheckedChange = { onOverlayConfigChange(overlayConfig.withHideToggleVisible(it)) },
            enabled = overlayConfig.controlsVisible,
            help = SettingsHelpEntry(
                title = stringResource(R.string.settings_controls_hide_toggle),
                body = stringResource(R.string.settings_controls_hide_toggle_desc),
                scope = SettingsScope.Global
            ),
            onShowHelp = onShowHelp
        )
        SettingsSliderRow(
            title = stringResource(R.string.settings_controls_scale),
            valueLabel = stringResource(R.string.settings_controls_percent_value, overlayConfig.overlayScale),
            value = overlayConfig.overlayScale.toFloat(),
            onValueChange = {
                onOverlayConfigChange(overlayConfig.copy(overlayScale = it.toInt()))
            },
            valueRange = 50f..150f,
            steps = 99,
            enabled = overlayConfig.controlsVisible,
            help = SettingsHelpEntry(
                title = stringResource(R.string.settings_controls_scale),
                body = stringResource(R.string.settings_controls_scale_desc),
                scope = SettingsScope.Global
            ),
            onShowHelp = onShowHelp
        )
        SettingsSliderRow(
            title = stringResource(R.string.settings_controls_opacity),
            valueLabel = stringResource(R.string.settings_controls_percent_value, overlayConfig.overlayOpacity),
            value = overlayConfig.overlayOpacity.toFloat(),
            onValueChange = {
                onOverlayConfigChange(overlayConfig.copy(overlayOpacity = it.toInt()))
            },
            valueRange = 20f..100f,
            steps = 79,
            enabled = overlayConfig.controlsVisible,
            help = SettingsHelpEntry(
                title = stringResource(R.string.settings_controls_opacity),
                body = stringResource(R.string.settings_controls_opacity_desc),
                scope = SettingsScope.Global
            ),
            onShowHelp = onShowHelp
        )
    }
}
