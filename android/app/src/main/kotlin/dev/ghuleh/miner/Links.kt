package dev.ghuleh.miner

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.widget.Toast

/**
 * Where the app's help popups and About dialog link out to. One place so the
 * URLs can't drift.
 *
 * Ghuleh Miner has no dedicated docs site (unlike upstream primolab.dev) —
 * everything currently points at the GitHub repo. NOTE: that repo is private
 * as of this writing, so these links will 404 for anyone without access.
 * Make the repo public (or swap in a real docs site here) before handing
 * this APK to people other than yourself. Per-algo coinPage() is kept as a
 * seam for later — point it at README anchors or real per-coin docs pages
 * once they exist instead of collapsing to SITE for everything.
 */
object Links {
    const val SITE = "https://github.com/heather2084/ghuleh-miner"
    const val FAQ = "https://github.com/heather2084/ghuleh-miner#readme"
    const val DOCS = "https://github.com/heather2084/ghuleh-miner#readme"

    /** Per-coin page: recommended pools + wallet setup for [algo]. TODO: real per-coin docs. */
    fun coinPage(algo: String): String = SITE

    fun open(ctx: Context, url: String) {
        try {
            ctx.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)))
        } catch (e: Exception) {
            // No browser on the device — nothing sensible to do beyond saying so.
            Toast.makeText(ctx, "No browser app available", Toast.LENGTH_SHORT).show()
        }
    }
}
