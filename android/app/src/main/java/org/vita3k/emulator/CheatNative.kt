package org.vita3k.emulator

/**
 * JNI bridge to the live memory scanner (vita3k/android/jni/native_cheat.cpp).
 * Scan functions return a candidate count (or byte count for snapshot); negative values are errors:
 * -1 snapshot too large, -2 no snapshot, -3 no game running.
 */
object CheatNative {
    const val TYPE_INT = 0
    const val TYPE_FLOAT = 1
    const val TYPE_BYTE = 2
    const val TYPE_SHORT = 3

    const val CMP_INCREASED = 0
    const val CMP_DECREASED = 1
    const val CMP_UNCHANGED = 2
    const val CMP_CHANGED = 3

    external fun clear()
    external fun search(raw: Int, type: Int, tolerance: Float): Long
    external fun searchBytes(pattern: ByteArray): Long

    /** Byte dari teks: ASCII (1 byte per huruf) atau UTF-16LE (2 byte per huruf). */
    fun textPattern(text: String, utf16: Boolean): ByteArray =
        if (utf16) text.toByteArray(Charsets.UTF_16LE) else text.toByteArray(Charsets.US_ASCII)
    external fun snapshot(): Long
    external fun compare(mode: Int, type: Int): Long
    /** mode < 0 = exact match against raw, otherwise compare with each candidate's previous value. */
    external fun refine(mode: Int, raw: Int, type: Int, tolerance: Float): Long
    external fun count(): Long
    /** Interleaved [address, value, ...] */
    external fun results(offset: Int, max: Int): LongArray
    /** Raw value or Long.MIN_VALUE when unreadable. */
    external fun readValue(address: Long, type: Int): Long
    external fun writeValue(address: Long, raw: Int, type: Int): Boolean
    /** One line per item: id \t psv \t enabled \t supported \t address \t type \t raw \t info \t name */
    external fun cheatList(): String
    external fun cheatSetManual(address: Long, raw: Int, type: Int, enabled: Boolean, name: String)
    external fun cheatSetEnabled(id: Long, enabled: Boolean)
    external fun cheatRemove(id: Long)
    external fun cheatSetAll(enabled: Boolean)
    /** Re-reads the list and the .psv file; returns the item count or -1 when no game runs. */
    external fun cheatReload(): Int
    external fun cheatDir(): String
    /** Added to every static address of the .psv codes (signed). */
    external fun cheatSetPsvOffset(offset: Long)
    external fun cheatPsvOffset(): Long
}
