package dev.primolab.miner

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.widget.Toast

/**
 * The primolab.dev pages the app links back to (config help popups, About).
 * One place so the URLs can't drift from the site.
 */
object Links {
    const val SITE = "https://primolab.dev/"
    const val FAQ = "https://primolab.dev/faq.html"
    const val DOCS = "https://primolab.dev/docs.html"

    /** Per-coin page: recommended pools + wallet setup for [algo]. */
    fun coinPage(algo: String): String = when (algo) {
        "verus" -> "https://primolab.dev/mine-verus.html"
        "randomx" -> "https://primolab.dev/mine-monero.html"
        "scrypt" -> "https://primolab.dev/mine-litecoin-dogecoin.html"
        "sha256d" -> "https://primolab.dev/mine-bitcoin.html"
        else -> SITE
    }

    fun open(ctx: Context, url: String) {
        try {
            ctx.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)))
        } catch (e: Exception) {
            // No browser on the device — nothing sensible to do beyond saying so.
            Toast.makeText(ctx, "No browser app available", Toast.LENGTH_SHORT).show()
        }
    }
}
