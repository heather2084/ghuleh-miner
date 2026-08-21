package dev.primolab.miner

import android.app.Activity
import android.os.Bundle
import android.view.LayoutInflater
import android.view.MenuItem
import android.view.View
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.Spinner
import android.widget.TextView
import android.widget.Toast

/**
 * Page 2 — Config. Sectioned form: MINER (algorithm + threads), POOLS
 * (primary + up to MAX_POOLS-1 failovers as removable cards, in the miner's
 * failover priority order), MONITORING (LAN API toggle).
 *
 * Algorithm is a dropdown; each algo keeps its own pools/threads
 * (ProfileStore), so switching algos repopulates that algo's saved values.
 * SAVE flattens the active algo into config.json for the native miner —
 * which is unchanged and reads the ccminer-compatible pools[] format.
 */
class ConfigActivity : Activity() {

    private lateinit var algoSpinner: Spinner
    private lateinit var threads: EditText
    private lateinit var poolContainer: LinearLayout
    private lateinit var addPool: TextView
    private lateinit var lanApi: CheckBox

    private var currentAlgo = ProfileStore.ALGOS[0]
    private var ready = false
    private var accent = Palette.TEAL

    /** Re-tint the accent-carrying views to the selected algorithm's coin
     *  color, so the page previews the scheme the dashboard will wear. */
    private fun applyAccent(algo: String) {
        accent = Palette.accentFor(algo)
        findViewById<TextView>(R.id.sectionMiner).setTextColor(accent)
        findViewById<TextView>(R.id.sectionPools).setTextColor(accent)
        findViewById<TextView>(R.id.sectionMonitoring).setTextColor(accent)
        addPool.setTextColor(accent)
        findViewById<Button>(R.id.saveButton).backgroundTintList =
            android.content.res.ColorStateList.valueOf(accent)
        for (i in 0 until poolContainer.childCount)
            poolContainer.getChildAt(i)
                .findViewById<TextView>(R.id.poolTitle).setTextColor(accent)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_config)
        actionBar?.setDisplayHomeAsUpEnabled(true)
        algoSpinner = findViewById(R.id.algoSpinner)
        threads = findViewById(R.id.threadsField)
        poolContainer = findViewById(R.id.poolContainer)
        addPool = findViewById(R.id.addPoolButton)
        lanApi = findViewById(R.id.lanApiCheck)
        lanApi.isChecked = ProfileStore.lanApi(this)   // app-wide, not per-algo

        val cores = Runtime.getRuntime().availableProcessors()
        findViewById<TextView>(R.id.threadsHint).text = "This device reports $cores CPU cores."

        val adapter = ArrayAdapter(this, android.R.layout.simple_spinner_item,
            ProfileStore.ALGOS.map { ProfileStore.algoLabel(it) })
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
        algoSpinner.adapter = adapter

        currentAlgo = ProfileStore.activeAlgo(this)
        algoSpinner.setSelection(ProfileStore.ALGOS.indexOf(currentAlgo), false)
        loadFields(currentAlgo)
        applyAccent(currentAlgo)
        ready = true  // ignore the programmatic selection above

        algoSpinner.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(p: AdapterView<*>?, v: View?, pos: Int, id: Long) {
                if (!ready) return
                val selected = ProfileStore.ALGOS[pos]
                if (selected == currentAlgo) return
                // keep edits to the algo we're leaving, then load the new one
                ProfileStore.saveProfile(this@ConfigActivity, currentAlgo, readFields())
                currentAlgo = selected
                loadFields(selected)
                applyAccent(selected)
            }
            override fun onNothingSelected(p: AdapterView<*>?) {}
        }

        addPool.setOnClickListener {
            if (poolContainer.childCount < ProfileStore.MAX_POOLS) {
                addPoolCard(ProfileStore.Pool(pass = ""))
                rebindPoolCards()
            }
        }
        findViewById<Button>(R.id.saveButton).setOnClickListener { save() }

        findViewById<View>(R.id.infoMiner).setOnClickListener {
            showHelp(R.string.help_miner_title, R.string.help_miner, Links.FAQ)
        }
        findViewById<View>(R.id.infoPools).setOnClickListener {
            // Link to the page for the coin being configured (pool + wallet setup).
            showHelp(R.string.help_pools_title, R.string.help_pools, Links.coinPage(currentAlgo))
        }
        findViewById<View>(R.id.infoMonitoring).setOnClickListener {
            showHelp(R.string.help_monitoring_title, R.string.help_monitoring, Links.DOCS)
        }
    }

    /** Section ⓘ popup: what each input means, with a linkback to the site. */
    private fun showHelp(titleRes: Int, bodyRes: Int, url: String) {
        android.app.AlertDialog.Builder(this)
            .setTitle(getString(titleRes))
            .setMessage(getString(bodyRes))
            .setPositiveButton(getString(R.string.help_close), null)
            .setNeutralButton(getString(R.string.help_learn_more)) { _, _ ->
                Links.open(this, url)
            }
            .show()
    }

    override fun onOptionsItemSelected(item: MenuItem): Boolean {
        if (item.itemId == android.R.id.home) { finish(); return true }
        return super.onOptionsItemSelected(item)
    }

    private fun loadFields(algo: String) {
        val p = ProfileStore.profile(this, algo)
        threads.setText(p.threads.toString())
        poolContainer.removeAllViews()
        p.pools.take(ProfileStore.MAX_POOLS).forEach { addPoolCard(it) }
        rebindPoolCards()
    }

    private fun addPoolCard(pool: ProfileStore.Pool) {
        val card = LayoutInflater.from(this).inflate(R.layout.pool_card, poolContainer, false)
        card.findViewById<EditText>(R.id.poolUrl).setText(pool.url)
        card.findViewById<EditText>(R.id.poolUser).setText(pool.user)
        card.findViewById<EditText>(R.id.poolPass).setText(pool.pass)
        card.findViewById<TextView>(R.id.poolRemove).setOnClickListener {
            poolContainer.removeView(card)
            rebindPoolCards()
        }
        card.findViewById<TextView>(R.id.poolTitle).setTextColor(accent)
        poolContainer.addView(card)
    }

    /** Re-title cards after add/remove so numbering stays consecutive; the
     *  primary card can't be removed. Failover fields hint at inheritance. */
    private fun rebindPoolCards() {
        for (i in 0 until poolContainer.childCount) {
            val card = poolContainer.getChildAt(i)
            card.findViewById<TextView>(R.id.poolTitle).text =
                if (i == 0) "PRIMARY POOL" else "FAILOVER $i"
            card.findViewById<TextView>(R.id.poolRemove).visibility =
                if (i == 0) View.GONE else View.VISIBLE
            val hint = if (i == 0) "wallet.worker" else "blank = same as primary"
            card.findViewById<EditText>(R.id.poolUser).hint = hint
            card.findViewById<EditText>(R.id.poolPass).hint =
                if (i == 0) "x" else "blank = same as primary"
        }
        addPool.visibility =
            if (poolContainer.childCount >= ProfileStore.MAX_POOLS) View.GONE else View.VISIBLE
    }

    private fun readFields(): ProfileStore.Profile {
        val pools = mutableListOf<ProfileStore.Pool>()
        for (i in 0 until poolContainer.childCount) {
            val card = poolContainer.getChildAt(i)
            pools.add(ProfileStore.Pool(
                url = card.findViewById<EditText>(R.id.poolUrl).text.toString().trim(),
                user = card.findViewById<EditText>(R.id.poolUser).text.toString().trim(),
                pass = card.findViewById<EditText>(R.id.poolPass).text.toString().trim(),
            ))
        }
        if (pools.isEmpty()) pools.add(ProfileStore.Pool())
        return ProfileStore.Profile(
            pools = pools,
            threads = threads.text.toString().trim().toIntOrNull() ?: 4,
        )
    }

    private fun save() {
        val p = readFields()
        if (p.pools.first().url.isBlank()) {
            Toast.makeText(this, "Primary pool URL is required", Toast.LENGTH_LONG).show()
            return
        }
        // Native miner allows 1..32 (MAX_THREADS); anything else makes it
        // exit on startup, which the dashboard would surface as a crash.
        if (p.threads < 1 || p.threads > 32) {
            Toast.makeText(this, "Threads must be between 1 and 32", Toast.LENGTH_LONG).show()
            return
        }
        ProfileStore.setLanApi(this, lanApi.isChecked)
        ProfileStore.commitActive(this, currentAlgo, p)
        val extra = p.pools.drop(1).count { it.url.isNotBlank() }
        val msg = if (extra > 0) "Saved ($currentAlgo, $extra failover pool${if (extra > 1) "s" else ""})"
                  else "Saved ($currentAlgo)"
        Toast.makeText(this, msg, Toast.LENGTH_SHORT).show()
        finish()
    }
}
