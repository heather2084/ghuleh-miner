package dev.primolab.miner

import android.app.Activity
import android.content.Intent
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.SpannableStringBuilder
import android.text.Spanned
import android.text.style.ForegroundColorSpan
import android.view.Menu
import android.view.MenuItem
import android.view.View
import android.widget.ScrollView
import android.widget.TextView
import java.io.File

/**
 * Read-only viewer for the miner's log (filesDir/miner.log, the drained stdout
 * of the native subprocess — one session, MinerService truncates it per start).
 * Tails the last chunk, strips the miner's ANSI color codes and re-colors lines
 * with the app palette (same convention as the site terminals: dim timestamps,
 * green reserved for "Accepted"), and auto-refreshes while visible. Auto-scroll
 * follows the tail only while the view is at the bottom; a "↓ LATEST" pill
 * appears when scrolled up. The action bar's share button sends the visible
 * tail plus app/device info — the "send me your log" tester flow.
 */
class LogActivity : Activity() {

    private val handler = Handler(Looper.getMainLooper())
    private val ansi = Regex("\u001B?\\[[0-9;]*m")   // strip the miner's color codes
    private val timestamp = Regex("^\\[\\d{2}:\\d{2}:\\d{2}\\]")
    // Whole-line tints. Errors win over warnings; the share-result words are
    // colored separately so "Accepted" stays the only green (site convention).
    private val errorish = Regex(
        "\\b(error|failed|failure|rejected|reject reason|cannot|could not|unreachable|refused)\\b",
        RegexOption.IGNORE_CASE)
    private val warnish = Regex(
        "\\b(failing over|reconnect|timeout|retry|retrying|disconnect|stale|clamping)\\b",
        RegexOption.IGNORE_CASE)

    private lateinit var text: TextView
    private lateinit var scroll: ScrollView
    private lateinit var jump: TextView
    private var lastClean = ""

    private val refresh = object : Runnable {
        override fun run() { load(); handler.postDelayed(this, 2000) }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_log)
        actionBar?.setDisplayHomeAsUpEnabled(true)
        actionBar?.title = getString(R.string.logs_title)
        text = findViewById(R.id.logText)
        scroll = findViewById(R.id.logScroll)
        jump = findViewById(R.id.jumpLatest)
        jump.setOnClickListener {
            scroll.post { scroll.fullScroll(ScrollView.FOCUS_DOWN) }
        }
        scroll.setOnScrollChangeListener { _, _, _, _, _ -> updateJumpPill() }
    }

    override fun onResume() { super.onResume(); handler.post(refresh) }
    override fun onPause() { super.onPause(); handler.removeCallbacks(refresh) }

    /** Pill is only useful when there is somewhere further down to go. */
    private fun updateJumpPill() {
        jump.visibility = if (scroll.canScrollVertically(1)) View.VISIBLE else View.GONE
    }

    private fun load() {
        val f = File(filesDir, "miner.log")
        val raw = when {
            !f.exists() || f.length() == 0L -> "No log yet — start mining first."
            else -> try {
                val max = 64 * 1024L                       // tail the last 64 KB
                if (f.length() > max) {
                    f.inputStream().use { it.skip(f.length() - max); it.readBytes() }
                        .toString(Charsets.UTF_8).substringAfter('\n')
                } else f.readText()
            } catch (e: Exception) { "Could not read log: ${e.message}" }
        }
        val clean = ansi.replace(raw, "").trimEnd()
        if (clean != lastClean) {
            lastClean = clean
            // Keep auto-scroll only when the user is already near the bottom.
            val atBottom = !scroll.canScrollVertically(1)
            text.text = colorize(clean)
            scroll.post {
                if (atBottom) scroll.fullScroll(ScrollView.FOCUS_DOWN)
                updateJumpPill()
            }
        }
    }

    private fun colorize(log: String): CharSequence {
        val dim = getColor(R.color.textdim)
        val ok = getColor(R.color.log_ok)
        val bad = getColor(R.color.danger)
        val warn = getColor(R.color.warn)
        val sb = SpannableStringBuilder()
        for (line in log.lineSequence()) {
            val start = sb.length
            sb.append(line).append('\n')
            val ts = timestamp.find(line)
            if (ts != null) {
                sb.setSpan(ForegroundColorSpan(dim), start, start + ts.value.length,
                    Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
            }
            val bodyStart = start + (ts?.value?.length ?: 0)
            when {
                // Share results: tint only the verdict word, keep the rest plain.
                line.contains("Accepted") -> {
                    val i = start + line.indexOf("Accepted")
                    sb.setSpan(ForegroundColorSpan(ok), i, i + "Accepted".length,
                        Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
                }
                line.contains("Rejected") -> {
                    val i = start + line.indexOf("Rejected")
                    sb.setSpan(ForegroundColorSpan(bad), i, i + "Rejected".length,
                        Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
                }
                errorish.containsMatchIn(line) ->
                    sb.setSpan(ForegroundColorSpan(bad), bodyStart, start + line.length,
                        Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
                warnish.containsMatchIn(line) ->
                    sb.setSpan(ForegroundColorSpan(warn), bodyStart, start + line.length,
                        Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
            }
        }
        if (sb.isNotEmpty()) sb.delete(sb.length - 1, sb.length)   // trailing \n
        return sb
    }

    override fun onCreateOptionsMenu(menu: Menu): Boolean {
        menuInflater.inflate(R.menu.log_menu, menu)
        return true
    }

    override fun onOptionsItemSelected(item: MenuItem): Boolean {
        when (item.itemId) {
            android.R.id.home -> { finish(); return true }
            R.id.action_share_log -> { shareLog(); return true }
        }
        return super.onOptionsItemSelected(item)
    }

    /** Share the visible tail + app/device info (what a bug report needs). */
    private fun shareLog() {
        // The non-deprecated PackageInfoFlags overload needs API 33; minSdk is 24.
        @Suppress("DEPRECATION")
        val version = try {
            packageManager.getPackageInfo(packageName, 0).versionName ?: "?"
        } catch (e: Exception) { "?" }
        val body = buildString {
            append("Primo ARM Miner ").append(version)
            append(" — ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL)
            append(", Android ").append(Build.VERSION.RELEASE).append("\n\n")
            append(lastClean.ifBlank { "(log empty)" })
        }
        val send = Intent(Intent.ACTION_SEND)
            .setType("text/plain")
            .putExtra(Intent.EXTRA_SUBJECT, "Primo ARM Miner log")
            .putExtra(Intent.EXTRA_TEXT, body)
        try {
            startActivity(Intent.createChooser(send, getString(R.string.share_log)))
        } catch (e: Exception) { /* no share targets — nothing to do */ }
    }
}
