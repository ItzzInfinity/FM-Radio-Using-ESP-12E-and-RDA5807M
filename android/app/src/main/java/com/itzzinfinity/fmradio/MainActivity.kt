package com.itzzinfinity.fmradio

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.viewModels
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import com.itzzinfinity.fmradio.ui.App
import com.itzzinfinity.fmradio.ui.Palette
import com.itzzinfinity.fmradio.ui.RadioViewModel

class MainActivity : ComponentActivity() {
    private val vm: RadioViewModel by viewModels()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            MaterialTheme(
                colorScheme = darkColorScheme(
                    primary = Palette.acc,
                    background = Palette.bg,
                    surface = Palette.card,
                    onSurface = Palette.fg,
                    outline = Palette.line,
                ),
            ) { App(vm) }
        }
    }
}
