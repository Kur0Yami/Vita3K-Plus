package org.vita3k.emulator.overlay

import android.app.Dialog
import android.content.Context
import android.os.Handler
import android.os.Looper
import android.widget.LinearLayout
import java.io.File
import android.widget.Toast
import org.vita3k.emulator.CheatNative

/**
 * Macro dari tombol M.
 * - M saat idle: buka panel (5 slot).
 * - M saat sedang merekam atau memutar: langsung dihentikan, panel tidak dibuka.
 * - Rekam dan Putar menutup panel, jadi game langsung bisa dimainkan.
 */
object MacroControl {
    private const val MODE_IDLE = 0
    private const val MODE_RECORDING = 1
    private const val MODE_PLAYING = 2
    private const val SLOTS = 5

    // Pilihan pengulangan: 1, 2, 5, atau 0 = terus.
    private val repeatOptions = listOf(1 to "1x", 2 to "2x", 5 to "5x", 0 to "Terus")
    private var repeat = 1

    private fun toast(context: Context, msg: String) {
        Toast.makeText(context, msg, Toast.LENGTH_SHORT).show()
    }

    /** Folder per game: <Emulated System Storage>/Auto/<ID game>/. ID diambil dari nama file cheat .psv. */
    private fun macroFile(context: Context): File {
        val psv = try {
            CheatNative.cheatList().lines().firstOrNull()?.split("\t")?.getOrNull(1)?.takeIf { it.isNotBlank() }
        } catch (e: Throwable) {
            null
        } ?: "default"
        val dir = File(context.getExternalFilesDir(null), "Auto/$psv")
        dir.mkdirs()
        return File(dir, "macro_slots.bin")
    }

    @JvmStatic
    fun show(context: Context) {
        try {
            when (CheatNative.macroMode()) {
                MODE_RECORDING -> {
                    val slot = CheatNative.macroActiveSlot()
                    CheatNative.macroRecordStop()
                    toast(context, "Rekam slot ${slot + 1} selesai (${CheatNative.macroLength(slot)} sampel)")
                    return
                }
                MODE_PLAYING -> {
                    CheatNative.macroPlayStop()
                    toast(context, "Putar dihentikan.")
                    return
                }
            }
        } catch (e: Throwable) {
            toast(context, "Macro gagal: ${e.message}")
            return
        }
        openPanel(context)
    }

    private fun openPanel(context: Context) {
        val container = PanelStyle.column(context)
        val handler = Handler(Looper.getMainLooper())
        var dialog: Dialog? = null
        val close: () -> Unit = { dialog?.dismiss() }

        fun render() {
            container.removeAllViews()
            container.addView(PanelStyle.title(context, "Macro"))
            for (i in 0 until SLOTS) container.addView(slotRow(context, i, ::render, close))
            container.addView(PanelStyle.spacer(context))

            val repeatRow = PanelStyle.row(context)
            repeatRow.addView(PanelStyle.label(context, "Ulang  "))
            repeatOptions.forEach { (value, text) ->
                PanelStyle.addPill(context, repeatRow, PanelStyle.pill(context, text, repeat == value) {
                    repeat = value
                    render()
                })
            }
            container.addView(repeatRow)
            container.addView(PanelStyle.spacer(context))

            val footer = PanelStyle.row(context)
            PanelStyle.addPill(context, footer, PanelStyle.pill(context, "Simpan") {
                try {
                    val f = macroFile(context)
                    val ok = CheatNative.macroSave(f.absolutePath)
                    toast(context, if (ok) "Tersimpan: ${f.parentFile?.name}" else "Gagal menyimpan.")
                } catch (e: Throwable) { toast(context, "Gagal: ${e.message}") }
            })
            PanelStyle.addPill(context, footer, PanelStyle.pill(context, "Muat") {
                try {
                    val f = macroFile(context)
                    if (!f.exists()) {
                        toast(context, "Belum ada rekaman tersimpan untuk game ini.")
                    } else {
                        val ok = CheatNative.macroLoad(f.absolutePath)
                        toast(context, if (ok) "Rekaman dimuat." else "Gagal muat (sedang rekam/putar atau file rusak).")
                    }
                } catch (e: Throwable) { toast(context, "Gagal: ${e.message}") }
                render()
            })
            PanelStyle.addPill(context, footer, PanelStyle.pill(context, "Tutup") { close() })
            container.addView(footer)
        }

        render()
        dialog = PanelStyle.show(context, container)

        // Segarkan status tiap 500 ms selama panel terbuka.
        val tick = object : Runnable {
            override fun run() {
                if (dialog?.isShowing == true) {
                    render()
                    handler.postDelayed(this, 500)
                }
            }
        }
        handler.postDelayed(tick, 500)
    }

    private fun slotRow(context: Context, slot: Int, refresh: () -> Unit, close: () -> Unit): LinearLayout {
        val mode = CheatNative.macroMode()
        val active = CheatNative.macroActiveSlot()
        val len = CheatNative.macroLength(slot)
        val isRec = mode == MODE_RECORDING && active == slot
        val isPlay = mode == MODE_PLAYING && active == slot

        val status = when {
            isRec -> "Merekam... $len sampel"
            isPlay -> "Memutar..."
            len > 0 -> "$len sampel (~${len / 60} dtk)"
            else -> "Kosong"
        }

        val wrap = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, 0, 0, PanelStyle.dp(context, 10))
        }
        wrap.addView(PanelStyle.label(context, "Slot ${slot + 1}"))
        wrap.addView(PanelStyle.sub(context, status))

        val btns = PanelStyle.row(context)
        PanelStyle.addPill(context, btns, PanelStyle.pill(context, if (isRec) "Stop" else "Rekam", isRec) {
            try {
                when {
                    isRec -> { CheatNative.macroRecordStop(); refresh() }
                    mode != MODE_IDLE -> { toast(context, "Hentikan dulu yang sedang berjalan."); refresh() }
                    else -> {
                        CheatNative.macroRecordStart(slot)
                        toast(context, "Rekam slot ${slot + 1}. Tekan M untuk berhenti.")
                        close()
                    }
                }
            } catch (e: Throwable) { toast(context, "Macro gagal: ${e.message}") }
        })
        PanelStyle.addPill(context, btns, PanelStyle.pill(context, if (isPlay) "Stop" else "Putar", isPlay) {
            try {
                when {
                    isPlay -> { CheatNative.macroPlayStop(); refresh() }
                    mode != MODE_IDLE -> { toast(context, "Hentikan dulu yang sedang berjalan."); refresh() }
                    len <= 0L -> { toast(context, "Slot ${slot + 1} masih kosong."); refresh() }
                    else -> {
                        CheatNative.macroPlayStart(slot, repeat)
                        toast(context, "Memutar slot ${slot + 1}. Tekan M untuk berhenti.")
                        close()
                    }
                }
            } catch (e: Throwable) { toast(context, "Macro gagal: ${e.message}") }
        })
        PanelStyle.addPill(context, btns, PanelStyle.pill(context, "Hapus") {
            if (mode != MODE_IDLE && active == slot) {
                toast(context, "Hentikan dulu sebelum menghapus.")
            } else {
                try { CheatNative.macroClear(slot) } catch (e: Throwable) { toast(context, "Gagal: ${e.message}") }
            }
            refresh()
        })
        wrap.addView(btns)
        return wrap
    }
}
