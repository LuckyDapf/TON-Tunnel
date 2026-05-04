package com.example.dapf.tongate.ui

import android.app.Activity
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.graphics.Color
import android.net.VpnService
import android.os.Bundle
import android.view.View
import android.widget.FrameLayout
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import com.example.dapf.tongate.R
import com.example.dapf.tongate.data.vpn.TonShieldVpnService
import com.google.android.material.button.MaterialButton
import com.google.android.material.snackbar.Snackbar
import kotlinx.coroutines.launch

class MainActivity : AppCompatActivity() {

    private lateinit var vpnViewModel: VpnViewModel

    private lateinit var shieldIndicator: FrameLayout
    private lateinit var vpnStateText: TextView
    private lateinit var blockedCounterText: TextView
    private lateinit var toggleVpnButton: MaterialButton
    private lateinit var rootView: View
    private var isErrorReceiverRegistered = false

    private val vpnErrorReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            if (intent?.action != TonShieldVpnService.ACTION_VPN_ERROR) return
            val errorText = intent.getStringExtra(TonShieldVpnService.EXTRA_ERROR_MESSAGE)
                ?: "Неизвестная ошибка VPN"
            Snackbar.make(rootView, errorText, Snackbar.LENGTH_LONG)
                .setBackgroundTint(Color.parseColor("#B00020"))
                .setTextColor(Color.WHITE)
                .show()
        }
    }

    private val vpnPermissionLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { result ->
        if (result.resultCode == Activity.RESULT_OK) {
            vpnViewModel.onVpnPermissionGrantedAndStart()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        vpnViewModel = ViewModelProvider(
            this,
            VpnViewModel.Factory(application)
        )[VpnViewModel::class.java]

        bindViews()
        bindActions()
        observeState()
    }

    private fun bindViews() {
        rootView = findViewById(android.R.id.content)
        shieldIndicator = findViewById(R.id.shieldIndicator)
        vpnStateText = findViewById(R.id.vpnStateText)
        blockedCounterText = findViewById(R.id.blockedCounterText)
        toggleVpnButton = findViewById(R.id.toggleVpnButton)
    }

    override fun onStart() {
        super.onStart()
        registerVpnErrorReceiver()
    }

    override fun onStop() {
        unregisterVpnErrorReceiver()
        super.onStop()
    }

    private fun bindActions() {
        toggleVpnButton.setOnClickListener {
            val state = vpnViewModel.uiState.value
            if (state.isVpnEnabled) {
                vpnViewModel.stopVpn()
            } else {
                requestVpnPermissionAndStart()
            }
        }
    }

    private fun observeState() {
        lifecycleScope.launch {
            repeatOnLifecycle(androidx.lifecycle.Lifecycle.State.STARTED) {
                vpnViewModel.uiState.collect(::render)
            }
        }
    }

    /**
     * Рендер экрана отделен от логики запуска/остановки VPN.
     */
    private fun render(state: VpnUiState) {
        if (state.isVpnEnabled) {
            shieldIndicator.setBackgroundResource(R.drawable.shield_indicator_on)
            vpnStateText.text = getString(R.string.vpn_enabled)
            toggleVpnButton.text = getString(R.string.stop_vpn)
        } else {
            shieldIndicator.setBackgroundResource(R.drawable.shield_indicator_off)
            vpnStateText.text = getString(R.string.vpn_disabled)
            toggleVpnButton.text = getString(R.string.start_vpn)
        }

        blockedCounterText.text = getString(R.string.blocked_threats_template, state.blockedThreats)
    }

    private fun requestVpnPermissionAndStart() {
        val prepareIntent = VpnService.prepare(applicationContext)
        if (prepareIntent == null) {
            vpnViewModel.onVpnPermissionGrantedAndStart()
            return
        }
        vpnPermissionLauncher.launch(Intent(prepareIntent))
    }

    private fun registerVpnErrorReceiver() {
        if (isErrorReceiverRegistered) return

        val filter = IntentFilter(TonShieldVpnService.ACTION_VPN_ERROR)
        ContextCompat.registerReceiver(
            this,
            vpnErrorReceiver,
            filter,
            ContextCompat.RECEIVER_NOT_EXPORTED
        )
        isErrorReceiverRegistered = true
    }

    private fun unregisterVpnErrorReceiver() {
        if (!isErrorReceiverRegistered) return
        unregisterReceiver(vpnErrorReceiver)
        isErrorReceiverRegistered = false
    }
}
