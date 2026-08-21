package dev.primolab.miner

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import android.util.Log
import org.json.JSONObject
import java.io.File
import java.net.InetAddress

/**
 * Foreground service that runs the native miner as a subprocess.
 *
 * The miner ships inside the APK as lib/arm64-v8a/libprimo.so and is extracted
 * to nativeLibraryDir (the one app dir mounted executable), so we can exec it
 * directly. It reads a config.json we write to filesDir and serves the
 * read-only status API on 127.0.0.1:4068 for MiningActivity to poll.
 */
class MinerService : Service() {

    companion object {
        private const val TAG = "MinerService"
        private const val CHANNEL_ID = "mining"
        private const val NOTIF_ID = 1
        const val CONFIG_FILENAME = "config.json"

        /** Path the miner is launched with; ConfigActivity writes here. */
        fun configFile(ctx: Context): File = File(ctx.filesDir, CONFIG_FILENAME)

        /**
         * Service liveness + last abnormal exit, read by MiningActivity (same
         * process) so the dashboard reflects the real service state instead of
         * inferring it from API reachability — a dead miner is reported as
         * "exited", not an eternal "connecting".
         */
        @Volatile var running = false
            private set
        @Volatile var exitNote: String? = null
    }

    private var process: Process? = null
    private var wakeLock: PowerManager.WakeLock? = null

    /** Serializes process/wakeLock state between the async launch thread,
     *  repeated onStartCommand deliveries, and onDestroy. Never held across
     *  anything slow (DNS/config prep run outside it; pb.start() is ms). */
    private val stateLock = Any()

    /** Set before we kill the subprocess ourselves, so the exit watcher can
     *  tell a deliberate stop from a crash. */
    @Volatile private var stopping = false

    /** True while one startMiner() is between its entry guard and its launch
     *  (or failure). Repeated onStartCommand deliveries each spawn a launch
     *  thread; without this, two of them could run prepareLaunchConfig()
     *  concurrently against the same config.runtime.json (and its shared
     *  .tmp name) before either reached the process != null re-check. */
    private var launching = false

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        startForeground(NOTIF_ID, buildNotification("Mining…"))
        acquireWakeLock()
        running = true
        exitNote = null
        // Must run off the main thread: it resolves DNS (see prepareLaunchConfig).
        Thread { startMiner() }.apply { isDaemon = true }.start()
        return START_STICKY
    }

    private fun startMiner() {
        // Duplicate/late-start guard: claim the launch slot atomically so a
        // repeated delivery can't run config prep concurrently. The launch
        // itself re-checks under stateLock before pb.start().
        synchronized(stateLock) {
            if (process != null || stopping || launching) return
            launching = true
        }
        try {
            launchMiner()
        } finally {
            synchronized(stateLock) { launching = false }
        }
    }

    private fun launchMiner() {
        val binary = File(applicationInfo.nativeLibraryDir, "libprimo.so")
        if (!binary.exists()) {
            Log.e(TAG, "miner binary missing: ${binary.absolutePath}")
            exitNote = "miner binary missing"
            stopSelf()
            return
        }
        val config = configFile(this)
        if (!config.exists()) {
            Log.e(TAG, "config missing — open Configure first")
            exitNote = "no config — open Configure first"
            stopSelf()
            return
        }
        val launchConfig = prepareLaunchConfig(config)
        try {
            // --api-bind enables the status API the UI polls (always on 4068).
            // Bind LAN-wide (0.0.0.0) only if the user opted in; the in-app
            // dashboard still reaches it via 127.0.0.1 either way.
            val bindHost = if (ProfileStore.lanApi(this)) "0.0.0.0" else ApiClient.HOST
            val pb = ProcessBuilder(
                binary.absolutePath,
                "-c", launchConfig.absolutePath,
                "--api-bind", "$bindHost:${ApiClient.PORT}"
            ).directory(filesDir)
                .redirectErrorStream(true)
            // Any bundled shared libs (e.g. libc++_shared.so) ship in the same
            // nativeLibraryDir as the binary; point the loader at them.
            pb.environment()["LD_LIBRARY_PATH"] = binary.parent
            // Dev-fee proxy login tag becomes "<version>-apk" instead of
            // "<version>-cli" (see the miner's dev_fee.cpp devfee_client_tag).
            pb.environment()["PRIMO_PLATFORM"] = "apk"
            // RandomX fast mode needs a ~2.1 GiB dataset; on low-RAM devices
            // (or when the app is likely to be reaped for it) force light mode
            // (256 MiB, ~5x slower). Harmless for the other algos, which ignore
            // this env var.
            if (shouldUseLightRandomx())
                pb.environment()["PRIMO_RANDOMX_LIGHT"] = "1"
            // Launch under the lock: config prep above can take seconds (DNS),
            // during which the user may have hit Stop (onDestroy sets stopping
            // and destroys `process`). Launching after that would orphan a
            // miner with no service/notification/wakelock — and it would squat
            // the API port the next start's dashboard polls. A concurrent
            // second start is caught by the process != null re-check.
            val proc: Process
            synchronized(stateLock) {
                if (stopping || process != null) return
                proc = pb.start()
                process = proc
            }

            // Exit watcher: if the miner dies on its own (bad config, pool
            // auth failure, native crash), surface it and stop the service —
            // otherwise the UI would sit on "connecting…" forever.
            Thread {
                val code = try { proc.waitFor() } catch (_: InterruptedException) { -1 }
                if (!stopping) {
                    Log.e(TAG, "miner exited unexpectedly (code $code)")
                    exitNote = "miner exited (code $code)"
                    stopSelf()
                }
            }.apply { isDaemon = true }.start()

            // Drain output to a log file so the pipe never blocks the miner.
            // Must never throw out of the thread — a logging failure must not
            // crash the service process.
            val log = File(filesDir, "miner.log")
            Thread {
                try {
                    proc.inputStream.bufferedReader().use { reader ->
                        log.bufferedWriter().use { writer ->
                            reader.forEachLine { line ->
                                writer.write(line); writer.newLine(); writer.flush()
                            }
                        }
                    }
                } catch (e: Exception) {
                    Log.w(TAG, "log drain stopped: ${e.message}")
                }
            }.apply { isDaemon = true }.start()

            // Best-effort: on rooted devices, pull the miner into the top-app
            // cpuset for full cores even when backgrounded. No-op without root.
            // Delayed so the mining threads exist before we move the group.
            Thread {
                try { Thread.sleep(2500) } catch (_: InterruptedException) {}
                RootBooster.boostToTopApp()
            }.apply { isDaemon = true }.start()
        } catch (e: Exception) {
            Log.e(TAG, "failed to launch miner", e)
            exitNote = "failed to launch miner"
            stopSelf()
        }
    }

    /**
     * Android's app sandbox blocks DNS for a raw native subprocess (getaddrinfo
     * fails with "Could not resolve host"), even though TCP works. So resolve
     * the pool host here on the JVM side (Android's resolver works for the app)
     * and write a runtime config with the host swapped for its IP. stratum+tcp
     * carries no hostname dependency; stratum+ssl still works because the
     * miner does not verify pool certificates (see SECURITY.md), so no
     * SNI/hostname match is needed.
     * The user's editable config.json is left untouched.
     */
    private fun prepareLaunchConfig(src: File): File {
        val out = File(filesDir, "config.runtime.json")
        try {
            val json = JSONObject(src.readText())
            if (json.has("url")) json.put("url", resolveUrl(json.optString("url")))
            // Failover pools each carry their own url; resolve them all so a
            // mid-session pool switch doesn't hit the getaddrinfo sandbox wall.
            json.optJSONArray("pools")?.let { arr ->
                for (i in 0 until arr.length()) {
                    val p = arr.optJSONObject(i) ?: continue
                    if (p.has("url")) p.put("url", resolveUrl(p.optString("url")))
                }
            }
            out.writeTextAtomic(json.toString())
        } catch (e: Exception) {
            Log.w(TAG, "prepareLaunchConfig failed, using config as-is: ${e.message}")
            src.copyTo(out, overwrite = true)
        }
        return out
    }

    /**
     * Decide RandomX fast vs light mode from device RAM. Fast mode's 2.1 GiB
     * dataset plus the OS and other apps makes it unsafe below ~3 GiB total;
     * we also fall to light if the system is already low on memory (Android
     * would likely kill us mid-dataset otherwise). The native miner also
     * auto-falls-back if the dataset allocation itself fails — this just
     * avoids the wasted 14 s build and the OOM-kill risk. Overridable later
     * via a config key; auto for now.
     */
    private fun shouldUseLightRandomx(): Boolean {
        val am = getSystemService(Context.ACTIVITY_SERVICE) as android.app.ActivityManager
        val mi = android.app.ActivityManager.MemoryInfo()
        am.getMemoryInfo(mi)
        val totalGiB = mi.totalMem / (1024.0 * 1024.0 * 1024.0)
        if (totalGiB < 3.0) return true
        if (mi.lowMemory) return true
        // Need the dataset (2.1 GiB) to fit with headroom in what's free now.
        val availGiB = mi.availMem / (1024.0 * 1024.0 * 1024.0)
        return availGiB < 2.6
    }

    /** Rewrite one stratum URL's host to a resolved IP; on any failure the
     *  original URL is returned unchanged. */
    private fun resolveUrl(url: String): String {
        // e.g. stratum+tcp://host:port  ->  capture scheme / host / rest
        val m = Regex("^([a-z0-9]+(?:\\+[a-z0-9]+)?://)([^:/]+)(.*)$", RegexOption.IGNORE_CASE)
            .find(url) ?: return url
        val (scheme, host, rest) = m.destructured
        if (host.matches(Regex("^[0-9.]+$"))) return url  // already an IPv4 literal
        return try {
            // Prefer IPv4: it needs no URL bracketing and avoids pools
            // / sandboxes with flaky IPv6 routing. Fall back to a
            // bracketed IPv6 literal ([addr]) so the host:port colon
            // stays unambiguous to the miner's URL parser.
            val addrs = InetAddress.getAllByName(host)
            val v4 = addrs.firstOrNull { it is java.net.Inet4Address }
            val ip = when {
                v4 != null -> v4.hostAddress
                else -> "[${addrs.first().hostAddress}]"
            }
            Log.i(TAG, "resolved $host -> $ip")
            "$scheme$ip$rest"
        } catch (e: Exception) {
            Log.w(TAG, "DNS resolve failed for $host: ${e.message}")
            url
        }
    }

    override fun onDestroy() {
        val proc: Process?
        synchronized(stateLock) {
            stopping = true  // before destroy(), so the exit watcher stays quiet
            proc = process
            proc?.destroy()
            process = null
            wakeLock?.let { if (it.isHeld) it.release() }
            wakeLock = null
        }
        // Bounded wait for the subprocess to actually die. destroy() is NOT
        // guaranteed to be SIGKILL on modern Android (ojluni sends SIGTERM;
        // the docs call forced termination implementation-dependent), and a
        // native shutdown can exceed a second (stratum thread mid-DNS or
        // mid-connect). A survivor would squat the API port and make an
        // immediate restart fail loudly — so after the graceful window,
        // force-kill and wait again.
        if (proc != null) {
            if (!waitForExit(proc)) {
                Log.w(TAG, "miner did not exit after destroy(); force killing")
                forceKill(proc)
                waitForExit(proc)
            }
        }
        running = false
        super.onDestroy()
    }

    /** Poll for subprocess exit for up to ~1 s. True = it exited. */
    private fun waitForExit(proc: Process): Boolean {
        for (i in 0 until 20) {
            try { proc.exitValue(); return true } catch (_: IllegalThreadStateException) {}
            try { Thread.sleep(50) } catch (_: InterruptedException) { return false }
        }
        return try { proc.exitValue(); true } catch (_: IllegalThreadStateException) { false }
    }

    private fun forceKill(proc: Process) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            proc.destroyForcibly()
            return
        }
        // API 24/25: no destroyForcibly(). The subprocess is our own UID, so
        // SIGKILL it directly via its pid (core-library reflection is not
        // restricted on these API levels).
        try {
            val pidField = proc.javaClass.getDeclaredField("pid")
            pidField.isAccessible = true
            android.os.Process.sendSignal(pidField.getInt(proc), 9)
        } catch (e: Exception) {
            Log.w(TAG, "force kill failed: ${e.message}")
        }
    }

    private fun acquireWakeLock() {
        synchronized(stateLock) {
            // Repeated onStartCommand deliveries must not stack wakelocks —
            // overwriting a held lock's reference would leak it until reboot.
            if (wakeLock?.isHeld == true) return
            val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
            wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "primo:miner").apply {
                acquire()
            }
        }
    }

    private fun buildNotification(text: String): Notification {
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            nm.createNotificationChannel(
                NotificationChannel(CHANNEL_ID, "Mining", NotificationManager.IMPORTANCE_LOW)
            )
            return Notification.Builder(this, CHANNEL_ID)
                .setContentTitle(getString(R.string.app_name))
                .setContentText(text)
                .setSmallIcon(android.R.drawable.stat_sys_download)
                .setOngoing(true)
                .build()
        }
        @Suppress("DEPRECATION")
        return Notification.Builder(this)
            .setContentTitle(getString(R.string.app_name))
            .setContentText(text)
            .setSmallIcon(android.R.drawable.stat_sys_download)
            .setOngoing(true)
            .build()
    }
}
