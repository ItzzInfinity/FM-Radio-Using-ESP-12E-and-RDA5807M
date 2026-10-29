package com.itzzinfinity.fmradio.data

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.net.wifi.WifiManager
import android.util.Log
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * Finds the radio via the "_http._tcp" service the firmware advertises with
 * MDNS.addService("http", "tcp", 80). Android cannot resolve "radio.local"
 * hostnames directly, so DNS-SD is the only mDNS route available to an app.
 *
 * NsdManager only allows one resolve at a time (FAILURE_ALREADY_ACTIVE), so
 * found services are queued and resolved one by one.
 */
class RadioDiscovery(context: Context) {

    data class Found(val name: String, val host: String, val port: Int) {
        val baseUrl get() = "http://$host" + if (port == 80) "" else ":$port"
    }

    private val nsd = context.getSystemService(NsdManager::class.java)
    private val wifi = context.applicationContext.getSystemService(WifiManager::class.java)
    private var lock: WifiManager.MulticastLock? = null

    private val _found = MutableStateFlow<List<Found>>(emptyList())
    val found: StateFlow<List<Found>> = _found.asStateFlow()

    private val _running = MutableStateFlow(false)
    val running: StateFlow<Boolean> = _running.asStateFlow()

    private val pending = ArrayDeque<NsdServiceInfo>()
    private var resolving = false
    private var listener: NsdManager.DiscoveryListener? = null

    fun start() {
        if (listener != null) return
        _found.value = emptyList()
        lock = wifi.createMulticastLock("fmradio-nsd").apply { setReferenceCounted(false); acquire() }

        val l = object : NsdManager.DiscoveryListener {
            override fun onDiscoveryStarted(serviceType: String) { _running.value = true }
            override fun onDiscoveryStopped(serviceType: String) { _running.value = false }
            override fun onStartDiscoveryFailed(serviceType: String, errorCode: Int) {
                Log.w(TAG, "discovery start failed $errorCode")
                stop()
            }
            override fun onStopDiscoveryFailed(serviceType: String, errorCode: Int) {
                Log.w(TAG, "discovery stop failed $errorCode")
            }
            override fun onServiceFound(info: NsdServiceInfo) {
                synchronized(pending) { pending.addLast(info) }
                resolveNext()
            }
            override fun onServiceLost(info: NsdServiceInfo) {
                _found.value = _found.value.filterNot { it.name == info.serviceName }
            }
        }
        listener = l
        nsd.discoverServices(SERVICE_TYPE, NsdManager.PROTOCOL_DNS_SD, l)
    }

    fun stop() {
        listener?.let { runCatching { nsd.stopServiceDiscovery(it) } }
        listener = null
        lock?.release()
        lock = null
        _running.value = false
        synchronized(pending) { pending.clear() }
    }

    @Suppress("DEPRECATION") // resolveService is fine back to API 26; the 34+ replacement isn't.
    private fun resolveNext() {
        val next = synchronized(pending) {
            if (resolving || pending.isEmpty()) return
            resolving = true
            pending.removeFirst()
        }
        nsd.resolveService(next, object : NsdManager.ResolveListener {
            override fun onResolveFailed(info: NsdServiceInfo, errorCode: Int) {
                Log.w(TAG, "resolve ${info.serviceName} failed $errorCode")
                done()
            }
            override fun onServiceResolved(info: NsdServiceInfo) {
                val host = info.host?.hostAddress
                if (host != null) {
                    val f = Found(info.serviceName, host, info.port)
                    _found.value = (_found.value.filterNot { it.name == f.name } + f)
                        .sortedByDescending { it.name.contains("radio", ignoreCase = true) }
                }
                done()
            }
            private fun done() {
                synchronized(pending) { resolving = false }
                resolveNext()
            }
        })
    }

    companion object {
        private const val TAG = "RadioDiscovery"
        private const val SERVICE_TYPE = "_http._tcp."
    }
}
