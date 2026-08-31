package com.ck3dlss.installer

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Checkbox
import androidx.compose.material3.ColorScheme
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.window.Window
import androidx.compose.ui.window.WindowState
import androidx.compose.ui.window.application
import java.awt.Dimension
import java.awt.EventQueue
import java.awt.Window as AwtWindow
import java.io.File
import javax.swing.JFileChooser
import javax.swing.JOptionPane

private val NvidiaGreen = Color(0xFF76B900)
private val NvidiaGreenSoft = Color(0xFF9AD63D)
private val Ink = Color(0xFF0C0F0D)
private val Panel = Color(0xFF151A17)
private val PanelRaised = Color(0xFF1D241F)
private val Border = Color(0xFF354038)
private val Muted = Color(0xFFABB5AE)
private val Warning = Color(0xFFFFC857)
private val Error = Color(0xFFFF6B6B)

private val InstallerColors: ColorScheme = darkColorScheme(
    primary = NvidiaGreenSoft,
    onPrimary = Color(0xFF172000),
    primaryContainer = Color(0xFF263516),
    onPrimaryContainer = Color(0xFFD9FFA5),
    secondary = Color(0xFFAFC8B6),
    background = Ink,
    onBackground = Color(0xFFE9F0EB),
    surface = Panel,
    onSurface = Color(0xFFE9F0EB),
    surfaceVariant = PanelRaised,
    onSurfaceVariant = Muted,
    outline = Border,
    error = Error,
)

private enum class UiOutcome { IDLE, RUNNING, SUCCESS, ERROR }

fun main(args: Array<String>) {
    val explicitRoot = args.indexOf("--package-root")
        .takeIf { it >= 0 && it + 1 < args.size }
        ?.let { args[it + 1] }
    val backend = InstallerBackend(InstallerBackend.locatePackageRoot(explicitRoot))

    application {
        Window(
            onCloseRequest = ::exitApplication,
            title = "CK3 DLSS Installer",
            state = WindowState(width = 1080.dp, height = 800.dp),
            icon = painterResource("icon.png"),
        ) {
            LaunchedEffect(Unit) {
                window.minimumSize = Dimension(940, 700)
            }
            MaterialTheme(colorScheme = InstallerColors) {
                InstallerScreen(backend, window)
            }
        }
    }
}

@Composable
private fun InstallerScreen(backend: InstallerBackend, owner: AwtWindow) {
    var selectedProfile by remember { mutableStateOf(InstallProfile.DLSS45) }
    var gameRoot by remember { mutableStateOf(backend.defaultGameRoot().toString()) }
    var licensesAccepted by remember { mutableStateOf(false) }
    var busy by remember { mutableStateOf(false) }
    var outcome by remember { mutableStateOf(UiOutcome.IDLE) }
    var status by remember {
        mutableStateOf(
            if (backend.packageReady()) "Ready to configure Crusader Kings III."
            else "Package payload was not found. Run this app from the extracted CK3 DLSS package.",
        )
    }
    var logText by remember {
        mutableStateOf(
            "Package: ${backend.packageRoot}\n" +
                "Choose a profile, review the licenses, then install. No game launch happens automatically.\n",
        )
    }
    var activeProfile by remember { mutableStateOf<InstallProfile?>(null) }

    val validation = backend.validateGameRoot(gameRoot)
    val normalizedRoot = validation.normalizedRoot
    val logScroll = rememberScrollState()

    fun appendLog(line: String) {
        EventQueue.invokeLater {
            logText = (logText + line + "\n").takeLast(80_000)
        }
    }

    fun startOperation(label: String, work: () -> CommandResult) {
        if (busy) return
        busy = true
        outcome = UiOutcome.RUNNING
        status = label
        logText += "\n> $label\n"
        Thread({
            val result = runCatching(work).getOrElse { error ->
                appendLog("ERROR: ${error.message ?: error::class.simpleName}")
                CommandResult(1, error.message ?: "The operation failed.")
            }
            EventQueue.invokeLater {
                busy = false
                outcome = if (result.succeeded) UiOutcome.SUCCESS else UiOutcome.ERROR
                status = result.summary
                activeProfile = normalizedRoot?.let(backend::readActiveProfile)
            }
        }, "ck3-dlss-installer-worker").apply {
            isDaemon = true
            start()
        }
    }

    LaunchedEffect(gameRoot, busy) {
        if (!busy && validation.valid && normalizedRoot != null) {
            activeProfile = backend.readActiveProfile(normalizedRoot)
        }
    }
    LaunchedEffect(logText) {
        logScroll.scrollTo(logScroll.maxValue)
    }

    Surface(modifier = Modifier.fillMaxSize(), color = Ink) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(horizontal = 28.dp, vertical = 22.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
        Header(activeProfile, outcome, status)

        GameFolderCard(
            path = gameRoot,
            validation = validation,
            enabled = !busy,
            onPathChange = { gameRoot = it },
            onBrowse = {
                chooseGameFolder(owner, gameRoot)?.let { gameRoot = it.absolutePath }
            },
        )

        Text(
            text = "Choose a runtime profile",
            style = MaterialTheme.typography.titleMedium,
            fontWeight = FontWeight.SemiBold,
        )
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            InstallProfile.entries.forEach { profile ->
                ProfileCard(
                    profile = profile,
                    selected = selectedProfile == profile,
                    active = activeProfile == profile,
                    enabled = !busy,
                    onSelect = { selectedProfile = profile },
                    modifier = Modifier.weight(1f),
                )
            }
        }

        if (selectedProfile.caution) {
            Surface(
                color = Color(0xFF2C2412),
                shape = RoundedCornerShape(10.dp),
                border = BorderStroke(1.dp, Color(0xFF6A5520)),
            ) {
                Text(
                    "Extended uses a modified compatibility runtime. Start with DLSS 4.5 or Stock unless your hardware requires it.",
                    color = Warning,
                    style = MaterialTheme.typography.bodySmall,
                    modifier = Modifier.padding(horizontal = 14.dp, vertical = 10.dp),
                )
            }
        }

        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Checkbox(
                checked = licensesAccepted,
                onCheckedChange = { licensesAccepted = it },
                enabled = !busy,
            )
            Text(
                "I accept the bundled dependency and runtime license terms.",
                style = MaterialTheme.typography.bodyMedium,
                modifier = Modifier.clickable(enabled = !busy) { licensesAccepted = !licensesAccepted },
            )
            Spacer(Modifier.weight(1f))
            OutlinedButton(
                onClick = {
                    if (!backend.openThirdPartyNotices()) {
                        status = "THIRD-PARTY-NOTICES.md could not be opened."
                        outcome = UiOutcome.ERROR
                    }
                },
                enabled = !busy,
            ) {
                Text("View notices")
            }
        }

        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            Button(
                onClick = {
                    val root = normalizedRoot ?: return@Button
                    startOperation("Installing ${selectedProfile.title}…") {
                        backend.runAction(InstallerAction.INSTALL, selectedProfile, root, ::appendLog)
                    }
                },
                enabled = backend.packageReady() && validation.valid && licensesAccepted && !busy,
                colors = ButtonDefaults.buttonColors(containerColor = NvidiaGreen),
                modifier = Modifier.weight(1.35f),
            ) {
                Text("Install ${selectedProfile.title}", fontWeight = FontWeight.Bold)
            }
            OutlinedButton(
                onClick = {
                    val root = normalizedRoot ?: return@OutlinedButton
                    startOperation("Validating the active installation…") {
                        backend.runAction(InstallerAction.VALIDATE, selectedProfile, root, ::appendLog)
                    }
                },
                enabled = backend.packageReady() && validation.valid && !busy,
                modifier = Modifier.weight(0.75f),
            ) {
                Text("Validate")
            }
            OutlinedButton(
                onClick = {
                    if (JOptionPane.showConfirmDialog(
                            owner,
                            "Disable CK3 DLSS and restore the renderer saved during installation?",
                            "Disable CK3 DLSS",
                            JOptionPane.YES_NO_OPTION,
                            JOptionPane.WARNING_MESSAGE,
                        ) == JOptionPane.YES_OPTION
                    ) {
                        val root = normalizedRoot ?: return@OutlinedButton
                        startOperation("Disabling CK3 DLSS…") {
                            backend.runAction(InstallerAction.DISABLE, selectedProfile, root, ::appendLog)
                        }
                    }
                },
                enabled = backend.packageReady() && validation.valid && !busy,
                modifier = Modifier.weight(0.75f),
            ) {
                Text("Disable")
            }
            Button(
                onClick = {
                    val root = normalizedRoot ?: return@Button
                    startOperation("Validating and launching CK3…") {
                        backend.launchGame(root, ::appendLog)
                    }
                },
                enabled = backend.packageReady() && validation.valid && activeProfile != null && !busy,
                modifier = Modifier.weight(0.85f),
            ) {
                Text("Launch CK3")
            }
        }

        if (busy) {
            LinearProgressIndicator(modifier = Modifier.fillMaxWidth())
        } else {
            HorizontalDivider(color = Border)
        }

        Surface(
            modifier = Modifier.fillMaxWidth().weight(1f),
            color = Color(0xFF090C0A),
            shape = RoundedCornerShape(12.dp),
            border = BorderStroke(1.dp, Border),
        ) {
            Column(
                modifier = Modifier
                    .fillMaxSize()
                    .verticalScroll(logScroll)
                    .padding(14.dp),
            ) {
                Text(
                    text = logText,
                    color = Color(0xFFBCD0C1),
                    fontFamily = FontFamily.Monospace,
                    fontSize = 12.sp,
                    lineHeight = 17.sp,
                )
            }
        }
    }
    }
}

@Composable
private fun Header(activeProfile: InstallProfile?, outcome: UiOutcome, status: String) {
    Row(modifier = Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Column(modifier = Modifier.weight(1f)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(
                    Modifier
                        .size(13.dp)
                        .background(NvidiaGreen, CircleShape),
                )
                Spacer(Modifier.width(9.dp))
                Text(
                    "CK3 DLSS",
                    fontSize = 26.sp,
                    fontWeight = FontWeight.Black,
                    letterSpacing = 0.2.sp,
                )
            }
            Text(
                "Safe profile setup for the Vulkan feeder",
                color = Muted,
                style = MaterialTheme.typography.bodyMedium,
                modifier = Modifier.padding(top = 2.dp),
            )
        }
        Column(horizontalAlignment = Alignment.End) {
            StatusPill(outcome, status)
            Text(
                activeProfile?.let { "Active: ${it.title}" } ?: "No active profile detected",
                color = if (activeProfile == null) Muted else NvidiaGreenSoft,
                style = MaterialTheme.typography.labelMedium,
                modifier = Modifier.padding(top = 7.dp),
            )
        }
    }
}

@Composable
private fun StatusPill(outcome: UiOutcome, text: String) {
    val color = when (outcome) {
        UiOutcome.IDLE -> Muted
        UiOutcome.RUNNING -> Color(0xFF63B3ED)
        UiOutcome.SUCCESS -> NvidiaGreenSoft
        UiOutcome.ERROR -> Error
    }
    Surface(
        color = color.copy(alpha = 0.12f),
        shape = RoundedCornerShape(999.dp),
        border = BorderStroke(1.dp, color.copy(alpha = 0.55f)),
    ) {
        Text(
            text = text,
            color = color,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            style = MaterialTheme.typography.labelMedium,
            modifier = Modifier.padding(horizontal = 12.dp, vertical = 7.dp).width(330.dp),
        )
    }
}

@Composable
private fun GameFolderCard(
    path: String,
    validation: GameRootValidation,
    enabled: Boolean,
    onPathChange: (String) -> Unit,
    onBrowse: () -> Unit,
) {
    Surface(
        color = Panel,
        shape = RoundedCornerShape(14.dp),
        border = BorderStroke(1.dp, Border),
    ) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text("Crusader Kings III folder", fontWeight = FontWeight.SemiBold)
            Row(verticalAlignment = Alignment.CenterVertically) {
                OutlinedTextField(
                    value = path,
                    onValueChange = onPathChange,
                    singleLine = true,
                    enabled = enabled,
                    modifier = Modifier.weight(1f),
                    placeholder = { Text("Select the folder containing binaries\\ck3.exe") },
                )
                Spacer(Modifier.width(10.dp))
                OutlinedButton(onClick = onBrowse, enabled = enabled) { Text("Browse…") }
            }
            Text(
                validation.message,
                color = if (validation.valid) NvidiaGreenSoft else Error,
                style = MaterialTheme.typography.bodySmall,
            )
        }
    }
}

@Composable
private fun ProfileCard(
    profile: InstallProfile,
    selected: Boolean,
    active: Boolean,
    enabled: Boolean,
    onSelect: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val accent = if (profile.caution) Warning else NvidiaGreenSoft
    Card(
        modifier = modifier.height(176.dp),
        onClick = onSelect,
        enabled = enabled,
        colors = CardDefaults.cardColors(
            containerColor = if (selected) Color(0xFF202A21) else PanelRaised,
        ),
        border = BorderStroke(if (selected) 2.dp else 1.dp, if (selected) accent else Border),
        shape = RoundedCornerShape(14.dp),
    ) {
        Column(
            modifier = Modifier.fillMaxHeight().padding(14.dp),
            verticalArrangement = Arrangement.spacedBy(7.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                RadioButton(selected = selected, onClick = onSelect, enabled = enabled)
                Text(
                    profile.title,
                    fontWeight = FontWeight.Bold,
                    style = MaterialTheme.typography.titleSmall,
                    modifier = Modifier.weight(1f),
                )
            }
            Text(profile.badge, color = accent, fontSize = 10.sp, fontWeight = FontWeight.Bold)
            Text(profile.summary, style = MaterialTheme.typography.bodySmall, color = Color(0xFFD7E0D9))
            Text(profile.detail, style = MaterialTheme.typography.bodySmall, color = Muted)
            Spacer(Modifier.weight(1f))
            if (active) {
                Text("CURRENTLY ACTIVE", color = NvidiaGreenSoft, fontSize = 10.sp, fontWeight = FontWeight.Bold)
            }
        }
    }
}

private fun chooseGameFolder(owner: AwtWindow, currentPath: String): File? {
    val current = File(currentPath).takeIf(File::exists)
    val chooser = JFileChooser(current).apply {
        dialogTitle = "Select the Crusader Kings III folder"
        fileSelectionMode = JFileChooser.DIRECTORIES_ONLY
        isAcceptAllFileFilterUsed = false
        selectedFile = current
    }
    return if (chooser.showOpenDialog(owner) == JFileChooser.APPROVE_OPTION) chooser.selectedFile else null
}
