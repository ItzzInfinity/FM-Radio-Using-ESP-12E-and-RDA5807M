package com.itzzinfinity.fmradio.ui

import android.app.Application
import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.itzzinfinity.fmradio.data.RadioApi
import com.itzzinfinity.fmradio.data.RadioDiscovery
import com.itzzinfinity.fmradio.data.RadioStatus
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import java.net.SocketTimeoutException

data class Diagnostics(
    val lastRttMs: Long? = null,
    val okCount: Int = 0,
    val failCount: Int = 0,
    val consecutiveFails: Int = 0,
    val lastError: String? = null,
    val bindToWifi: Boolean = true,
    val wifiAvailable: Boolean = false,
)

data class UiState(
    val hostInput: String = "",
    val connectedUrl: String? = null,
    val connecting: Boolean = false,
    val connectError: String? = null,
    val status: RadioStatus? = null,
    val busy: String? = null,          // non-null while a slow command runs
    val message: String? = null,       // one-line result of the last command
    val volumeDraft: Int? = null,      // slider value while the user drags
    val diag: Diagnostics = Diagnostics(),
) {
    /** Three missed polls in a row: show "not responding" but keep trying. */
    val linkLost get() = connectedUrl != null && diag.consecutiveFails >= 3
}

class RadioViewModel(app: Application) : AndroidViewModel(app) {

    private val prefs = app.getSharedPreferences("radio", Context.MODE_PRIVATE)
    private val cm = app.getSystemService(ConnectivityManager::class.java)
    val discovery = RadioDiscovery(app)

    private val _ui = MutableStateFlow(UiState(hostInput = prefs.getString(KEY_HOST, "") ?: ""))
    val ui: StateFlow<UiState> = _ui.asStateFlow()

    // The firmware handles one request at a time, so polls and commands never overlap.
    private val wire = Mutex()
    private var api: RadioApi? = null
    private var pollJob: Job? = null
    private var volumeJob: Job? = null
    private var draftClearJob: Job? = null

    // Tracks the Wi-Fi network so requests can be bound to it explicitly. This is
    // the mechanism the AP-mode app depends on (no-internet AP vs. mobile data),
    // so the feasibility build exercises it on the home LAN too.
    @Volatile private var wifiNetwork: Network? = null
    private val wifiCallback = object : ConnectivityManager.NetworkCallback() {
        override fun onAvailable(network: Network) {
            wifiNetwork = network
            _ui.update { it.copy(diag = it.diag.copy(wifiAvailable = true)) }
            rebuildApi()
        }
        override fun onLost(network: Network) {
            if (wifiNetwork == network) wifiNetwork = null
            _ui.update { it.copy(diag = it.diag.copy(wifiAvailable = false)) }
            rebuildApi()
        }
    }

    init {
        cm.registerNetworkCallback(
            NetworkRequest.Builder().addTransportType(NetworkCapabilities.TRANSPORT_WIFI).build(),
            wifiCallback,
        )
    }

    override fun onCleared() {
        runCatching { cm.unregisterNetworkCallback(wifiCallback) }
        discovery.stop()
    }

    // --- connect ----------------------------------------------------------------

    fun onHostInput(s: String) = _ui.update { it.copy(hostInput = s, connectError = null) }

    fun connect(input: String = _ui.value.hostInput) {
        val url = normalise(input) ?: run {
            _ui.update { it.copy(connectError = "Enter an IP address or host") }
            return
        }
        _ui.update { it.copy(hostInput = input, connecting = true, connectError = null) }
        viewModelScope.launch {
            val candidate = makeApi(url)
            try {
                val r = wire.withLock { candidate.status() }
                api = candidate
                prefs.edit().putString(KEY_HOST, input.trim()).apply()
                discovery.stop()
                _ui.update {
                    it.copy(
                        connecting = false, connectedUrl = url, status = r.status,
                        diag = it.diag.copy(lastRttMs = r.millis, okCount = it.diag.okCount + 1,
                            consecutiveFails = 0, lastError = null),
                    )
                }
                startPolling()
            } catch (e: Exception) {
                _ui.update { it.copy(connecting = false, connectError = describe(e)) }
            }
        }
    }

    fun disconnect() {
        stopPolling()
        api = null
        _ui.update { it.copy(connectedUrl = null, status = null, busy = null, message = null) }
    }

    fun setBindToWifi(on: Boolean) {
        _ui.update { it.copy(diag = it.diag.copy(bindToWifi = on)) }
        rebuildApi()
    }

    private fun rebuildApi() {
        val url = _ui.value.connectedUrl ?: return
        api = makeApi(url)
    }

    private fun makeApi(url: String) =
        RadioApi(url, if (_ui.value.diag.bindToWifi) wifiNetwork else null)

    // --- polling ------------------------------------------------------------------

    /** Called from the screen's lifecycle: poll only while visible. */
    fun startPolling() {
        if (pollJob?.isActive == true || _ui.value.connectedUrl == null) return
        pollJob = viewModelScope.launch {
            while (isActive) {
                // Skip a beat rather than queue behind a slow seek.
                if (!wire.isLocked) {
                    runCatching { wire.withLock { api?.status() } }
                        .onSuccess { r -> if (r != null) record(r) }
                        .onFailure { e -> recordFailure(e) }
                }
                // Back off while the radio is not answering.
                delay(if (_ui.value.linkLost) 3000 else POLL_MS)
            }
        }
    }

    fun stopPolling() {
        pollJob?.cancel()
        pollJob = null
    }

    private fun record(r: RadioApi.Timed) = _ui.update {
        it.copy(
            status = r.status,
            diag = it.diag.copy(lastRttMs = r.millis, okCount = it.diag.okCount + 1,
                consecutiveFails = 0),
        )
    }

    private fun recordFailure(e: Throwable) = _ui.update {
        it.copy(diag = it.diag.copy(failCount = it.diag.failCount + 1,
            consecutiveFails = it.diag.consecutiveFails + 1, lastError = describe(e)))
    }

    // --- commands -----------------------------------------------------------------

    fun selectPreset(i: Int) = command { preset(i) }
    fun tune(mhz: String) {
        val f = mhz.replace(',', '.').toDoubleOrNull()?.let { Math.round(it * 100).toInt() }
        if (f == null || f !in 8700..10800) {
            _ui.update { it.copy(message = "Frequency must be between 87.0 and 108.0") }
            return
        }
        command { tune(f) }
    }
    fun seek(up: Boolean) = command(busyLabel = "Searching for a station…") { seek(up) }
    fun setOled(type: String) = command { oled(type) }

    fun savePreset(i: Int, mhz: String, name: String) {
        val f = mhz.replace(',', '.').toDoubleOrNull()?.let { Math.round(it * 100).toInt() }
        if (f == null || f !in 8700..10800) {
            _ui.update { it.copy(message = "Frequency must be between 87.0 and 108.0") }
            return
        }
        command { setPreset(i, f, name.take(12)) }
    }

    /** Slider moved: show it immediately, send after a short pause. */
    fun onVolumeDrag(v: Int) {
        _ui.update { it.copy(volumeDraft = v) }
        volumeJob?.cancel()
        volumeJob = viewModelScope.launch {
            delay(150)
            sendVolume(v)
        }
    }

    fun onVolumeDragEnd() {
        val v = _ui.value.volumeDraft ?: return
        volumeJob?.cancel()
        viewModelScope.launch { sendVolume(v) }
    }

    private suspend fun sendVolume(v: Int) {
        runCommand(null) { volume(v) }
        // Hand the slider back to the ESP's value once the user has let go.
        draftClearJob?.cancel()
        draftClearJob = viewModelScope.launch {
            delay(1500)
            _ui.update { it.copy(volumeDraft = null) }
        }
    }

    fun clearMessage() = _ui.update { it.copy(message = null) }

    private fun command(busyLabel: String? = null, call: suspend RadioApi.() -> RadioApi.Timed) {
        if (_ui.value.busy != null) return
        viewModelScope.launch { runCommand(busyLabel, call) }
    }

    private suspend fun runCommand(busyLabel: String?, call: suspend RadioApi.() -> RadioApi.Timed) {
        val a = api ?: return
        _ui.update { it.copy(busy = busyLabel, message = null) }
        try {
            record(wire.withLock { a.call() })
        } catch (e: SocketTimeoutException) {
            // A timeout doesn't prove the command failed: read back, never resend.
            recordFailure(e)
            _ui.update { it.copy(message = "No reply in time — refreshed the radio's state") }
            runCatching { wire.withLock { a.status() } }.onSuccess { record(it) }
        } catch (e: Exception) {
            recordFailure(e)
            _ui.update { it.copy(message = describe(e)) }
        } finally {
            _ui.update { it.copy(busy = null) }
        }
    }

    // --- helpers ------------------------------------------------------------------

    private fun normalise(input: String): String? {
        val s = input.trim().trimEnd('/')
        if (s.isEmpty()) return null
        return if (s.startsWith("http://") || s.startsWith("https://")) s else "http://$s"
    }

    private fun describe(e: Throwable): String = when (e) {
        is SocketTimeoutException -> "Timed out — is the phone on the same Wi-Fi as the radio?"
        is java.net.UnknownHostException ->
            "Unknown host. Android apps can't resolve .local names — use Scan or the IP."
        is java.net.ConnectException -> "Connection refused or unreachable"
        is IllegalArgumentException -> e.message ?: "Unexpected response"
        is org.json.JSONException -> "Response wasn't valid JSON (status buffer overflow?)"
        else -> e.message ?: e.javaClass.simpleName
    }

    companion object {
        private const val KEY_HOST = "last_host"
        private const val POLL_MS = 1000L
    }
}
