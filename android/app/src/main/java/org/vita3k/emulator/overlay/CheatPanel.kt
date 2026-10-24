package org.vita3k.emulator.overlay

import android.app.AlertDialog
import android.content.Context
import android.content.SharedPreferences
import android.graphics.Color
import android.view.ViewGroup
import android.widget.Button
import android.widget.CheckBox
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast
import org.vita3k.emulator.CheatNative

/**
 * Panel cheat yang muncul dari tombol floating di overlay.
 * Cheat yang dipin disimpan per game (kunci = nama file .psv, mis. PCSE00789).
 */
object CheatPanel {
    private const val PREFS = "cheat_panel"

    private class Row(val id: Long, val psv: String, val enabled: Boolean, val name: String)

    private fun loadRows(): List<Row> {
        val raw = try {
            CheatNative.cheatList()
        } catch (e: Throwable) {
            ""
        }
        if (raw.isBlank()) return emptyList()
        return raw.lines().mapNotNull { line ->
            // id \t psv \t enabled \t supported \t address \t type \t raw \t info \t name
            val f = line.split("\t", limit = 9)
            if (f.size < 9) return@mapNotNull null
            val id = f[0].toLongOrNull() ?: return@mapNotNull null
            Row(id, f[1], f[2] == "1" || f[2] == "true", f[8])
        }
    }

    private fun prefs(context: Context): SharedPreferences =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    private fun pinKey(psv: String) = "pins_" + (psv.ifBlank { "default" })

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

        val box = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(32, 16, 32, 16)
        }

        if (shown.isEmpty()) {
            box.addView(TextView(context).apply {
                text = "Belum ada cheat yang dipin. Tekan Kelola untuk memilih."
                setTextColor(Color.GRAY)
            })
        }

        shown.forEach { row ->
            box.addView(Switch(context).apply {
                text = row.name
                isChecked = row.enabled
                setOnCheckedChangeListener { _, checked ->
                    try {
                        CheatNative.cheatSetEnabled(row.id, checked)
                    } catch (e: Throwable) {
                        Toast.makeText(context, "Gagal: ${e.message}", Toast.LENGTH_SHORT).show()
                    }
                }
            })
        }

        val scroll = ScrollView(context).apply {
            addView(box, ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
        }

        AlertDialog.Builder(context)
            .setTitle("Cheat")
            .setView(scroll)
            .setPositiveButton("Kelola") { _, _ -> showManage(context, rows, psv) }
            .setNeutralButton("Acak ulang") { _, _ -> reroll(context, shown) }
            .setNegativeButton("Tutup", null)
            .show()
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
        val box = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(32, 16, 32, 16)
        }
        val checks = rows.map { row ->
            CheckBox(context).apply {
                text = row.name
                isChecked = row.name in current
            }.also { box.addView(it) }
        }
        val scroll = ScrollView(context).apply { addView(box) }

        AlertDialog.Builder(context)
            .setTitle("Pilih cheat yang dipin")
            .setView(scroll)
            .setPositiveButton("Simpan") { _, _ ->
                val chosen = checks.filter { it.isChecked }.map { it.text.toString() }.toSet()
                savePins(context, psv, chosen)
                show(context)
            }
            .setNegativeButton("Batal", null)
            .show()
    }
}
