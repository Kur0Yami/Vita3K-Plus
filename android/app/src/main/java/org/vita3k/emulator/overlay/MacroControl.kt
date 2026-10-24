package org.vita3k.emulator.overlay

import android.content.Context
import android.widget.Toast
import org.vita3k.emulator.CheatNative

/**
 * Tombol REC dan PLAY di overlay.
 * REC: mulai rekam, tekan lagi untuk berhenti.
 * PLAY: putar rekaman, tekan lagi untuk berhenti.
 */
object MacroControl {
    private const val MODE_IDLE = 0
    private const val MODE_RECORDING = 1
    private const val MODE_PLAYING = 2

    private fun toast(context: Context, msg: String) {
        Toast.makeText(context, msg, Toast.LENGTH_SHORT).show()
    }

    @JvmStatic
    fun onRecordPressed(context: Context) {
        try {
            when (CheatNative.macroMode()) {
                MODE_RECORDING -> {
                    CheatNative.macroRecordStop()
                    toast(context, "Rekam selesai: ${CheatNative.macroLength()} sampel")
                }
                MODE_PLAYING -> toast(context, "Sedang memutar. Tekan PLAY untuk berhenti.")
                else -> {
                    CheatNative.macroRecordStart()
                    toast(context, "Rekam dimulai. Tekan REC lagi untuk berhenti.")
                }
            }
        } catch (e: Throwable) {
            toast(context, "Macro gagal: ${e.message}")
        }
    }

    @JvmStatic
    fun onPlayPressed(context: Context) {
        try {
            when (CheatNative.macroMode()) {
                MODE_PLAYING -> {
                    CheatNative.macroPlayStop()
                    toast(context, "Putar dihentikan.")
                }
                MODE_RECORDING -> toast(context, "Sedang merekam. Tekan REC untuk berhenti dulu.")
                else -> {
                    if (CheatNative.macroLength() <= 0L) {
                        toast(context, "Belum ada rekaman.")
                    } else {
                        CheatNative.macroPlayStart(false)
                        toast(context, "Memutar rekaman.")
                    }
                }
            }
        } catch (e: Throwable) {
            toast(context, "Macro gagal: ${e.message}")
        }
    }
}
