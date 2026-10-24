package org.vita3k.emulator.ui.screens.emulation

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.vita3k.emulator.CheatNative
import org.vita3k.emulator.ui.theme.ApplyDialogDim

private const val MAX_SHOWN = 40
private const val RESULT_CAP = 4_000_000L

private data class CheatRow(val address: Long, val raw: Int)

private data class CheatItem(
    val id: Long,
    val psv: Boolean,
    val enabled: Boolean,
    val supported: Boolean,
    val address: Long,
    val type: Int,
    val raw: Int,
    val info: String,
    val name: String
)

private class EntryAction(val label: String, val run: (Int, String) -> Unit)

private fun typeLabel(type: Int): String = when (type) {
    CheatNative.TYPE_FLOAT -> "Float"
    CheatNative.TYPE_BYTE -> "Byte"
    CheatNative.TYPE_SHORT -> "Short"
    else -> "Int"
}

private fun fmtValue(raw: Int, type: Int): String =
    if (type == CheatNative.TYPE_FLOAT) Float.fromBits(raw).toString() else raw.toString()

private fun parseValue(text: String, type: Int): Int? = when (type) {
    CheatNative.TYPE_FLOAT -> text.trim().replace(',', '.').toFloatOrNull()?.toRawBits()
    CheatNative.TYPE_BYTE -> text.trim().toIntOrNull()?.and(0xFF)
    CheatNative.TYPE_SHORT -> text.trim().toIntOrNull()?.and(0xFFFF)
    else -> text.trim().toIntOrNull()
}

private fun capNote(n: Long): String =
    if (n >= RESULT_CAP) " (batas tercapai, hasil terpotong; persempit lagi)" else ""

private fun floatTolerance(text: String): Float {
    val t = text.trim().replace(',', '.')
    val dot = t.indexOf('.')
    val decimals = if (dot < 0) 0 else t.length - dot - 1
    return if (decimals <= 0) 0.001f else (0.5 * Math.pow(10.0, -decimals.toDouble())).toFloat()
}

private fun errorText(code: Long): String = when (code) {
    -1L -> "Memori game terlalu besar untuk snapshot."
    -2L -> "Belum ada snapshot."
    else -> "Game belum berjalan."
}

private fun addr(a: Long): String = "0x%08X".format(a)

private fun parseItems(text: String): List<CheatItem> = text.lines().mapNotNull { line ->
    val p = line.split('\t', limit = 9)
    if (p.size < 9) {
        null
    } else {
        runCatching {
            CheatItem(p[0].toLong(), p[1] == "1", p[2] == "1", p[3] == "1", p[4].toLong(), p[5].toInt(), p[6].toInt(), p[7], p[8])
        }.getOrNull()
    }
}

@Composable
private fun EntryDialog(
    title: String,
    type: Int,
    initialRaw: Int,
    initialName: String,
    actions: List<EntryAction>,
    onDelete: (() -> Unit)?,
    onDismiss: () -> Unit,
    onInvalid: () -> Unit
) {
    var text by remember { mutableStateOf(fmtValue(initialRaw, type)) }
    var name by remember { mutableStateOf(initialName) }
    val tight = PaddingValues(horizontal = 4.dp, vertical = 6.dp)
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = {
            ApplyDialogDim()
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(
                    value = text,
                    onValueChange = { text = it },
                    singleLine = true,
                    label = { Text("Nilai (${typeLabel(type)})") },
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal)
                )
                OutlinedTextField(
                    value = name,
                    onValueChange = { name = it },
                    singleLine = true,
                    label = { Text("Nama (opsional)") }
                )
                Row(horizontalArrangement = Arrangement.spacedBy(6.dp), modifier = Modifier.fillMaxWidth()) {
                    actions.forEach { action ->
                        OutlinedButton(
                            contentPadding = tight,
                            modifier = Modifier.weight(1f),
                            onClick = {
                                val raw = parseValue(text, type)
                                if (raw == null) {
                                    onInvalid()
                                } else {
                                    action.run(raw, name.trim())
                                    onDismiss()
                                }
                            }
                        ) { Text(action.label, maxLines = 1) }
                    }
                    if (onDelete != null) {
                        OutlinedButton(
                            contentPadding = tight,
                            modifier = Modifier.weight(1f),
                            onClick = {
                                onDelete()
                                onDismiss()
                            }
                        ) { Text("Hapus", maxLines = 1) }
                    }
                }
            }
        },
        confirmButton = {},
        dismissButton = { TextButton(onClick = onDismiss) { Text("Batal") } }
    )
}

@Composable
internal fun CheatSearchTab() {
    val scope = rememberCoroutineScope()
    var valueText by remember { mutableStateOf("") }
    var type by remember { mutableStateOf(CheatNative.TYPE_INT) }
    var busy by remember { mutableStateOf(false) }
    var status by remember { mutableStateOf("Isi nilai lalu tekan Cari.") }
    var rows by remember { mutableStateOf<List<CheatRow>>(emptyList()) }
    var rowsType by remember { mutableStateOf(CheatNative.TYPE_INT) }
    var total by remember { mutableStateOf(0L) }
    var items by remember { mutableStateOf<List<CheatItem>>(emptyList()) }
    var searchEdit by remember { mutableStateOf<CheatRow?>(null) }
    var listEdit by remember { mutableStateOf<CheatItem?>(null) }
    var hasSnapshot by remember { mutableStateOf(false) }
    var dir by remember { mutableStateOf("") }
    var offsetText by remember { mutableStateOf("") }

    fun refreshItems() {
        items = parseItems(CheatNative.cheatList())
    }

    fun refreshRows(t: Int) {
        val a = CheatNative.results(0, MAX_SHOWN)
        rows = List(a.size / 2) { CheatRow(a[it * 2], a[it * 2 + 1].toInt()) }
        rowsType = t
        total = CheatNative.count()
    }

    fun launchScan(label: String, block: () -> Long, done: (Long) -> String) {
        if (busy) return
        busy = true
        status = "$label..."
        val t = type
        scope.launch {
            val n = withContext(Dispatchers.Default) { block() }
            busy = false
            status = if (n < 0) errorText(n) else done(n)
            refreshRows(t)
        }
    }

    LaunchedEffect(Unit) {
        while (true) {
            refreshItems()
            kotlinx.coroutines.delay(1000)
        }
    }

    LaunchedEffect(Unit) {
        refreshItems()
        refreshRows(type)
        dir = CheatNative.cheatDir()
        offsetText = "%X".format(CheatNative.cheatPsvOffset())
        hasSnapshot = false
    }

    Column(verticalArrangement = Arrangement.spacedBy(10.dp), modifier = Modifier.fillMaxWidth()) {
        Text(
            text = "Cheat Search",
            style = MaterialTheme.typography.titleMedium,
            color = MaterialTheme.colorScheme.onSurface
        )
        Text(
            text = "Cari nilai di memori game. Menu terbuka = game dijeda. Cari nilai, tutup menu, lanjutkan game sampai nilainya berubah, buka lagi lalu Persempit dengan nilai baru. Jumlah item biasanya 1 atau 2 byte (coba Byte atau Short), HP/uang biasanya Int atau Float.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )

        OutlinedTextField(
            value = valueText,
            onValueChange = { valueText = it },
            singleLine = true,
            label = { Text("Nilai") },
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
            modifier = Modifier.fillMaxWidth()
        )
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            listOf(
                "Byte" to CheatNative.TYPE_BYTE,
                "Short" to CheatNative.TYPE_SHORT,
                "Int" to CheatNative.TYPE_INT,
                "Float" to CheatNative.TYPE_FLOAT
            ).forEach { (label, t) ->
                FilterChip(selected = type == t, onClick = { type = t }, label = { Text(label) })
            }
        }

        val tight = PaddingValues(horizontal = 4.dp, vertical = 6.dp)
        var randomMsg by remember { mutableStateOf("") }
        OutlinedButton(
            enabled = !busy,
            contentPadding = tight,
            modifier = Modifier.fillMaxWidth(),
            onClick = {
                CheatNative.resetRandom()
                randomMsg = "Angka acak diatur ulang. Matikan lalu nyalakan cheat acak."
            }
        ) { Text("Acak ulang cheat", maxLines = 1) }
        if (randomMsg.isNotEmpty()) Text(randomMsg, style = MaterialTheme.typography.bodySmall)
        var nameText by remember { mutableStateOf("") }
        var nameUtf16 by remember { mutableStateOf(false) }
        OutlinedTextField(
            value = nameText,
            onValueChange = { nameText = it },
            enabled = !busy,
            singleLine = true,
            label = { Text("Nama (teks)") },
            modifier = Modifier.fillMaxWidth()
        )
        Row(horizontalArrangement = Arrangement.spacedBy(6.dp), modifier = Modifier.fillMaxWidth()) {
            FilterChip(
                selected = nameUtf16,
                onClick = { nameUtf16 = !nameUtf16 },
                label = { Text("UTF-16") }
            )
            OutlinedButton(
                enabled = !busy && nameText.isNotBlank(),
                contentPadding = tight,
                modifier = Modifier.weight(1f),
                onClick = {
                    val p = CheatNative.textPattern(nameText, nameUtf16)
                    hasSnapshot = false
                    launchScan("Mencari nama", { CheatNative.searchBytes(p) }) { "Ditemukan $it alamat.${capNote(it)}" }
                }
            ) { Text("Cari nama", maxLines = 1) }
        }
        Row(horizontalArrangement = Arrangement.spacedBy(6.dp), modifier = Modifier.fillMaxWidth()) {
            OutlinedButton(
                enabled = !busy,
                contentPadding = tight,
                modifier = Modifier.weight(1f),
                onClick = {
                    val raw = parseValue(valueText, type)
                    if (raw == null) {
                        status = "Nilai tidak valid."
                    } else {
                        hasSnapshot = false
                        val tol = if (type == CheatNative.TYPE_FLOAT) floatTolerance(valueText) else 0f
                        launchScan("Mencari", { CheatNative.search(raw, type, tol) }) { "Ditemukan $it alamat.${capNote(it)}" }
                    }
                }
            ) { Text("Cari", maxLines = 1) }
            OutlinedButton(
                enabled = !busy,
                contentPadding = tight,
                modifier = Modifier.weight(1f),
                onClick = {
                    val raw = parseValue(valueText, type)
                    if (raw == null) {
                        status = "Nilai tidak valid."
                    } else {
                        val tol = if (type == CheatNative.TYPE_FLOAT) floatTolerance(valueText) else 0f
                        launchScan("Mempersempit", { CheatNative.refine(-1, raw, type, tol) }) { "Tersisa $it alamat." }
                    }
                }
            ) { Text("Persempit", maxLines = 1) }
            OutlinedButton(
                enabled = !busy,
                contentPadding = tight,
                modifier = Modifier.weight(1f),
                onClick = {
                    hasSnapshot = true
                    launchScan("Snapshot", { CheatNative.snapshot() }) {
                        "Snapshot ${it / (1024 * 1024)} MB. Lanjutkan game, ubah nilainya, lalu pilih Naik/Turun/Sama/Berubah."
                    }
                }
            ) { Text("Snapshot", maxLines = 1) }
        }
        Row(horizontalArrangement = Arrangement.spacedBy(6.dp), modifier = Modifier.fillMaxWidth()) {
            listOf(
                "Naik" to CheatNative.CMP_INCREASED,
                "Turun" to CheatNative.CMP_DECREASED,
                "Sama" to CheatNative.CMP_UNCHANGED,
                "Berubah" to CheatNative.CMP_CHANGED
            ).forEach { (label, mode) ->
                OutlinedButton(
                    enabled = !busy,
                    contentPadding = tight,
                    modifier = Modifier.weight(1f),
                    onClick = {
                        val fromSnapshot = hasSnapshot
                        hasSnapshot = false
                        launchScan(
                            "Membandingkan",
                            { if (fromSnapshot) CheatNative.compare(mode, type) else CheatNative.refine(mode, 0, type, 0f) }
                        ) { "Tersisa $it alamat." }
                    }
                ) { Text(label, maxLines = 1) }
            }
        }

        Text(text = status, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.primary)

        if (rows.isNotEmpty()) {
            Text(
                text = "Hasil: $total (tampil ${rows.size})",
                style = MaterialTheme.typography.titleSmall,
                color = MaterialTheme.colorScheme.onSurface
            )
            rows.forEach { row ->
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.SpaceBetween
                ) {
                    Text(
                        text = "${addr(row.address)} = ${fmtValue(row.raw, rowsType)}",
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurface
                    )
                    TextButton(onClick = { searchEdit = row }) { Text("Edit") }
                }
            }
        }

        Text(
            text = "Daftar cheat (${items.size})",
            style = MaterialTheme.typography.titleMedium,
            color = MaterialTheme.colorScheme.onSurface
        )
        Text(
            text = if (dir.isEmpty()) "Daftar disimpan otomatis per game." else "Disimpan otomatis di $dir. Taruh file <ID game>.psv (format VitaCheat) di folder itu lalu tekan Muat ulang.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        if (items.isEmpty()) {
            Text(
                text = "Kosong. Dari hasil pencarian, tekan Edit lalu Tambah.",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
        items.forEach { item ->
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(10.dp)
            ) {
                Switch(
                    checked = item.enabled,
                    enabled = item.supported,
                    onCheckedChange = {
                        CheatNative.cheatSetEnabled(item.id, it)
                        refreshItems()
                    }
                )
                Column(
                    modifier = Modifier
                        .weight(1f)
                        .clickable(enabled = !item.psv) { listEdit = item }
                ) {
                    Text(
                        text = if (item.psv) item.name else item.name.ifBlank { addr(item.address) },
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurface
                    )
                    Text(
                        text = when {
                            item.psv && !item.supported -> "PSV, tipe kode belum didukung"
                            item.psv && item.enabled && item.info.isNotEmpty() -> "PSV: ${item.info}"
                            item.psv -> "PSV"
                            else -> "${addr(item.address)} = ${fmtValue(item.raw, item.type)} (${typeLabel(item.type)}), ketuk untuk edit"
                        },
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            }
        }
        Row(
            horizontalArrangement = Arrangement.spacedBy(6.dp),
            verticalAlignment = Alignment.CenterVertically,
            modifier = Modifier.fillMaxWidth()
        ) {
            OutlinedTextField(
                value = offsetText,
                onValueChange = { offsetText = it },
                singleLine = true,
                label = { Text("Offset alamat PSV (hex, boleh -)") },
                modifier = Modifier.weight(1f)
            )
            OutlinedButton(
                contentPadding = tight,
                onClick = {
                    val t = offsetText.trim().removePrefix("0x")
                    val neg = t.startsWith("-")
                    val v = t.removePrefix("-").toLongOrNull(16)
                    if (v == null) {
                        status = "Offset tidak valid."
                    } else {
                        CheatNative.cheatSetPsvOffset(if (neg) -v else v)
                        refreshItems()
                        status = "Offset PSV diterapkan."
                    }
                }
            ) { Text("Terapkan", maxLines = 1) }
        }
        Row(horizontalArrangement = Arrangement.spacedBy(6.dp), modifier = Modifier.fillMaxWidth()) {
            OutlinedButton(
                contentPadding = tight,
                modifier = Modifier.weight(1f),
                onClick = {
                    CheatNative.cheatSetAll(true)
                    refreshItems()
                }
            ) { Text("Semua ON", maxLines = 1) }
            OutlinedButton(
                contentPadding = tight,
                modifier = Modifier.weight(1f),
                onClick = {
                    CheatNative.cheatSetAll(false)
                    refreshItems()
                }
            ) { Text("Semua OFF", maxLines = 1) }
            OutlinedButton(
                contentPadding = tight,
                modifier = Modifier.weight(1f),
                onClick = {
                    val n = CheatNative.cheatReload()
                    refreshItems()
                    dir = CheatNative.cheatDir()
                    status = if (n < 0) "Game belum berjalan." else "Dimuat ulang: $n cheat."
                }
            ) { Text("Muat ulang", maxLines = 1) }
        }
    }

    searchEdit?.let { row ->
        EntryDialog(
            title = addr(row.address),
            type = rowsType,
            initialRaw = row.raw,
            initialName = "",
            actions = listOf(
                EntryAction("Tulis") { raw, _ ->
                    status = if (CheatNative.writeValue(row.address, raw, rowsType)) "Nilai ditulis." else "Gagal menulis."
                    rows = rows.map { if (it.address == row.address) it.copy(raw = raw) else it }
                },
                EntryAction("Tambah") { raw, name ->
                    CheatNative.writeValue(row.address, raw, rowsType)
                    CheatNative.cheatSetManual(row.address, raw, rowsType, true, name)
                    rows = rows.map { if (it.address == row.address) it.copy(raw = raw) else it }
                    refreshItems()
                    status = "Ditambahkan ke daftar cheat (ON). Matikan atau edit kapan saja."
                }
            ),
            onDelete = null,
            onDismiss = { searchEdit = null },
            onInvalid = { status = "Nilai tidak valid." }
        )
    }

    listEdit?.let { item ->
        EntryDialog(
            title = item.name.ifBlank { addr(item.address) },
            type = item.type,
            initialRaw = item.raw,
            initialName = item.name,
            actions = listOf(
                EntryAction("Simpan") { raw, name ->
                    CheatNative.cheatSetManual(item.address, raw, item.type, item.enabled, name)
                    refreshItems()
                }
            ),
            onDelete = {
                CheatNative.cheatRemove(item.id)
                refreshItems()
            },
            onDismiss = { listEdit = null },
            onInvalid = { status = "Nilai tidak valid." }
        )
    }
}
