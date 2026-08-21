package dev.primolab.miner

import android.util.Log

/**
 * Optional, best-effort root booster (STUB).
 *
 * A normal APK cannot make itself `top-app` — that tier is reserved for the
 * focused app, so a backgrounded miner is throttled (cpuset core-withholding +
 * uclamp freq-cap). The ONLY way to get full cores while backgrounded/screen-off
 * is root: move the miner's threads into the `top-app` cpuset cgroup (exactly
 * what `adb shell`/`ssh` sessions get for free).
 *
 * This is a no-op without root. With root it writes the miner process into
 * /dev/cpuset/top-app/cgroup.procs (cgroup-v1; moves all current threads, and
 * threads spawned afterwards inherit the cpuset).
 *
 * STUB status: functional but minimal — no periodic re-assert, no UI toggle yet.
 */
object RootBooster {

    private const val TAG = "RootBooster"
    private const val TOP_APP_PROCS = "/dev/cpuset/top-app/cgroup.procs"
    private const val SU_TIMEOUT_MS = 5000L

    @Volatile private var rootCache: Boolean? = null

    /**
     * Run `su -c cmd` with a hard timeout (a pending root-manager prompt would
     * otherwise block readText()/waitFor() forever — minSdk 24 has no
     * Process.waitFor(timeout), so a watchdog thread destroys the process).
     * Returns exitCode to output, or null on timeout/launch failure.
     */
    private fun runSu(cmd: String): Pair<Int, String>? = runCatching {
        val p = ProcessBuilder("su", "-c", cmd).redirectErrorStream(true).start()
        val watchdog = Thread {
            try { Thread.sleep(SU_TIMEOUT_MS) } catch (_: InterruptedException) { return@Thread }
            p.destroy()
        }.apply { isDaemon = true; start() }
        val out = p.inputStream.bufferedReader().readText()
        val code = p.waitFor()
        val timedOut = !watchdog.isAlive       // watchdog fired = we were destroyed
        watchdog.interrupt()
        if (timedOut) null else code to out
    }.getOrNull()

    /** True if `su` grants root. Definitive answers are cached; a timeout or
     *  launch failure is NOT (the user may grant the prompt later). */
    fun isRootAvailable(): Boolean {
        rootCache?.let { return it }
        val result = runSu("id -u") ?: return false
        val (code, out) = result
        // Exit code must be success AND some line must be exactly "0" — a
        // substring check would misclassify uid 1000 (or any output that
        // merely contains a zero) as root.
        val ok = code == 0 && out.lines().any { it.trim() == "0" }
        rootCache = ok
        return ok
    }

    /**
     * Move the running miner ("libprimo.so") into the top-app cpuset.
     * Best-effort: logs and returns false if root is unavailable or it fails.
     */
    fun boostToTopApp(): Boolean {
        if (!isRootAvailable()) {
            Log.i(TAG, "no root — staying in the platform-assigned cpuset (throttled when backgrounded)")
            return false
        }
        // pidof gives the miner's pid; cgroup.procs moves the whole thread group.
        val cmd = "for p in \$(pidof libprimo.so); do echo \$p > $TOP_APP_PROCS; done"
        val result = runSu(cmd) ?: run {
            Log.w(TAG, "boost timed out")
            return false
        }
        val (code, out) = result
        return if (code == 0) {
            Log.i(TAG, "boosted miner into top-app cpuset")
            true
        } else {
            Log.w(TAG, "boost failed (exit $code): $out")
            false
        }
    }
}
