package com.ck3dlss.installer

import java.awt.Desktop
import java.nio.charset.Charset
import java.nio.file.Files
import java.nio.file.InvalidPathException
import java.nio.file.Path
import kotlin.io.path.absolute
import kotlin.io.path.exists
import kotlin.io.path.isDirectory
import kotlin.io.path.isRegularFile
import kotlin.io.path.name

enum class InstallProfile(
    val scriptValue: String,
    val title: String,
    val badge: String,
    val summary: String,
    val detail: String,
    val caution: Boolean = false,
) {
    DLSS45(
        scriptValue = "DLSS45",
        title = "DLSS 4.5 / Model M",
        badge = "RECOMMENDED",
        summary = "Native-resolution DLAA for RTX 20, 30 and 40 series.",
        detail = "Signed baseline with Model M neural reconstruction.",
    ),
    DLSS5(
        scriptValue = "DLSS5",
        title = "DLSS 5 Stock",
        badge = "SIGNED PREVIEW",
        summary = "NVIDIA's signed DLSS 5 and Neural Rendering runtime pair.",
        detail = "Use on hardware supported by the stock preview.",
    ),
    DLSS5_EXTENDED(
        scriptValue = "DLSS5Extended",
        title = "DLSS 5 Extended",
        badge = "ADVANCED",
        summary = "Broader compatibility through the ShortFuse runtime.",
        detail = "Modified runtime for GPUs unsupported by Stock.",
        caution = true,
    ),
}

enum class InstallerAction(val label: String) {
    INSTALL("Install profile"),
    VALIDATE("Validate installation"),
    DISABLE("Disable and restore"),
}

data class GameRootValidation(
    val valid: Boolean,
    val normalizedRoot: Path?,
    val message: String,
)

data class CommandResult(
    val exitCode: Int,
    val summary: String,
) {
    val succeeded: Boolean get() = exitCode == 0
}

class InstallerBackend(val packageRoot: Path) {
    val installerScript: Path = packageRoot.resolve("DLSS5-CK3.ps1")

    fun packageReady(): Boolean =
        installerScript.isRegularFile() && packageRoot.resolve("binaries/dlss-payload").isDirectory()

    fun validateGameRoot(value: String): GameRootValidation {
        val candidate = try {
            Path.of(value.trim().trim('"')).absolute().normalize()
        } catch (_: InvalidPathException) {
            return GameRootValidation(false, null, "The folder path is not valid.")
        }

        val normalized = if (candidate.name.equals("binaries", ignoreCase = true) &&
            candidate.resolve("ck3.exe").isRegularFile()
        ) {
            candidate.parent
        } else {
            candidate
        }

        return when {
            !normalized.exists() -> GameRootValidation(false, normalized, "The selected folder does not exist.")
            !normalized.resolve("binaries/ck3.exe").isRegularFile() ->
                GameRootValidation(false, normalized, "Expected binaries\\ck3.exe below this folder.")
            else -> GameRootValidation(true, normalized, "Crusader Kings III installation found.")
        }
    }

    fun defaultGameRoot(): Path {
        val candidates = buildList {
            System.getenv("CK3_GAME_ROOT")?.takeIf(String::isNotBlank)?.let { add(Path.of(it)) }
            add(packageRoot)
            System.getenv("ProgramFiles(x86)")?.let {
                add(Path.of(it, "Steam", "steamapps", "common", "Crusader Kings III"))
            }
            add(Path.of("C:/Program Files (x86)/Steam/steamapps/common/Crusader Kings III"))
        }
        return candidates.firstOrNull { validateGameRoot(it.toString()).valid } ?: packageRoot
    }

    fun readActiveProfile(gameRoot: Path): InstallProfile? {
        val receipt = gameRoot.resolve("binaries/dlss-active/CK3-DLSS-RUNTIME.json")
        if (!receipt.isRegularFile()) return null
        val value = Regex("\\\"Profile\\\"\\s*:\\s*\\\"([^\\\"]+)\\\"")
            .find(Files.readString(receipt))
            ?.groupValues
            ?.get(1)
            ?: return null
        return InstallProfile.entries.firstOrNull { it.scriptValue.equals(value, ignoreCase = true) }
    }

    fun buildPowerShellCommand(
        action: InstallerAction,
        profile: InstallProfile,
        gameRoot: Path,
    ): List<String> = buildList {
        add(findPowerShell())
        addAll(listOf("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", installerScript.toString()))
        addAll(listOf("-Action", action.name.lowercase().replaceFirstChar(Char::uppercase)))
        if (action == InstallerAction.INSTALL) {
            addAll(listOf("-Profile", profile.scriptValue))
        }
        addAll(listOf("-GameRoot", gameRoot.toString()))
        if (action == InstallerAction.INSTALL) {
            add("-AcceptDependencyLicenses")
            add("-AcceptRuntimeLicenses")
        }
    }

    fun runAction(
        action: InstallerAction,
        profile: InstallProfile,
        gameRoot: Path,
        onLine: (String) -> Unit,
    ): CommandResult {
        if (!packageReady()) return CommandResult(2, "The GUI cannot find its package payload.")
        val validation = validateGameRoot(gameRoot.toString())
        if (!validation.valid || validation.normalizedRoot == null) return CommandResult(2, validation.message)

        val process = ProcessBuilder(buildPowerShellCommand(action, profile, validation.normalizedRoot))
            .directory(packageRoot.toFile())
            .redirectErrorStream(true)
            .apply { environment()["POWERSHELL_TELEMETRY_OPTOUT"] = "1" }
            .start()

        process.inputStream.bufferedReader(Charset.defaultCharset()).useLines { lines ->
            lines.forEach(onLine)
        }
        val exit = process.waitFor()
        return CommandResult(
            exit,
            if (exit == 0) "${action.label} completed successfully." else "${action.label} failed with exit code $exit.",
        )
    }

    fun launchGame(gameRoot: Path, onLine: (String) -> Unit): CommandResult {
        val validationResult = runAction(
            InstallerAction.VALIDATE,
            InstallProfile.DLSS45,
            gameRoot,
            onLine,
        )
        if (!validationResult.succeeded) return validationResult

        val root = validateGameRoot(gameRoot.toString()).normalizedRoot
            ?: return CommandResult(2, "The CK3 folder is invalid.")
        val binaries = root.resolve("binaries")
        val executable = binaries.resolve("ck3.exe")
        val process = ProcessBuilder(executable.toString(), "-gdpr-compliant")
            .directory(binaries.toFile())
            .apply {
                environment()["VK_LAYER_PATH"] = binaries.resolve("dlss5-vulkan").toString()
                environment()["VK_INSTANCE_LAYERS"] = "VK_LAYER_feed_vk;VK_LAYER_reshade"
                environment()["DISABLE_VK_LAYER_reshade_1"] = "1"
                environment()["RESHADE_BASE_PATH_OVERRIDE"] = binaries.toString()
            }
            .start()
        onLine("Started CK3 (process ${process.pid()}) with the package-local Vulkan layers.")
        return CommandResult(0, "Crusader Kings III launched.")
    }

    fun openThirdPartyNotices(): Boolean {
        val notices = packageRoot.resolve("THIRD-PARTY-NOTICES.md")
        if (!notices.isRegularFile() || !Desktop.isDesktopSupported()) return false
        Desktop.getDesktop().open(notices.toFile())
        return true
    }

    companion object {
        fun locatePackageRoot(explicit: String? = null): Path {
            val seeds = buildList {
                explicit?.takeIf(String::isNotBlank)?.let { add(Path.of(it)) }
                System.getenv("CK3_DLSS_PACKAGE_ROOT")?.takeIf(String::isNotBlank)?.let { add(Path.of(it)) }
                add(Path.of(System.getProperty("user.dir")))
                System.getProperty("compose.application.resources.dir")?.let { add(Path.of(it)) }
                runCatching {
                    add(Path.of(InstallerBackend::class.java.protectionDomain.codeSource.location.toURI()))
                }
            }

            for (seed in seeds) {
                var current: Path? = if (Files.isDirectory(seed)) seed.absolute().normalize() else seed.parent
                repeat(8) {
                    val candidate = current ?: return@repeat
                    if (looksLikePackageRoot(candidate)) return candidate
                    val repositoryTemplate = candidate.resolve("ck3-package")
                    if (looksLikePackageRoot(repositoryTemplate)) return repositoryTemplate
                    current = candidate.parent
                }
            }
            return Path.of(System.getProperty("user.dir")).absolute().normalize()
        }

        private fun looksLikePackageRoot(path: Path): Boolean =
            path.resolve("DLSS5-CK3.ps1").isRegularFile() && path.resolve("binaries").isDirectory()

        private fun findPowerShell(): String {
            val systemRoot = System.getenv("SystemRoot") ?: "C:/Windows"
            val fullPath = Path.of(
                systemRoot,
                "System32",
                "WindowsPowerShell",
                "v1.0",
                "powershell.exe",
            )
            return if (fullPath.isRegularFile()) fullPath.toString() else "powershell.exe"
        }
    }
}
