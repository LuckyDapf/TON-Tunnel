package com.example.dapf.tongate.ui

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

/**
 * ViewModel хранит состояние экрана VPN и не зависит от Activity.
 */
class VpnViewModel(
    application: Application,
    private val controller: VpnServiceController
) : AndroidViewModel(application) {

    private val _uiState = MutableStateFlow(VpnUiState())
    val uiState: StateFlow<VpnUiState> = _uiState.asStateFlow()

    init {
        viewModelScope.launch {
            controller.observeStats().collect { stats ->
                _uiState.value = stats
            }
        }
    }

    fun onVpnPermissionGrantedAndStart() {
        controller.startVpn()
        _uiState.update { it.copy(isVpnEnabled = true) }
    }

    fun stopVpn() {
        controller.stopVpn()
        _uiState.update { it.copy(isVpnEnabled = false) }
    }

    class Factory(private val application: Application) : ViewModelProvider.Factory {
        @Suppress("UNCHECKED_CAST")
        override fun <T : androidx.lifecycle.ViewModel> create(modelClass: Class<T>): T {
            val controller = VpnServiceController(application.applicationContext)
            return VpnViewModel(application, controller) as T
        }
    }
}
