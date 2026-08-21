package dev.primolab.miner

import android.graphics.Color
import android.graphics.drawable.GradientDrawable

/**
 * Per-algorithm accent palette. Each algorithm carries its coin's brand color
 * (lifted where the brand shade is too dark to read on the near-black
 * background); everything that "glows" in the UI — the hero card, hashrate,
 * MINING status, active thread chips, START pill, config section headers —
 * tints through here, so switching algorithms re-skins the whole app.
 */
object Palette {
    private const val BG = 0xFF0B0C10.toInt()          // window background
    val TEAL = 0xFF2FC6B5.toInt()                      // app brand (site teal) / unknown algo

    fun accentFor(algo: String?): Int = when (algo?.trim()?.lowercase()) {
        "verus" -> 0xFF5B8DEF.toInt()                  // Verus blue (#3165D4 lifted)
        "sha256d" -> 0xFFF7931A.toInt()                // Bitcoin orange
        "scrypt" -> 0xFF9BB5E3.toInt()                 // Litecoin silver-blue (#345D9D lifted)
        "randomx" -> 0xFFFF6B00.toInt()                // Monero orange
        else -> TEAL
    }

    /** Mix [src] over the window background at [f] opacity (dim accent fills). */
    fun dim(src: Int, f: Float): Int {
        fun ch(s: Int, d: Int) = (s * f + d * (1f - f)).toInt().coerceIn(0, 255)
        return Color.rgb(
            ch(Color.red(src), Color.red(BG)),
            ch(Color.green(src), Color.green(BG)),
            ch(Color.blue(src), Color.blue(BG)))
    }

    /** Hero panel: soft accent gradient wash with an accent-tinted edge. */
    fun heroCard(accent: Int, density: Float): GradientDrawable =
        GradientDrawable(
            GradientDrawable.Orientation.TL_BR,
            intArrayOf(dim(accent, 0.22f), dim(accent, 0.06f))
        ).apply {
            cornerRadius = 22f * density
            setStroke((1f * density).toInt().coerceAtLeast(1), dim(accent, 0.45f))
        }

    /** Small rounded badge behind the algorithm name in the hero. */
    fun badge(accent: Int, density: Float): GradientDrawable =
        GradientDrawable().apply {
            setColor(dim(accent, 0.18f))
            cornerRadius = 9f * density
            setStroke((1f * density).toInt().coerceAtLeast(1), dim(accent, 0.55f))
        }
}
