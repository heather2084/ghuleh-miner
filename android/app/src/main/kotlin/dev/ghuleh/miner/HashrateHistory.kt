package dev.ghuleh.miner

import android.content.Context
import org.json.JSONObject
import java.io.File

/**
 * Rolling 24-hour average hashrate, independent of whether the app is on
 * screen. MinerService samples the local API every SAMPLE_INTERVAL_SEC while
 * mining is active and calls [recordSample]; MiningActivity reads
 * [average24hKhs] each poll to render the "24H AVG" stat tile.
 *
 * Storage: one bucket per wall-clock hour (epoch seconds / 3600), each
 * holding the total kilohashes done during that hour (khs * sampling
 * interval, summed). The average is total kilohashes across the last 24
 * buckets divided by 86400 seconds — a real 24-hour WALL-CLOCK average, the
 * same convention pool dashboards use: time the app wasn't mining (closed,
 * phone off, no config) counts as zero rather than being excluded, so the
 * number reflects "how much did this phone actually average today", not
 * just "how fast is it while running". Buckets older than 24h are pruned on
 * every read/write, so the file never grows and old sessions age out on
 * their own — nothing to reset manually between mining runs.
 */
object HashrateHistory {

    const val SAMPLE_INTERVAL_SEC = 30L
    private const val WINDOW_SEC = 24 * 3600L

    @Volatile private var cache: JSONObject? = null

    private fun file(ctx: Context) = File(ctx.filesDir, "hashrate_history.json")

    @Synchronized
    private fun load(ctx: Context): JSONObject {
        cache?.let { return it }
        val f = file(ctx)
        val loaded = if (f.exists()) {
            runCatching { JSONObject(f.readText()) }.getOrNull() ?: JSONObject()
        } else JSONObject()
        cache = loaded
        return loaded
    }

    /** Drop bucket keys older than the 24h window, relative to [nowSec]. */
    private fun prune(root: JSONObject, nowSec: Long) {
        val cutoffHour = (nowSec - WINDOW_SEC) / 3600
        val stale = root.keys().asSequence().filter { key ->
            key.toLongOrNull()?.let { it <= cutoffHour } ?: true  // drop unparsable keys too
        }.toList()
        for (key in stale) root.remove(key)
    }

    /**
     * Record one sample: [khs] was the miner's reported hashrate for the
     * last [intervalSec] seconds. Call this from a periodic sampler while —
     * and only while — mining is actually running; a gap in calls (idle,
     * app/service not running) is exactly what should read as zero later.
     */
    @Synchronized
    fun recordSample(ctx: Context, khs: Double, intervalSec: Long = SAMPLE_INTERVAL_SEC) {
        if (khs <= 0.0 || intervalSec <= 0) return
        val now = System.currentTimeMillis() / 1000
        val root = load(ctx)
        prune(root, now)
        val hourKey = (now / 3600).toString()
        val kilohashes = khs * intervalSec
        val existing = root.optDouble(hourKey, 0.0)
        root.put(hourKey, existing + kilohashes)
        file(ctx).writeTextAtomic(root.toString())
        cache = root
    }

    /** Average kH/s over the trailing 24 wall-clock hours (0 if no samples yet). */
    @Synchronized
    fun average24hKhs(ctx: Context): Double {
        val now = System.currentTimeMillis() / 1000
        val root = load(ctx)
        prune(root, now)
        var totalKilohashes = 0.0
        for (key in root.keys()) totalKilohashes += root.optDouble(key, 0.0)
        return totalKilohashes / WINDOW_SEC
    }
}
