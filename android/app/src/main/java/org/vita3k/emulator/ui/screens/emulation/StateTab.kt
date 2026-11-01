package org.vita3k.emulator.ui.screens.emulation

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import java.text.DateFormat
import java.util.Date
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.vita3k.emulator.StateNative

private fun slotLabel(info: String): String {
    if (info.isEmpty()) return "kosong"
    val parts = info.split('|')
    val mib = (parts.getOrNull(0)?.toLongOrNull() ?: 0L) / 1048576.0
    val whenText = parts.getOrNull(1)?.toLongOrNull()?.let {
        DateFormat.getDateTimeInstance(DateFormat.SHORT, DateFormat.SHORT).format(Date(it * 1000))
    } ?: "?"
    return String.format("%.1f MiB, %s", mib, whenText)
}

/** Pause menu tab: save state slots (stage 2: experimental load, same session only). */
@Composable
internal fun StateTab() {
    val scope = rememberCoroutineScope()
    var infos by remember { mutableStateOf(List(StateNative.SLOT_COUNT) { "" }) }
    var busy by remember { mutableStateOf(false) }
    var report by remember { mutableStateOf("") }

    suspend fun refresh() {
        infos = withContext(Dispatchers.IO) {
            List(StateNative.SLOT_COUNT) { runCatching { StateNative.slotInfo(it + 1) }.getOrDefault("") }
        }
    }
    LaunchedEffect(Unit) { refresh() }

    Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
        Text(
            text = "Tahap 2 (eksperimental): Save menyimpan RAM dan thread ke " +
                "<storage emulasi>/savestate/<ID game>/slot-N.v3ks. Load hanya jalan di sesi game yang sama " +
                "dan hanya kalau semua thread masih di titik yang sama seperti saat Save; kalau tidak, " +
                "tidak ada yang diubah dan alasannya ditampilkan. Pakai Save/Load di titik game yang sama " +
                "(misalnya di menu yang sama). Jangan jadikan satu-satunya salinan progres.",
            style = MaterialTheme.typography.bodySmall
        )
        for (i in 1..StateNative.SLOT_COUNT) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(10.dp),
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(
                    text = "Slot $i: ${slotLabel(infos[i - 1])}",
                    style = MaterialTheme.typography.bodyMedium,
                    modifier = Modifier.weight(1f)
                )
                OutlinedButton(
                    enabled = !busy,
                    onClick = {
                        busy = true
                        report = "Menyimpan slot $i..."
                        scope.launch {
                            val result = withContext(Dispatchers.IO) {
                                runCatching { StateNative.saveSlot(i) }
                                    .getOrElse { "ERR: ${it.message}" }
                            }
                            report = result
                            refresh()
                            busy = false
                        }
                    }
                ) { Text("Save") }
                OutlinedButton(
                    enabled = !busy && infos[i - 1].isNotEmpty(),
                    onClick = {
                        busy = true
                        report = "Memuat slot $i..."
                        scope.launch {
                            val result = withContext(Dispatchers.IO) {
                                runCatching { StateNative.loadSlot(i) }
                                    .getOrElse { "ERR: ${it.message}" }
                            }
                            report = result
                            busy = false
                        }
                    }
                ) { Text("Load") }
            }
        }
        if (report.isNotEmpty()) {
            Text(text = report, style = MaterialTheme.typography.bodySmall)
        }
    }
}
