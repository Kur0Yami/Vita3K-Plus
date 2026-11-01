package org.vita3k.emulator.overlay

import android.app.Dialog
import android.content.Context
import android.content.SharedPreferences
import android.widget.Toast
import org.vita3k.emulator.CheatNative
import org.vita3k.emulator.overlay.PanelStyle as S

/**
 * Panel cheat dari tombol C. Cheat yang dipin disimpan per game (kunci = nama file .psv).
 */
object CheatPanel {
    private const val PREFS = "cheat_panel"

    private class Row(val id: Long, val psv: String, val enabled: Boolean, val name: String)

    private fun loadRows(): List<Row> {
        val raw = try { CheatNative.cheatList() } catch (e: Throwable) { "" }
        if (raw.isBlank()) return emptyList()
        return raw.lines().mapNotNull { line ->
            val f = line.split("\t", limit = 9) // id, psv, enabled, supported, address, type, raw, info, name
            if (f.size < 9) return@mapNotNull null
            val id = f[0].toLongOrNull() ?: return@mapNotNull null
            Row(id, f[1], f[2] == "1" || f[2] == "true", f[8])
        }
    }

    private fun prefs(context: Context): SharedPreferences =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    private fun pinKey(psv: String) = "pins_" + psv.ifBlank { "default" }

    private fun pins(context: Context, psv: String): Set<String> =
        prefs(context).getStringSet(pinKey(psv), emptySet()) ?: emptySet()

    private fun savePins(context: Context, psv: String, names: Set<String>) {
        prefs(context).edit().putStringSet(pinKey(psv), HashSet(names)).apply()
    }

    @JvmStatic
    fun show(context: Context) {
        val rows = loadRows()
        if (rows.isEmpty()) {
            Toast.makeText(context, "Cheat tidak terbaca. Pastikan game sedang berjalan.", Toast.LENGTH_SHORT).show()
            return
        }
        val psv = rows.first().psv
        val pinned = pins(context, psv)
        val shown = rows.filter { it.name in pinned }

        val col = S.column(context)
        col.addView(S.title(context, "Cheat"))
        if (shown.isEmpty()) {
            col.addView(S.sub(context, "Belum ada cheat yang dipin. Tekan Kelola untuk memilih."))
        }
        shown.forEach { row ->
            col.addView(S.switchRow(context, row.name, row.enabled) { checked ->
                try { CheatNative.cheatSetEnabled(row.id, checked) } catch (e: Throwable) {
                    Toast.makeText(context, "Gagal: ${e.message}", Toast.LENGTH_SHORT).show()
                }
            })
        }
        col.addView(S.spacer(context))

        lateinit var dialog: Dialog
        val footer = S.row(context)
        S.addPill(context, footer, S.pill(context, "Kelola") { dialog.dismiss(); showManage(context, rows, psv) })
        S.addPill(context, footer, S.pill(context, "Acak ulang") { reroll(context, shown) })
        S.addPill(context, footer, S.pill(context, "Tutup") { dialog.dismiss() })
        col.addView(footer)
        dialog = S.show(context, col)
    }

    /** Hapus pilihan angka acak, lalu matikan dan nyalakan lagi cheat yang dipin supaya angkanya dipilih ulang. */
    private fun reroll(context: Context, pinnedRows: List<Row>) {
        try {
            CheatNative.resetRandom()
            pinnedRows.forEach { row ->
                if (row.enabled) {
                    CheatNative.cheatSetEnabled(row.id, false)
                    CheatNative.cheatSetEnabled(row.id, true)
                }
            }
            Toast.makeText(context, "Angka acak diacak ulang.", Toast.LENGTH_SHORT).show()
        } catch (e: Throwable) {
            Toast.makeText(context, "Gagal acak ulang: ${e.message}", Toast.LENGTH_SHORT).show()
        }
    }

    private fun showManage(context: Context, rows: List<Row>, psv: String) {
        val current = pins(context, psv)
        val chosen = HashSet<String>(current)
        val col = S.column(context)
        col.addView(S.title(context, "Pilih cheat yang dipin"))
        rows.forEach { row ->
            col.addView(S.checkRow(context, row.name, row.name in current) { checked ->
                if (checked) chosen.add(row.name) else chosen.remove(row.name)
            })
        }
        col.addView(S.spacer(context))
        lateinit var dialog: Dialog
        val footer = S.row(context)
        S.addPill(context, footer, S.pill(context, "Simpan") {
            savePins(context, psv, chosen)
            dialog.dismiss()
            show(context)
        })
        S.addPill(context, footer, S.pill(context, "Batal") { dialog.dismiss() })
        col.addView(footer)
        dialog = S.show(context, col)
    }
}
