package com.example.dapf.tongate.ui

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import androidx.core.content.ContextCompat
import com.example.dapf.tongate.data.vpn.TonShieldVpnService
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.callbackFlow

/**
 * Отдельный контроллер управления VPN-сервисом и чтения его телеметрии.
 * UI получает только готовый поток состояний, без прямой работы с BroadcastReceiver.
 */
class VpnServiceController(private val appContext: Context) {

    fun startVpn() {
        val serviceIntent = Intent(appContext, TonShieldVpnService::class.java).apply {
            action = TonShieldVpnService.ACTION_START_VPN
        }
        ContextCompat.startForegroundService(appContext, serviceIntent)
    }

    fun stopVpn() {
        val serviceIntent = Intent(appContext, TonShieldVpnService::class.java).apply {
            action = TonShieldVpnService.ACTION_STOP_VPN
        }
        ContextCompat.startForegroundService(appContext, serviceIntent)
    }

    fun observeStats(): Flow<VpnUiState> = callbackFlow {
        val statsReceiver = object : BroadcastReceiver() {
            override fun onReceive(context: Context?, intent: Intent?) {
                if (intent?.action != TonShieldVpnService.ACTION_VPN_STATS) return

                val enabled = intent.getBooleanExtra(TonShieldVpnService.EXTRA_VPN_ENABLED, false)
                val blockedCount = intent.getIntExtra(TonShieldVpnService.EXTRA_BLOCKED_COUNT, 0)
                trySend(
                    VpnUiState(
                        isVpnEnabled = enabled,
                        blockedThreats = blockedCount
                    )
                )
            }
        }

        val filter = IntentFilter(TonShieldVpnService.ACTION_VPN_STATS)
        ContextCompat.registerReceiver(
            appContext,
            statsReceiver,
            filter,
            ContextCompat.RECEIVER_NOT_EXPORTED
        )

        awaitClose {
            appContext.unregisterReceiver(statsReceiver)
        }
    }
}
