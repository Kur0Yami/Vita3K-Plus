package org.vita3k.emulator.overlay

import android.app.Dialog
import android.content.Context
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.ColorDrawable
import android.graphics.drawable.GradientDrawable
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.view.Window
import android.widget.CheckBox
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Switch
import android.widget.TextView

/**
 * Gaya panel overlay: kartu gelap membulat, teks putih, aksen oranye, tombol pil berbingkai.
 * Dibuat dengan View biasa karena panel tampil di atas SDLActivity (bukan Compose).
 */
object PanelStyle {
    const val ACCENT = 0xFFFF9800.toInt()
    const val BG = 0xFF1C1C1E.toInt()
    const val TEXT = 0xFFFFFFFF.toInt()
    const val SUB = 0xFF9E9E9E.toInt()
    private const val OFF = 0xFF5A5A5E.toInt()

    fun dp(context: Context, v: Int): Int = (v * context.resources.displayMetrics.density).toInt()

    private fun rounded(context: Context, fill: Int, radiusDp: Int, stroke: Int, strokeDp: Int) =
        GradientDrawable().apply {
            shape = GradientDrawable.RECTANGLE
            cornerRadius = dp(context, radiusDp).toFloat()
            setColor(fill)
            if (strokeDp > 0) setStroke(dp(context, strokeDp), stroke)
        }

    fun column(context: Context): LinearLayout = LinearLayout(context).apply {
        orientation = LinearLayout.VERTICAL
        background = rounded(context, BG, 24, 0, 0)
        val p = dp(context, 20)
        setPadding(p, p, p, dp(context, 16))
    }

    fun row(context: Context): LinearLayout = LinearLayout(context).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
    }

    fun spacer(context: Context, dpHeight: Int = 12): View = View(context).apply {
        layoutParams = LinearLayout.LayoutParams(1, dp(context, dpHeight))
    }

    fun title(context: Context, text: String): TextView = TextView(context).apply {
        this.text = text
        setTextColor(TEXT)
        textSize = 18f
        setTypeface(null, Typeface.BOLD)
        setPadding(0, 0, 0, dp(context, 12))
    }

    fun label(context: Context, text: String): TextView = TextView(context).apply {
        this.text = text
        setTextColor(TEXT)
        textSize = 15f
    }

    fun sub(context: Context, text: String): TextView = TextView(context).apply {
        this.text = text
        setTextColor(SUB)
        textSize = 12f
    }

    /** Tombol pil. Selected = terisi oranye. */
    fun pill(context: Context, text: String, selected: Boolean = false, onClick: () -> Unit): TextView =
        TextView(context).apply {
            this.text = text
            textSize = 14f
            gravity = Gravity.CENTER
            setTextColor(if (selected) BG else ACCENT)
            setPadding(dp(context, 14), dp(context, 8), dp(context, 14), dp(context, 8))
            background = rounded(context, if (selected) ACCENT else Color.TRANSPARENT, 50, ACCENT, 1)
            isClickable = true
            setOnClickListener { onClick() }
        }

    fun addPill(context: Context, parent: LinearLayout, view: View) {
        val lp = LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT)
        lp.rightMargin = dp(context, 8)
        parent.addView(view, lp)
    }

    fun switchRow(context: Context, text: String, checked: Boolean, onChange: (Boolean) -> Unit): LinearLayout {
        val r = row(context)
        val labelView = label(context, text).apply {
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        }
        r.addView(labelView)
        val sw = Switch(context).apply {
            isChecked = checked
            val states = arrayOf(intArrayOf(android.R.attr.state_checked), intArrayOf())
            thumbTintList = ColorStateList(states, intArrayOf(ACCENT, TEXT))
            trackTintList = ColorStateList(states, intArrayOf(0x66FF9800, OFF))
            setOnCheckedChangeListener { _, c -> onChange(c) }
        }
        r.addView(sw)
        return r
    }

    fun checkRow(context: Context, text: String, checked: Boolean, onChange: (Boolean) -> Unit): CheckBox =
        CheckBox(context).apply {
            this.text = text
            setTextColor(TEXT)
            buttonTintList = ColorStateList.valueOf(ACCENT)
            isChecked = checked
            setOnCheckedChangeListener { _, c -> onChange(c) }
        }

    /** Tampilkan konten dalam dialog gaya panel. Mengembalikan Dialog agar bisa ditutup. */
    fun show(context: Context, content: View): Dialog {
        val dialog = Dialog(context)
        dialog.requestWindowFeature(Window.FEATURE_NO_TITLE)
        val wrap = ScrollView(context).apply {
            addView(content)
            val m = dp(context, 16)
            setPadding(m, m, m, m)
        }
        dialog.setContentView(wrap)
        dialog.window?.apply {
            setBackgroundDrawable(ColorDrawable(Color.TRANSPARENT))
            val screenW = context.resources.displayMetrics.widthPixels
            val maxW = minOf((screenW * 0.42f).toInt(), dp(context, 380))
            setLayout(maxW, ViewGroup.LayoutParams.WRAP_CONTENT)
        }
        dialog.show()
        return dialog
    }
}
