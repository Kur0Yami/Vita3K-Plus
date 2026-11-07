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
import androidx.compose.ui.graphics.Color
import org.vita3k.emulator.overlay.InputOverlay
import androidx.compose.material3.Checkbox
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.ui.Alignment
import androidx.compose.ui.draw.clip
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.sp
import kotlin.math.roundToInt

private const val TUNE_ALL = -1

/**
 * One panel: a 4-column grid of every virtual button (checkbox = shown) and one pair of
 * scale/opacity sliders underneath. "Semua tombol" (top) makes the sliders edit the global
 * values; touching a button name makes the same sliders edit that button instead.
 * Per-button values are multipliers on top of the global ones.
 */
@Composable
private fun OverlayButtonTuningPanel(
    overlayConfig: OverlayConfig,
    onOverlayConfigChange: (OverlayConfig) -> Unit,
    onShowHelp: (SettingsHelpEntry) -> Unit
) {
    val context = LocalContext.current
    val items = remember {
        listOf(
            "Cross" to InputOverlay.ButtonType.BUTTON_CROSS,
            "Circle" to InputOverlay.ButtonType.BUTTON_CIRCLE,
            "Square" to InputOverlay.ButtonType.BUTTON_SQUARE,
            "Triangle" to InputOverlay.ButtonType.BUTTON_TRIANGLE,
            "L1" to InputOverlay.ButtonType.TRIGGER_L,
            "R1" to InputOverlay.ButtonType.TRIGGER_R,
            "L2" to InputOverlay.ButtonType.TRIGGER_L2,
            "R2" to InputOverlay.ButtonType.TRIGGER_R2,
            "L3" to InputOverlay.ButtonType.TRIGGER_L3,
            "R3" to InputOverlay.ButtonType.TRIGGER_R3,
            "Select" to InputOverlay.ButtonType.BUTTON_SELECT,
            "Start" to InputOverlay.ButtonType.BUTTON_START,
            "PS" to InputOverlay.ButtonType.BUTTON_PS,
            "Touch" to InputOverlay.ButtonType.BUTTON_TOUCH_SWITCH,
            "Hide" to InputOverlay.ButtonType.BUTTON_TOUCH_HIDE,
            "D-pad" to InputOverlay.ButtonType.DPAD_UP,
            "L-Stick" to InputOverlay.ButtonType.STICK_LEFT,
            "R-Stick" to InputOverlay.ButtonType.STICK_RIGHT,
            "Cheat" to InputOverlay.ButtonType.BUTTON_CHEAT,
            "Macro" to InputOverlay.ButtonType.BUTTON_MACRO,
            "Combo 1" to InputOverlay.ButtonType.COMBO_1,
            "Combo 2" to InputOverlay.ButtonType.COMBO_2,
            "Combo 3" to InputOverlay.ButtonType.COMBO_3,
            "Combo 4" to InputOverlay.ButtonType.COMBO_4,
            "Combo 5" to InputOverlay.ButtonType.COMBO_5
        )
    }
    val hidden = remember {
        mutableStateMapOf<Int, Boolean>().also { m ->
            items.forEach { (_, id) -> m[id] = OverlayTheme.isHidden(context, id) }
        }
    }
    val scales = remember {
        mutableStateMapOf<Int, Float>().also { m ->
            items.forEach { (_, id) -> m[id] = OverlayTheme.getScale(context, id) }
        }
    }
    val opacities = remember {
        mutableStateMapOf<Int, Float>().also { m ->
            items.forEach { (_, id) -> m[id] = OverlayTheme.getOpacity(context, id) }
        }
    }
    var selected by remember { mutableStateOf(TUNE_ALL) }
    val allSelected = selected == TUNE_ALL
    val selectedLabel = items.firstOrNull { it.second == selected }?.first ?: ""

    // L2/R2, L3/R3, Touch and Hide used to have their own on/off rows (overlay mask bits).
    // They are now plain grid cells: shown = mask bit on AND the button's own checkbox on.
    fun maskBitFor(id: Int): Int = when {
        id == InputOverlay.ButtonType.TRIGGER_L2 || id == InputOverlay.ButtonType.TRIGGER_R2 -> InputOverlay.OVERLAY_MASK_L2R2
        id == InputOverlay.ButtonType.TRIGGER_L3 || id == InputOverlay.ButtonType.TRIGGER_R3 -> InputOverlay.OVERLAY_MASK_L3R3
        id == InputOverlay.ButtonType.BUTTON_TOUCH_SWITCH -> InputOverlay.OVERLAY_MASK_TOUCH_SCREEN_SWITCH
        id == InputOverlay.ButtonType.BUTTON_TOUCH_HIDE -> InputOverlay.OVERLAY_MASK_HIDE_TOGGLE
        else -> 0
    }

    fun isShown(id: Int): Boolean {
        val bit = maskBitFor(id)
        val maskOn = bit == 0 || (overlayConfig.overlayMask and bit) != 0
        return maskOn && hidden[id] != true
    }

    fun withMaskBit(bit: Int, on: Boolean): OverlayConfig {
        val base = overlayConfig.activeMask()
        return overlayConfig.copy(overlayMask = if (on) base or bit else base and bit.inv()).normalized()
    }

    fun setShown(id: Int, shown: Boolean) {
        val bit = maskBitFor(id)
        if (bit == 0) {
            hidden[id] = !shown
            OverlayTheme.setHidden(context, id, !shown)
            OverlayTheme.notifyChanged()
            return
        }
        val siblings = items.map { it.second }.filter { it != id && maskBitFor(it) == bit }
        val maskOn = (overlayConfig.overlayMask and bit) != 0
        if (shown) {
            if (!maskOn) {
                // The group was off: keep the other button of the pair hidden so only this one appears.
                siblings.forEach {
                    hidden[it] = true
                    OverlayTheme.setHidden(context, it, true)
                }
            }
            hidden[id] = false
            OverlayTheme.setHidden(context, id, false)
            if (!maskOn) onOverlayConfigChange(withMaskBit(bit, true))
        } else if (siblings.isEmpty()) {
            // Touch / Hide have no pair: just clear their mask bit.
            onOverlayConfigChange(withMaskBit(bit, false))
        } else {
            hidden[id] = true
            OverlayTheme.setHidden(context, id, true)
        }
        OverlayTheme.notifyChanged()
    }

    val sliderEnabled = if (allSelected) overlayConfig.controlsVisible else isShown(selected)

    Column(modifier = Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        val allShape = RoundedCornerShape(8.dp)
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .clip(allShape)
                .border(
                    if (allSelected) 2.dp else 1.dp,
                    if (allSelected) MaterialTheme.colorScheme.primary else Color.Gray,
                    allShape
                )
                .clickable { selected = TUNE_ALL }
                .padding(vertical = 12.dp),
            contentAlignment = Alignment.Center
        ) {
            Text(text = "Semua tombol", color = Color.White)
        }

        items.chunked(4).forEach { rowItems ->
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(6.dp)
            ) {
                rowItems.forEach { (label, id) ->
                    OverlayTuneCell(
                        label = label,
                        hidden = !isShown(id),
                        selected = selected == id,
                        enabled = maskBitFor(id) == 0 || overlayConfig.controlsVisible,
                        onSelect = { selected = id },
                        onShownChange = { shown -> setShown(id, shown) },
                        modifier = Modifier.weight(1f)
                    )
                }
                repeat(4 - rowItems.size) { Spacer(modifier = Modifier.weight(1f)) }
            }
        }

        Text(
            text = "Centang = tampil (termasuk L2/R2, L3/R3, Touch, Hide). Sentuh nama tombol untuk mengatur skala dan opacity-nya.",
            color = Color.Gray,
            fontSize = 11.sp
        )

        val scalePct = if (allSelected) {
            overlayConfig.overlayScale.toFloat()
        } else {
            ((scales[selected] ?: 1f) * 100f).roundToInt().toFloat()
        }
        val opacityPct = if (allSelected) {
            overlayConfig.overlayOpacity.toFloat()
        } else {
            ((opacities[selected] ?: 1f) * 100f).roundToInt().toFloat()
        }
        SettingsSliderRow(
            title = if (allSelected) stringResource(R.string.settings_controls_scale) else "Skala: $selectedLabel",
            valueLabel = stringResource(R.string.settings_controls_percent_value, scalePct.toInt()),
            value = scalePct,
            onValueChange = { v ->
                if (allSelected) {
                    onOverlayConfigChange(overlayConfig.copy(overlayScale = v.toInt()))
                } else {
                    val s = v.toInt() / 100f
                    scales[selected] = s
                    OverlayTheme.setScale(context, selected, s)
                    OverlayTheme.notifyChanged()
                }
            },
            valueRange = 50f..150f,
            steps = 99,
            enabled = sliderEnabled,
            help = if (allSelected) {
                SettingsHelpEntry(
                    title = stringResource(R.string.settings_controls_scale),
                    body = stringResource(R.string.settings_controls_scale_desc),
                    scope = SettingsScope.Global
                )
            } else {
                null
            },
            onShowHelp = onShowHelp
        )
        SettingsSliderRow(
            title = if (allSelected) stringResource(R.string.settings_controls_opacity) else "Opacity: $selectedLabel",
            valueLabel = stringResource(R.string.settings_controls_percent_value, opacityPct.toInt()),
            value = opacityPct,
            onValueChange = { v ->
                if (allSelected) {
                    onOverlayConfigChange(overlayConfig.copy(overlayOpacity = v.toInt()))
                } else {
                    val o = v.toInt() / 100f
                    opacities[selected] = o
                    OverlayTheme.setOpacity(context, selected, o)
                    OverlayTheme.notifyChanged()
                }
            },
            valueRange = 20f..100f,
            steps = 79,
            enabled = sliderEnabled,
            help = if (allSelected) {
                SettingsHelpEntry(
                    title = stringResource(R.string.settings_controls_opacity),
                    body = stringResource(R.string.settings_controls_opacity_desc),
                    scope = SettingsScope.Global
                )
            } else {
                null
            },
            onShowHelp = onShowHelp
        )
    }
}

@Composable
private fun OverlayTuneCell(
    label: String,
    hidden: Boolean,
    selected: Boolean,
    onSelect: () -> Unit,
    onShownChange: (Boolean) -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true
) {
    val shape = RoundedCornerShape(8.dp)
    val borderColor = if (selected) MaterialTheme.colorScheme.primary else Color.Gray
    Column(
        modifier = modifier
            .clip(shape)
            .border(if (selected) 2.dp else 1.dp, borderColor, shape),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        Text(
            text = label,
            color = if (hidden) Color.Gray else Color.White,
            fontSize = 12.sp,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            textAlign = TextAlign.Center,
            modifier = Modifier
                .fillMaxWidth()
                .clickable(onClick = onSelect)
                .padding(start = 2.dp, end = 2.dp, top = 10.dp, bottom = 4.dp)
        )
        Checkbox(checked = !hidden, onCheckedChange = onShownChange, enabled = enabled)
    }
}

@Composable
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

        ComboKeySection(
            cfg = cfg,
            connectedGamepads = connectedGamepads,
            onUpdate = onUpdate,
            onShowHelp = onShowHelp,
            summary = if (isPerApp) {
                "Tahan modifier lalu tekan trigger untuk menekan beberapa tombol Vita sekaligus (maks 5). Disimpan khusus untuk game ini (config per ID game)."
            } else {
                "Tahan modifier lalu tekan trigger untuk menekan beberapa tombol Vita sekaligus (maks 5). Ini combo global; game yang punya config khusus memakai combo-nya sendiri."
            }
        )

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

        OverlayButtonTuningPanel(overlayConfig, onOverlayConfigChange, onShowHelp)
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
            title = stringResource(R.string.settings_controls_touch_switch_tilt),
            checked = overlayConfig.touchSwitchTilt,
            onCheckedChange = { onOverlayConfigChange(overlayConfig.copy(touchSwitchTilt = it)) },
            enabled = overlayConfig.controlsVisible && overlayConfig.touchSwitchVisible,
            help = SettingsHelpEntry(
                title = stringResource(R.string.settings_controls_touch_switch_tilt),
                body = stringResource(R.string.settings_controls_touch_switch_tilt_desc),
                scope = SettingsScope.Global
            ),
            onShowHelp = onShowHelp
        )
    }
}
