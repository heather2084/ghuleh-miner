package dev.ghuleh.miner

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.os.Build

/**
 * A narrow, adb-triggerable "restart mining" switch.
 *
 * MinerService itself is exported="false" (only this app may start it) --
 * that's intentional, so no other app on the phone can start/stop mining.
 * But that also blocks `adb shell am start-foreground-service` from
 * reaching it directly (adb runs as a different identity than the app).
 *
 * This receiver is exported and responds to exactly one action, doing
 * nothing but what the in-app Start Mining button already does: launch
 * MinerService, which reads whatever config (algorithm/pool/wallet/worker)
 * is already saved on this phone. It does not accept or need any config
 * data itself -- same as the button, it relies entirely on the persisted
 * config already on-device. Used by push_to_devices.ps1 to resume mining
 * fleet-wide after an APK update kills the running service.
 */
class MiningControlReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action != ACTION_START_MINING) return
        val svcIntent = Intent(context, MinerService::class.java)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            context.startForegroundService(svcIntent)
        } else {
            context.startService(svcIntent)
        }
    }

    companion object {
        const val ACTION_START_MINING = "dev.ghuleh.miner.action.START_MINING"
    }
}
