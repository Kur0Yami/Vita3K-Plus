package org.vita3k.emulator

/**
 * JNI bridge to the save state code (vita3k/android/jni/native_state.cpp).
 * Files go to <emulated system storage>/savestate/<title id>/slot-N.v3ks.
 */
object StateNative {
    const val SLOT_COUNT = 3

    /** Saves slot [slot] (1..3). The game must be paused. Returns a report, or a text starting with "ERR:". */
    external fun saveSlot(slot: Int): String

    /**
     * Loads slot [slot] (1..3), same session only. The game must be paused. It checks that every thread is where
     * the file says and changes nothing otherwise. Returns a report; failures start with "GAGAL" or "ERR:".
     */
    external fun loadSlot(slot: Int): String

    /** "" when the slot is empty, otherwise "<bytes>|<unix seconds>". */
    external fun slotInfo(slot: Int): String
}
