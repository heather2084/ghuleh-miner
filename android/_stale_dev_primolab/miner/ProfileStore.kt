package dev.primolab.miner

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream

/**
 * Temp-file + atomic-rename write, so a process kill or power loss mid-write
 * can never leave a truncated/corrupt file behind (the reader sees either the
 * old content or the new, nothing in between). fsync before the rename so the
 * data actually hits storage before the name flips.
 */
internal fun File.writeTextAtomic(text: String) {
    val tmp = File(parentFile, "$name.tmp")
    FileOutputStream(tmp).use { fos ->
        fos.write(text.toByteArray())
        fos.fd.sync()
    }
    if (!tmp.renameTo(this)) {
        // Same-directory rename in filesDir shouldn't fail; if it somehow
        // does, a direct write beats silently dropping the update.
        tmp.delete()
        writeText(text)
    }
}

/**
 * Per-algorithm config profiles, stored app-side in profiles.json. Lets the user
 * keep separate pools/threads for verus / sha256d / scrypt and switch between
 * them without re-entering anything.
 *
 * Each profile holds an ORDERED LIST of pools: the first is the primary, the
 * rest are failovers in priority order — this maps 1:1 onto the native miner's
 * ccminer-compatible `pools[]` config array (startup picks the first usable
 * pool; failover walks down the list). The native miner is unchanged.
 *
 * On save we flatten the ACTIVE algo's profile into config.json (what
 * MinerService launches with); profiles.json is purely the app's memory of the
 * others.
 *
 * profiles.json shape (v2):
 *   { "active": "verus", "lanApi": false,
 *     "algos": { "verus": { "threads": 4,
 *                           "pools": [ {url,user,pass}, ... ] }, ... } }
 * v1 entries ({url,user,pass,threads} flat per algo) migrate on read.
 */
object ProfileStore {

    // Canonical coin order (site rule): Verus, Monero, LTC+DOGE, Bitcoin.
    // Positions are never persisted — profiles are keyed by algo name.
    val ALGOS = listOf("verus", "randomx", "scrypt", "sha256d")

    /** Dropdown label: the algo name (what config.json uses) + the coin it mines. */
    fun algoLabel(algo: String): String = when (algo) {
        "verus" -> "verus — Verus (VRSC)"
        "randomx" -> "randomx — Monero (XMR)"
        "scrypt" -> "scrypt — Litecoin + Dogecoin"
        "sha256d" -> "sha256d — Bitcoin (BTC)"
        else -> algo
    }

    /** UI cap on pools per algo. The miner itself allows MAX_USER_POOLS=8. */
    const val MAX_POOLS = 4

    data class Pool(
        var url: String = "",
        var user: String = "",
        var pass: String = "x",
    )

    data class Profile(
        var pools: MutableList<Pool> = mutableListOf(Pool()),
        var threads: Int = 4,
    )

    /**
     * In-memory copy of profiles.json. This process is the only writer, so a
     * simple write-through cache is safe; it also keeps the 2s dashboard poll
     * from re-reading the file on the UI thread every tick.
     */
    @Volatile private var cache: JSONObject? = null

    private fun file(ctx: Context) = File(ctx.filesDir, "profiles.json")

    private fun load(ctx: Context): JSONObject {
        cache?.let { return it }
        val f = file(ctx)
        if (f.exists()) {
            runCatching { JSONObject(f.readText()) }.getOrNull()?.let {
                cache = it
                return it
            }
        }
        // Migrate an existing single config.json into its algo's profile.
        val root = JSONObject().put("active", ALGOS[0]).put("algos", JSONObject())
        val legacy = MinerService.configFile(ctx)
        if (legacy.exists()) {
            runCatching {
                val j = JSONObject(legacy.readText())
                val algo = j.optString("algo", ALGOS[0])
                root.put("active", algo)
                root.getJSONObject("algos").put(algo, JSONObject()
                    .put("threads", j.optInt("threads", 4))
                    .put("pools", JSONArray().put(JSONObject()
                        .put("url", j.optString("url"))
                        .put("user", j.optString("user"))
                        .put("pass", j.optString("pass", "x")))))
            }
        }
        cache = root
        return root
    }

    /** Persist [root] and keep the cache coherent. All writes go through here. */
    private fun store(ctx: Context, root: JSONObject) {
        file(ctx).writeTextAtomic(root.toString())
        cache = root
    }

    fun activeAlgo(ctx: Context): String =
        load(ctx).optString("active", ALGOS[0]).let { if (it in ALGOS) it else ALGOS[0] }

    /**
     * App-wide (not per-algo) flag: expose the status API to the LAN. Off = the
     * miner binds 127.0.0.1 (in-app dashboard only); on = 0.0.0.0 (other devices
     * on the network can reach the ccminer-compatible API on port 4068).
     */
    fun lanApi(ctx: Context): Boolean = load(ctx).optBoolean("lanApi", false)

    fun setLanApi(ctx: Context, on: Boolean) {
        store(ctx, load(ctx).put("lanApi", on))
    }

    /**
     * App-wide flag: user ticked "don't show again" on the first-launch
     * disclaimer (what the app does + dev fee + own-risk). Until then the
     * dialog reappears on every dashboard launch.
     */
    fun disclaimerAccepted(ctx: Context): Boolean =
        load(ctx).optBoolean("disclaimerOk", false)

    fun setDisclaimerAccepted(ctx: Context) {
        store(ctx, load(ctx).put("disclaimerOk", true))
    }

    fun profile(ctx: Context, algo: String): Profile {
        val algos = load(ctx).optJSONObject("algos") ?: return Profile()
        val j = algos.optJSONObject(algo) ?: return Profile()
        val pools = mutableListOf<Pool>()
        val arr = j.optJSONArray("pools")
        if (arr != null) {
            for (i in 0 until arr.length()) {
                val p = arr.optJSONObject(i) ?: continue
                pools.add(Pool(
                    url = p.optString("url"),
                    user = p.optString("user"),
                    pass = p.optString("pass", "x"),
                ))
            }
        } else if (j.has("url")) {
            // v1 flat profile — single pool
            pools.add(Pool(
                url = j.optString("url"),
                user = j.optString("user"),
                pass = j.optString("pass", "x"),
            ))
        }
        if (pools.isEmpty()) pools.add(Pool())
        return Profile(pools = pools, threads = j.optInt("threads", 4))
    }

    private fun profileJson(p: Profile): JSONObject {
        val arr = JSONArray()
        for (pool in p.pools.take(MAX_POOLS)) {
            arr.put(JSONObject()
                .put("url", pool.url).put("user", pool.user).put("pass", pool.pass))
        }
        return JSONObject().put("threads", p.threads).put("pools", arr)
    }

    /** Persist one algo's profile (does not change which algo is active). */
    fun saveProfile(ctx: Context, algo: String, p: Profile) {
        val root = load(ctx)
        val algos = root.optJSONObject("algos") ?: JSONObject().also { root.put("algos", it) }
        algos.put(algo, profileJson(p))
        store(ctx, root)
    }

    /** "stratum+tcp://host:port" -> "host", used as the pool's API/log label. */
    private fun hostOf(url: String): String =
        url.substringAfter("://").substringBefore('/').substringBeforeLast(':')
            .trim('[', ']').ifBlank { url }

    /**
     * Mark [algo] active, save its profile, and flatten it into config.json so
     * the miner launches with it. Pools with a blank URL are kept in the profile
     * (the user may still be filling them in) but excluded from config.json.
     * Returns true if at least one pool has a usable URL.
     */
    fun commitActive(ctx: Context, algo: String, p: Profile): Boolean {
        saveProfile(ctx, algo, p)
        store(ctx, load(ctx).put("active", algo))

        val usable = p.pools.take(MAX_POOLS).filter { it.url.isNotBlank() }
        val primary = usable.firstOrNull() ?: Pool()
        val cfg = JSONObject()
            .put("algo", algo)
            .put("threads", p.threads)
            // Top-level user/pass are the miner's inheritance defaults for
            // pools[] entries that omit them (blank failover user = primary's).
            .put("user", primary.user)
            .put("pass", primary.pass.ifBlank { "x" })
        val arr = JSONArray()
        for (pool in usable) {
            val o = JSONObject()
                // name = the hostname; keeps the API/logs readable after
                // MinerService rewrites url to a resolved IP.
                .put("name", hostOf(pool.url))
                .put("url", pool.url)
            if (pool.user.isNotBlank()) o.put("user", pool.user)
            if (pool.pass.isNotBlank()) o.put("pass", pool.pass)
            arr.put(o)
        }
        cfg.put("pools", arr)
        MinerService.configFile(ctx).writeTextAtomic(cfg.toString(2))
        return primary.url.isNotBlank()
    }
}
