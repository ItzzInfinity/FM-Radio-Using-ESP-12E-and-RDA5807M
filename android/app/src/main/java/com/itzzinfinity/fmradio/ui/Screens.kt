package com.itzzinfinity.fmradio.ui

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.LifecycleStartEffect
import com.itzzinfinity.fmradio.data.RadioStatus

// Same palette as the firmware's embedded web page.
object Palette {
    val bg = Color(0xFF0D1117)
    val card = Color(0xFF161B22)
    val line = Color(0xFF272E38)
    val fg = Color(0xFFE6EDF3)
    val dim = Color(0xFF8B949E)
    val acc = Color(0xFFF0883E)
    val ok = Color(0xFF3FB950)
    val warnBg = Color(0xFF3D1D1D)
    val warnFg = Color(0xFFFFB4B4)
}

@Composable
fun App(vm: RadioViewModel) {
    val ui by vm.ui.collectAsState()
    Box(
        Modifier
            .fillMaxSize()
            .background(Palette.bg)
            .safeDrawingPadding(),
        contentAlignment = Alignment.TopCenter,
    ) {
        Column(
            Modifier
                .widthIn(max = 520.dp)
                .fillMaxWidth()
                .verticalScroll(rememberScrollState())
                .padding(16.dp),
        ) {
            if (ui.connectedUrl == null) ConnectScreen(vm, ui) else RadioScreen(vm, ui)
        }
    }
}

// --- Connect ----------------------------------------------------------------------

@Composable
private fun ConnectScreen(vm: RadioViewModel, ui: UiState) {
    val found by vm.discovery.found.collectAsState()
    val scanning by vm.discovery.running.collectAsState()

    Heading("FM Radio")
    Card {
        Label("Radio address")
        OutlinedTextField(
            value = ui.hostInput,
            onValueChange = vm::onHostInput,
            singleLine = true,
            placeholder = { Text("192.168.1.42") },
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri),
            modifier = Modifier.fillMaxWidth(),
        )
        Spacer(Modifier.height(10.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton(
                onClick = { if (scanning) vm.discovery.stop() else vm.discovery.start() },
                modifier = Modifier.weight(1f),
            ) { Text(if (scanning) "Stop scan" else "Scan network") }
            AccentButton("Connect", enabled = !ui.connecting, modifier = Modifier.weight(1f)) {
                vm.connect()
            }
        }
        if (ui.connecting) {
            Spacer(Modifier.height(10.dp))
            LinearProgressIndicator(Modifier.fillMaxWidth(), color = Palette.acc)
        }
        ui.connectError?.let {
            Spacer(Modifier.height(10.dp))
            Text(it, color = Palette.warnFg, fontSize = 14.sp)
        }
    }

    if (scanning || found.isNotEmpty()) {
        Card {
            Label("Found on this network")
            if (found.isEmpty()) Dim("Looking for _http._tcp services…")
            found.forEach { f ->
                Row(
                    Modifier
                        .fillMaxWidth()
                        .clickable { vm.connect(f.host + if (f.port == 80) "" else ":${f.port}") }
                        .padding(vertical = 10.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Column(Modifier.weight(1f)) {
                        Text(f.name, color = Palette.fg, fontWeight = FontWeight.SemiBold)
                        Dim(f.baseUrl)
                    }
                    Text("Connect", color = Palette.acc)
                }
            }
        }
    }

    Dim(
        "Feasibility build for the current station-mode firmware. Put the phone on the " +
            "same Wi-Fi as the radio, then scan or type the IP shown on the OLED footer.",
    )
}

// --- Radio ------------------------------------------------------------------------

@Composable
private fun RadioScreen(vm: RadioViewModel, ui: UiState) {
    // Poll only while the screen is on top; resume (and re-check) when it returns.
    LifecycleStartEffect(Unit) {
        vm.startPolling()
        onStopOrDispose { vm.stopPolling() }
    }
    BackHandler { vm.disconnect() }

    val s = ui.status ?: return
    val controlsEnabled = ui.busy == null && !ui.linkLost

    Heading("FM Radio")

    if (ui.linkLost) Warn("Radio not responding — retrying. ${ui.diag.lastError ?: ""}")
    if (!s.radioPresent) Warn("RDA5807M not detected on the I2C bus — check wiring.")

    // Station
    Card {
        Row(verticalAlignment = Alignment.Bottom) {
            Text(s.freqStr, color = Palette.fg, fontSize = 52.sp, fontWeight = FontWeight.Bold)
            Spacer(Modifier.width(6.dp))
            Text("MHz", color = Palette.dim, fontSize = 18.sp, modifier = Modifier.padding(bottom = 8.dp))
        }
        Text(s.stationName, color = Palette.acc, fontWeight = FontWeight.SemiBold)
        Spacer(Modifier.height(8.dp))
        Row(verticalAlignment = Alignment.CenterVertically) {
            Box(
                Modifier
                    .size(8.dp)
                    .background(if (s.stereo) Palette.ok else Color(0xFF444444), CircleShape),
            )
            Spacer(Modifier.width(5.dp))
            Dim("Stereo")
            Spacer(Modifier.width(14.dp))
            Dim("Signal")
            Spacer(Modifier.width(8.dp))
            LinearProgressIndicator(
                progress = { (s.rssi / 64f).coerceIn(0f, 1f) },
                color = Palette.acc,
                trackColor = Color(0xFF21262D),
                modifier = Modifier.width(110.dp),
            )
        }
        ui.busy?.let {
            Spacer(Modifier.height(10.dp))
            Row(verticalAlignment = Alignment.CenterVertically) {
                CircularProgressIndicator(Modifier.size(16.dp), color = Palette.acc, strokeWidth = 2.dp)
                Spacer(Modifier.width(8.dp))
                Dim(it)
            }
        }
    }

    // Volume, seek, tune
    Card {
        val vol = ui.volumeDraft ?: s.volume
        Label("Volume $vol/15")
        Slider(
            value = vol.toFloat(),
            onValueChange = { vm.onVolumeDrag(it.toInt()) },
            onValueChangeFinished = vm::onVolumeDragEnd,
            valueRange = 0f..15f,
            steps = 14,
            enabled = !ui.linkLost,
        )
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedButton({ vm.seek(false) }, Modifier.weight(1f), enabled = controlsEnabled) {
                Text("◀◀ Seek")
            }
            OutlinedButton({ vm.seek(true) }, Modifier.weight(1f), enabled = controlsEnabled) {
                Text("Seek ▶▶")
            }
        }
        Spacer(Modifier.height(8.dp))
        var mhz by rememberSaveable { mutableStateOf("") }
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedTextField(
                value = mhz,
                onValueChange = { mhz = it },
                singleLine = true,
                placeholder = { Text("98.3") },
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
                modifier = Modifier.weight(1f),
            )
            AccentButton("Tune", enabled = controlsEnabled) { vm.tune(mhz) }
        }
    }

    PresetsCard(vm, s, controlsEnabled)

    // OLED
    Card {
        Label("OLED display")
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            listOf("off" to "Off", "ssd1306" to "SSD1306", "sh1106" to "SH1106")
                .forEachIndexed { i, (key, label) ->
                    Choice(label, selected = s.oledType == i, enabled = controlsEnabled,
                        modifier = Modifier.weight(1f)) { vm.setOled(key) }
                }
        }
        Spacer(Modifier.height(6.dp))
        Dim(if (s.oledFound) "Panel detected on the I2C bus." else "No panel on the bus — the radio runs fine without one.")
    }

    ui.message?.let {
        Card(Modifier.clickable { vm.clearMessage() }) { Text(it, color = Palette.fg, fontSize = 14.sp) }
    }

    DiagnosticsCard(vm, ui, s)
}

@Composable
private fun PresetsCard(vm: RadioViewModel, s: RadioStatus, enabled: Boolean) {
    var editing by rememberSaveable { mutableStateOf(false) }
    var slot by rememberSaveable { mutableIntStateOf(-1) }
    var name by rememberSaveable { mutableStateOf("") }
    var freq by rememberSaveable { mutableStateOf("") }

    Card {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Label("Presets", Modifier.weight(1f))
            Choice(if (editing) "Done" else "Edit", selected = editing) {
                editing = !editing
                if (!editing) slot = -1
            }
        }
        Spacer(Modifier.height(8.dp))
        s.presets.withIndex().chunked(3).forEach { row ->
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.padding(bottom = 8.dp)) {
                row.forEach { (i, p) ->
                    val sel = i == s.presetIdx || (editing && i == slot)
                    Column(
                        Modifier
                            .weight(1f)
                            .border(1.dp, if (sel) Palette.acc else Palette.line, RoundedCornerShape(10.dp))
                            .background(Color(0xFF21262D), RoundedCornerShape(10.dp))
                            .clickable(enabled = enabled || editing) {
                                if (editing) {
                                    slot = i; name = p.name; freq = "%.1f".format(p.freq10k / 100.0)
                                } else vm.selectPreset(i)
                            }
                            .padding(vertical = 10.dp, horizontal = 6.dp),
                        horizontalAlignment = Alignment.CenterHorizontally,
                    ) {
                        Text(p.name, color = if (sel) Palette.acc else Palette.fg,
                            maxLines = 1, overflow = TextOverflow.Ellipsis)
                        Text("%.1f".format(p.freq10k / 100.0), fontSize = 11.sp,
                            color = if (sel) Palette.acc else Palette.dim)
                    }
                }
                repeat(3 - row.size) { Spacer(Modifier.weight(1f)) }
            }
        }

        if (editing && slot >= 0) {
            Label("Slot ${slot + 1}")
            OutlinedTextField(name, { name = it.take(12) }, singleLine = true,
                label = { Text("Name (12 max)") }, modifier = Modifier.fillMaxWidth())
            Spacer(Modifier.height(8.dp))
            OutlinedTextField(freq, { freq = it }, singleLine = true, label = { Text("Frequency (MHz)") },
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
                modifier = Modifier.fillMaxWidth())
            Spacer(Modifier.height(10.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton({ slot = -1 }, Modifier.weight(1f)) { Text("Cancel") }
                AccentButton("Save preset", enabled = enabled, modifier = Modifier.weight(1f)) {
                    vm.savePreset(slot, freq, name)
                    slot = -1
                }
            }
        }
    }
}

@Composable
private fun DiagnosticsCard(vm: RadioViewModel, ui: UiState, s: RadioStatus) {
    val d = ui.diag
    Card {
        Label("Diagnostics")
        Dim("${ui.connectedUrl}  ·  ${s.board}")
        Dim("Firmware built ${s.build}")
        Dim("Last round trip ${d.lastRttMs?.let { "$it ms" } ?: "—"}  ·  ok ${d.okCount}  ·  failed ${d.failCount}")
        d.lastError?.let { Dim("Last error: $it") }
        Spacer(Modifier.height(6.dp))
        Row(verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text("Bind requests to Wi-Fi", color = Palette.fg, fontSize = 14.sp)
                Dim(if (d.wifiAvailable) "Wi-Fi network available" else "No Wi-Fi network — using default route")
            }
            Switch(d.bindToWifi, vm::setBindToWifi)
        }
        Spacer(Modifier.height(10.dp))
        OutlinedButton(vm::disconnect, Modifier.fillMaxWidth()) { Text("Disconnect") }
    }
}

// --- small building blocks -----------------------------------------------------------

@Composable
private fun Heading(text: String) = Text(
    text.uppercase(), color = Palette.dim, fontSize = 15.sp, fontWeight = FontWeight.SemiBold,
    letterSpacing = 1.2.sp, modifier = Modifier.padding(bottom = 12.dp),
)

@Composable
private fun Card(modifier: Modifier = Modifier, content: @Composable ColumnScope.() -> Unit) = Column(
    modifier
        .fillMaxWidth()
        .padding(bottom = 12.dp)
        .border(1.dp, Palette.line, RoundedCornerShape(14.dp))
        .background(Palette.card, RoundedCornerShape(14.dp))
        .padding(16.dp),
    content = content,
)

@Composable
private fun Warn(text: String) = Text(
    text, color = Palette.warnFg, fontSize = 14.sp,
    modifier = Modifier
        .fillMaxWidth()
        .padding(bottom = 12.dp)
        .background(Palette.warnBg, RoundedCornerShape(14.dp))
        .padding(16.dp),
)

@Composable
private fun Label(text: String, modifier: Modifier = Modifier) = Text(
    text.uppercase(), color = Palette.dim, fontSize = 12.sp, letterSpacing = 0.7.sp,
    modifier = modifier.padding(bottom = 6.dp),
)

@Composable
private fun Dim(text: String) = Text(text, color = Palette.dim, fontSize = 13.sp)

@Composable
private fun AccentButton(text: String, enabled: Boolean = true, modifier: Modifier = Modifier, onClick: () -> Unit) =
    Button(
        onClick, modifier, enabled = enabled,
        colors = ButtonDefaults.buttonColors(containerColor = Palette.acc, contentColor = Color(0xFF111111)),
    ) { Text(text, fontWeight = FontWeight.SemiBold) }

@Composable
private fun Choice(
    text: String, selected: Boolean, enabled: Boolean = true,
    modifier: Modifier = Modifier, onClick: () -> Unit,
) = OutlinedButton(
    onClick, modifier, enabled = enabled,
    border = androidx.compose.foundation.BorderStroke(1.dp, if (selected) Palette.acc else Palette.line),
    colors = ButtonDefaults.outlinedButtonColors(contentColor = if (selected) Palette.acc else Palette.fg),
) { Text(text, maxLines = 1) }
