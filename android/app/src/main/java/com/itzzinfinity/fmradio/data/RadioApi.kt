package com.itzzinfinity.fmradio.data

import android.net.Network
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder

/**
 * Client for the *current* station-mode firmware (test_I2C_Radio.ino).
 *
 * Talks to the legacy routes exactly as the embedded web page does: every
 * control is a GET that returns the full /api/status JSON. No /api/v1 yet.
 */

data class Preset(val freq10k: Int, val name: String)

data class RadioStatus(
    val board: String,
    val radioPresent: Boolean,
    val freq10k: Int,
    val freqStr: String,
    val presetIdx: Int,          // -1 == manual tune
    val volume: Int,             // 0..15
    val rssi: Int,               // 0..127, firmware UI scales by /64
    val stereo: Boolean,
    val tuned: Boolean,
    val oledType: Int,           // 0 off, 1 SSD1306, 2 SH1106
    val oledFound: Boolean,
    val ip: String,
    val host: String,
    val ota: Boolean,
    val build: String,
    val presets: List<Preset>,
) {
    val stationName: String
        get() = presets.getOrNull(presetIdx)?.name ?: "Manual tune"

    companion object {
        /** Throws if the payload does not look like the radio's status JSON. */
        fun parse(body: String): RadioStatus {
            val j = JSONObject(body)
            // Current firmware has no device_id; these keys are the identity check.
            require(j.has("freq") && j.has("presets") && j.has("board")) {
                "Not an FM radio status response"
            }
            val arr = j.getJSONArray("presets")
            val presets = (0 until arr.length()).map {
                val p = arr.getJSONObject(it)
                Preset(p.getInt("f"), p.getString("n"))
            }
            return RadioStatus(
                board = j.optString("board"),
                radioPresent = j.optBoolean("radio"),
                freq10k = j.getInt("freq"),
                freqStr = j.optString("freqStr"),
                presetIdx = j.optInt("preset", -1),
                volume = j.optInt("vol"),
                rssi = j.optInt("rssi"),
                stereo = j.optBoolean("stereo"),
                tuned = j.optBoolean("tuned"),
                oledType = j.optInt("oled"),
                oledFound = j.optBoolean("oledFound"),
                ip = j.optString("ip"),
                host = j.optString("host"),
                ota = j.optBoolean("ota"),
                build = j.optString("build"),
                presets = presets,
            )
        }
    }
}

class RadioApi(
    private val baseUrl: String,
    /** When set, sockets are opened on this network instead of the default route. */
    private val network: Network?,
) {
    /** Result of one round trip, with timing for the diagnostics panel. */
    data class Timed(val status: RadioStatus, val millis: Long)

    suspend fun status() = get("/api/status", READ_TIMEOUT_MS)
    suspend fun preset(i: Int) = get("/api/preset?i=$i", READ_TIMEOUT_MS)
    suspend fun tune(freq10k: Int) = get("/api/tune?freq=$freq10k", READ_TIMEOUT_MS)
    suspend fun volume(v: Int) = get("/api/volume?v=$v", READ_TIMEOUT_MS)

    // Seek blocks the firmware loop for up to ~3 s, plus a re-tune.
    suspend fun seek(up: Boolean) =
        get("/api/seek?dir=${if (up) "up" else "down"}", SEEK_TIMEOUT_MS)

    suspend fun setPreset(i: Int, freq10k: Int, name: String) =
        get("/api/preset/set?i=$i&freq=$freq10k&name=${URLEncoder.encode(name, "UTF-8")}",
            READ_TIMEOUT_MS)

    suspend fun oled(type: String) = get("/api/oled?type=$type", READ_TIMEOUT_MS)

    private suspend fun get(path: String, timeoutMs: Int): Timed = withContext(Dispatchers.IO) {
        val url = URL(baseUrl.trimEnd('/') + path)
        val start = System.nanoTime()
        val conn = (network?.openConnection(url) ?: url.openConnection()) as HttpURLConnection
        try {
            conn.connectTimeout = CONNECT_TIMEOUT_MS
            conn.readTimeout = timeoutMs
            conn.useCaches = false
            // ESP8266WebServer serves one client at a time; don't hold the socket
            // open between polls (gotcha #6).
            conn.setRequestProperty("Connection", "close")
            val code = conn.responseCode
            val body = (if (code in 200..299) conn.inputStream else conn.errorStream)
                ?.bufferedReader()?.use { it.readText() } ?: ""
            if (code !in 200..299) throw RadioHttpException(code, body)
            Timed(RadioStatus.parse(body), (System.nanoTime() - start) / 1_000_000)
        } finally {
            conn.disconnect()
        }
    }

    companion object {
        const val CONNECT_TIMEOUT_MS = 3000
        const val READ_TIMEOUT_MS = 3000
        const val SEEK_TIMEOUT_MS = 8000
    }
}

class RadioHttpException(val code: Int, body: String) : Exception("HTTP $code: ${body.take(80)}")
