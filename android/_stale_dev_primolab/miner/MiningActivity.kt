package dev.primolab.miner

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.res.ColorStateList
import android.graphics.Color
import android.net.Uri
import android.os.BatteryManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.provider.Settings
import android.view.Gravity
import android.view.Menu
import android.view.MenuItem
import android.view.View
import android.view.WindowManager
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import kotlin.concurrent.thread

/**
 * Page 1 (home) — mining dashboard. Big hashrate, per-thread chips, a stat grid,
 * and a Start/Stop pill. Config is reached via the cog in the action bar.
 * All data comes from the miner's read-only API (127.0.0.1:4068) + Android battery.
 */
class MiningActivity : Activity() {

    private val handler = Handler(Looper.getMainLooper())
    private val REQ_NOTIF = 1
    private var mining = false
    private var maxKhs = 0.0
    private var startTapTime = 0L

    private lateinit var hashrate: TextView
    private lateinit var algo: TextView
    private lateinit var pool: TextView
    private lateinit var status: TextView
    private lateinit var statusDot: View
    private lateinit var heroCard: View
    private lateinit var threadWrap: LinearLayout
    private lateinit var startStop: Button

    private val IDLE = Color.parseColor("#2A2E3A")
    private val DARK = Color.parseColor("#0B0C10")
    private val WARN = Color.parseColor("#FFB020")
    private val DANGER = Color.parseColor("#FF5566")
    private val DIM = Color.parseColor("#828A9A")
    private val TEXT = Color.parseColor("#F2F4F8")

    /** Live accent = the active algorithm's coin color (Palette). Everything
     *  accent-tinted is re-applied through applyAccent() when it changes. */
    private var accent = Palette.TEAL
    private var accentAlgo: String? = null

    private fun applyAccent(algoName: String?) {
        val normalized = algoName?.trim()?.lowercase()
        if (normalized == accentAlgo) return
        accentAlgo = normalized
        accent = Palette.accentFor(normalized)
        val d = resources.displayMetrics.density
        heroCard.background = Palette.heroCard(accent, d)
        algo.background = Palette.badge(accent, d)
        hashrate.setTextColor(accent)
        setPill(mining)
        if (status.text.toString() == "MINING") setStatus("MINING", accent)
    }

    private val poller = object : Runnable {
        override fun run() {
            thread {
                val s = ApiClient.summary()
                val t = ApiClient.threads()
                val hw = ApiClient.hwinfo()
                // "pool" tells us the LIVE pool — matters with failover, where
                // the miner may have switched away from the primary.
                val p = ApiClient.query("pool")
                runOnUiThread { render(s, t, hw, p) }
            }
            handler.postDelayed(this, 2000)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_mining)
        hashrate = findViewById(R.id.hashrateText)
        algo = findViewById(R.id.algoText)
        pool = findViewById(R.id.poolText)
        status = findViewById(R.id.statusText)
        statusDot = findViewById(R.id.statusDot)
        heroCard = findViewById(R.id.heroCard)
        threadWrap = findViewById(R.id.threadWrap)
        startStop = findViewById(R.id.startStopButton)
        applyAccent(ProfileStore.activeAlgo(this))
        setPill(false)
        startStop.setOnClickListener { if (mining) stopMining() else startMining() }
        if (!ProfileStore.disclaimerAccepted(this)) showDisclaimer()
    }

    /**
     * First-launch disclosure: what the app is (a CPU miner), the dev fee, and
     * an own-risk/no-warranty statement. Not dismissible except through the
     * buttons; "Don't show again" persists via ProfileStore so it reappears on
     * every launch until the user opts out. EXIT closes the app.
     */
    private fun showDisclaimer() {
        val dp = resources.displayMetrics.density
        val pad = (20 * dp).toInt()
        val body = TextView(this).apply {
            text = getString(R.string.disclaimer_text)
            textSize = 14f
            setLineSpacing(3 * dp, 1f)
        }
        val dontShow = android.widget.CheckBox(this).apply {
            text = getString(R.string.disclaimer_dont_show)
            isChecked = true
        }
        val box = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, (10 * dp).toInt(), pad, 0)
            addView(body)
            addView(dontShow, LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT).apply {
                    topMargin = (10 * dp).toInt()
                })
        }
        val scroll = android.widget.ScrollView(this).apply { addView(box) }
        android.app.AlertDialog.Builder(this)
            .setTitle(getString(R.string.disclaimer_title))
            .setView(scroll)
            .setCancelable(false)
            .setPositiveButton(getString(R.string.disclaimer_accept)) { d, _ ->
                if (dontShow.isChecked) ProfileStore.setDisclaimerAccepted(this)
                d.dismiss()
            }
            .setNegativeButton(getString(R.string.disclaimer_exit)) { _, _ ->
                finishAffinity()
            }
            .show()
    }

    override fun onCreateOptionsMenu(menu: Menu): Boolean {
        menuInflater.inflate(R.menu.mining_menu, menu)
        return true
    }

    override fun onOptionsItemSelected(item: MenuItem): Boolean {
        when (item.itemId) {
            R.id.action_config -> {
                startActivity(Intent(this, ConfigActivity::class.java)); return true
            }
            R.id.action_logs -> {
                startActivity(Intent(this, LogActivity::class.java)); return true
            }
            R.id.action_about -> { showAbout(); return true }
        }
        return super.onOptionsItemSelected(item)
    }

    /** Overflow → About: version + a linkback to the site (guides, FAQ, updates). */
    private fun showAbout() {
        // The non-deprecated PackageInfoFlags overload needs API 33; minSdk is 24.
        @Suppress("DEPRECATION")
        val version = try {
            packageManager.getPackageInfo(packageName, 0).versionName ?: "?"
        } catch (e: Exception) { "?" }
        android.app.AlertDialog.Builder(this)
            .setTitle(getString(R.string.app_name))
            .setMessage(getString(R.string.about_text, version))
            .setPositiveButton(getString(R.string.help_close), null)
            .setNeutralButton(getString(R.string.about_website)) { _, _ ->
                Links.open(this, Links.SITE)
            }
            .show()
    }

    override fun onResume() {
        super.onResume()
        // Algo may have changed on the config page while we were paused.
        applyAccent(ProfileStore.activeAlgo(this))
        handler.post(poller)
    }
    override fun onPause() { super.onPause(); handler.removeCallbacks(poller) }

    private fun startMining() {
        if (!MinerService.configFile(this).exists()) {
            setStatus("NO CONFIG — TAP THE COG", WARN)
            return
        }
        // Android 13+ (targetSdk 33): the manifest POST_NOTIFICATIONS entry
        // alone doesn't prompt — without the runtime grant the foreground
        // "Mining…" notification is silently invisible. Ask BEFORE the service
        // starts (it posts the notification once, at startForeground, so a
        // grant arriving later wouldn't show it), then launch either way from
        // the callback — the notification is status-only, never a gate. After
        // a hard "don't ask again" denial the callback fires immediately.
        if (Build.VERSION.SDK_INT >= 33 &&
            checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS) !=
                android.content.pm.PackageManager.PERMISSION_GRANTED) {
            requestPermissions(
                arrayOf(android.Manifest.permission.POST_NOTIFICATIONS), REQ_NOTIF)
            return
        }
        launchMinerService()
    }

    override fun onRequestPermissionsResult(
        requestCode: Int, permissions: Array<out String>, grantResults: IntArray,
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQ_NOTIF) launchMinerService()   // granted or not
    }

    private fun launchMinerService() {
        requestIgnoreBatteryOptimizations()
        val intent = Intent(this, MinerService::class.java)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) startForegroundService(intent)
        else startService(intent)
        mining = true
        maxKhs = 0.0
        startTapTime = System.currentTimeMillis()
        setPill(true)
        // Focused, on-screen app = top-app = all cores + uclamp boost (no root).
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
    }

    private fun stopMining() {
        stopService(Intent(this, MinerService::class.java))
        mining = false
        setPill(false)
        setStatus(getString(R.string.status_idle), DIM)
        threadWrap.removeAllViews()
        threadWrap.visibility = View.GONE
        window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
    }

    private fun setPill(active: Boolean) {
        startStop.text = if (active) getString(R.string.stop) else getString(R.string.start)
        startStop.backgroundTintList =
            ColorStateList.valueOf(if (active) DANGER else accent)
    }

    private fun render(
        s: Map<String, String>?, t: List<Map<String, String>>?,
        hw: Map<String, String>?, livePool: Map<String, String>?,
    ) {
        renderBattery()
        // Trust the SERVICE state, not API reachability: a crashed miner must
        // read as "exited", and one last successful poll right after Stop must
        // not flip the pill back to mining. Grace window: startForegroundService
        // is async, so briefly keep "starting" before the service reports in.
        val starting = mining && !MinerService.running &&
            System.currentTimeMillis() - startTapTime < 5000
        if (starting) { setStatus("STARTING…", WARN); return }
        if (!MinerService.running) {
            mining = false
            setPill(false)
            window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            hashrate.text = "0.00 MH/s"
            // Show the configured algo/pool so the hero isn't blank while idle.
            val cfgAlgo = ProfileStore.activeAlgo(this)
            applyAccent(cfgAlgo)
            algo.text = cfgAlgo.uppercase()
            pool.text = ProfileStore.profile(this, cfgAlgo).pools.first()
                .url.substringAfter("://").ifBlank { "—" }
            val note = MinerService.exitNote
            if (note != null) setStatus("STOPPED: ${note.uppercase()}", DANGER)
            else setStatus(getString(R.string.status_idle), DIM)
            threadWrap.removeAllViews()
            threadWrap.visibility = View.GONE
            return
        }
        mining = true; setPill(true)
        if (s == null) {
            hashrate.text = "0.00 MH/s"
            val cfgAlgo = ProfileStore.activeAlgo(this)
            applyAccent(cfgAlgo)
            algo.text = cfgAlgo.uppercase()
            pool.text = ProfileStore.profile(this, cfgAlgo).pools.first()
                .url.substringAfter("://").ifBlank { "—" }
            setStatus("CONNECTING…", WARN)
            return
        }

        val khs = s["KHS"]?.toDoubleOrNull() ?: 0.0
        if (khs > maxKhs) maxKhs = khs
        hashrate.text = fmtRate(khs)
        applyAccent(s["ALGO"])
        algo.text = s["ALGO"]?.uppercase() ?: "—"
        // Live pool from the API (name = hostname written by ProfileStore);
        // fall back to the configured primary if the pool query failed.
        pool.text = livePool?.get("POOL")?.ifBlank { null }
            ?: livePool?.get("URL")?.substringAfter("://")?.ifBlank { null }
            ?: ProfileStore.profile(this, ProfileStore.activeAlgo(this)).pools.first()
                .url.substringAfter("://").ifBlank { "—" }
        if (khs > 0.0) setStatus("MINING", accent) else setStatus("CONNECTING…", WARN)

        setVal(R.id.valAccepted, s["ACC"] ?: "0")
        val rej = s["REJ"]?.toIntOrNull() ?: 0
        setValColored(R.id.valRejected, (s["REJ"] ?: "0"), if (rej > 0) DANGER else TEXT)
        setVal(R.id.valDiff, fmtDiff(s["DIFF"]))
        setVal(R.id.valUptime, fmtUptime(s["UPTIME"]?.toDoubleOrNull() ?: 0.0))
        setVal(R.id.valMaxhash, fmtRate(maxKhs))
        renderTemp(hw?.get("CPUTEMP")?.toIntOrNull() ?: 0)

        renderThreads(t)
    }

    /** Status pill: set label + tint both the text and the leading dot. */
    private fun setStatus(label: String, color: Int) {
        status.text = label
        status.setTextColor(color)
        statusDot.backgroundTintList = ColorStateList.valueOf(color)
    }

    private fun tempColor(t: Int): Int = when {
        t <= 0 -> TEXT
        t >= 83 -> DANGER
        t >= 72 -> WARN
        else -> TEXT
    }

    /**
     * Temperature tile. Prefer the native miner's real °C (read from sysfs).
     * Where that's blocked without root (stock Samsung SELinux, etc.), fall back
     * to Android's framework thermal signal — these need NO root and NO
     * permission, and work where sysfs is sealed off:
     *   - getThermalHeadroom (API 30+): a 0..1 value toward the throttle point,
     *     continuous, so it tracks heating; shown as a "thermal load" %. (NaN on
     *     devices that don't implement it — e.g. Exynos — then we drop down.)
     *   - battery temperature (BatteryManager.EXTRA_TEMPERATURE): a real °C, on
     *     EVERY device with no root/perm. Not the CPU, but it tracks sustained
     *     load — the universal fallback for stock Exynos Samsung where both the
     *     sysfs read and thermal headroom fail. Labelled "BATT TEMP" to be honest.
     *   - currentThermalStatus (API 29+): coarse NONE/LIGHT/.../SHUTDOWN enum
     *     (only trips near the device's own throttle limit) — last-resort label.
     * True CPU °C from the framework needs device-owner privilege, so a sideloaded
     * app cannot get it; the native sysfs read is the only source of real CPU °C.
     */
    private fun renderTemp(nativeTemp: Int) {
        if (nativeTemp > 0) {
            setLbl(R.id.lblTemp, "TEMPERATURE")
            setValColored(R.id.valTemp, "$nativeTemp °C", tempColor(nativeTemp))
            return
        }
        val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
        if (Build.VERSION.SDK_INT >= 30) {
            val h = try { pm.getThermalHeadroom(0) } catch (e: Exception) { Float.NaN }
            if (h.isFinite() && h > 0f) {
                setLbl(R.id.lblTemp, "THERMAL LOAD")
                setValColored(R.id.valTemp, "${(h * 100).toInt().coerceIn(0, 200)}%",
                    if (h >= 0.9f) DANGER else if (h >= 0.75f) WARN else TEXT)
                return
            }
        }
        val batt = batteryTempC()
        if (batt != null) {
            setLbl(R.id.lblTemp, "BATT TEMP")
            setValColored(R.id.valTemp, "$batt °C",
                if (batt >= 45) DANGER else if (batt >= 40) WARN else TEXT)
            return
        }
        if (Build.VERSION.SDK_INT >= 29) {
            val (word, color) = thermalStatusWord(pm.currentThermalStatus)
            setLbl(R.id.lblTemp, "THERMAL STATE")
            setValColored(R.id.valTemp, word, color)
            return
        }
        setLbl(R.id.lblTemp, "TEMPERATURE")
        setValColored(R.id.valTemp, "—", TEXT)
    }

    /** Battery temperature in whole °C (EXTRA_TEMPERATURE is tenths). No perm. */
    private fun batteryTempC(): Int? {
        val bi = registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        val t = bi?.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, Int.MIN_VALUE) ?: Int.MIN_VALUE
        return if (t > 0) t / 10 else null
    }

    private fun thermalStatusWord(s: Int): Pair<String, Int> = when (s) {
        PowerManager.THERMAL_STATUS_LIGHT -> "Light" to TEXT
        PowerManager.THERMAL_STATUS_MODERATE -> "Moderate" to WARN
        PowerManager.THERMAL_STATUS_SEVERE -> "Severe" to WARN
        PowerManager.THERMAL_STATUS_CRITICAL -> "Critical" to DANGER
        PowerManager.THERMAL_STATUS_EMERGENCY -> "Emergency" to DANGER
        PowerManager.THERMAL_STATUS_SHUTDOWN -> "Shutdown" to DANGER
        else -> "Nominal" to TEXT   // THERMAL_STATUS_NONE
    }

    private fun setLbl(id: Int, v: String) { findViewById<TextView>(id).text = v }

    private fun renderThreads(list: List<Map<String, String>>?) {
        threadWrap.removeAllViews()
        if (list.isNullOrEmpty()) { threadWrap.visibility = View.GONE; return }
        threadWrap.visibility = View.VISIBLE
        val dp = resources.displayMetrics.density
        val sorted = list.sortedBy { it["GPU"]?.toIntOrNull() ?: 0 }

        // Wrap onto centered rows so 8/10/12+ threads never scroll off-screen.
        // Balance rows evenly (12 -> 6+6, not 8+4) for a tidy grid.
        val chipFootprint = 40f * dp                       // chip min-width + margin
        val avail = resources.displayMetrics.widthPixels - (32f * dp)
        val maxPerRow = maxOf(1, (avail / chipFootprint).toInt())
        val rows = (sorted.size + maxPerRow - 1) / maxPerRow
        val perRow = (sorted.size + rows - 1) / rows

        var row: LinearLayout? = null
        sorted.forEachIndexed { i, th ->
            if (i % perRow == 0) {
                row = LinearLayout(this).apply {
                    orientation = LinearLayout.HORIZONTAL
                    gravity = Gravity.CENTER
                }
                val rlp = LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT)
                rlp.topMargin = if (i == 0) 0 else (6 * dp).toInt()
                threadWrap.addView(row, rlp)
            }
            val active = (th["KHS"]?.toDoubleOrNull() ?: 0.0) > 0.0
            val chip = TextView(this).apply {
                text = th["GPU"] ?: "?"
                textSize = 13f
                setTypeface(typeface, android.graphics.Typeface.BOLD)
                gravity = Gravity.CENTER
                minWidth = (34 * dp).toInt()             // uniform width across single digits
                setTextColor(if (active) DARK else DIM)
                background = resources.getDrawable(R.drawable.thread_chip, theme)
                backgroundTintList = ColorStateList.valueOf(if (active) accent else IDLE)
                val padH = (6 * dp).toInt(); val padV = (7 * dp).toInt()
                setPadding(padH, padV, padH, padV)
            }
            val lp = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT)
            lp.marginEnd = (5 * dp).toInt()
            row!!.addView(chip, lp)
        }
    }

    private fun renderBattery() {
        val bi = registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        val level = bi?.getIntExtra(BatteryManager.EXTRA_LEVEL, -1) ?: -1
        val scale = bi?.getIntExtra(BatteryManager.EXTRA_SCALE, -1) ?: -1
        val pct = if (level >= 0 && scale > 0) level * 100 / scale else -1
        setVal(R.id.valBattery, if (pct >= 0) "$pct %" else "—")
        setVal(R.id.valCharge, when (bi?.getIntExtra(BatteryManager.EXTRA_STATUS, -1)) {
            BatteryManager.BATTERY_STATUS_CHARGING -> "Charging"
            BatteryManager.BATTERY_STATUS_FULL -> "Full"
            else -> "Discharging"
        })
    }

    private fun setVal(id: Int, v: String) { findViewById<TextView>(id).text = v }

    private fun setValColored(id: Int, v: String, color: Int) {
        findViewById<TextView>(id).apply { text = v; setTextColor(color) }
    }

    // API hashrates are in kH/s. RandomX runs at hundreds of H/s (kHS < 1),
    // so show raw H/s below 1 kH/s instead of "0.70 kH/s".
    private fun fmtRate(khs: Double): String = when {
        khs >= 1000 -> "%.2f MH/s".format(khs / 1000.0)
        khs >= 1.0  -> "%.2f kH/s".format(khs)
        else        -> "%.0f H/s".format(khs * 1000.0)
    }

    private fun fmtDiff(d: String?): String {
        val v = d?.toDoubleOrNull() ?: return "—"
        return if (v >= 100) v.toLong().toString() else "%.3f".format(v)
    }

    private fun fmtUptime(sec: Double): String = when {
        sec < 60 -> "${sec.toInt()}s"
        sec < 3600 -> "${(sec / 60).toInt()}m"
        else -> "%.1fh".format(sec / 3600.0)
    }

    private fun requestIgnoreBatteryOptimizations() {
        val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
        if (pm.isIgnoringBatteryOptimizations(packageName)) return
        try {
            startActivity(Intent(
                Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS,
                Uri.parse("package:$packageName")))
        } catch (_: Exception) {}
    }
}
