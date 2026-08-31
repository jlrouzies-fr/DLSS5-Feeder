package com.ck3dlss.installer

import java.nio.file.Files
import kotlin.io.path.createDirectories
import kotlin.io.path.createFile
import kotlin.io.path.writeText
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFalse
import kotlin.test.assertTrue

class InstallerBackendTest {
    @Test
    fun acceptsGameRootAndBinariesFolder() {
        val packageRoot = Files.createTempDirectory("ck3-package")
        packageRoot.resolve("DLSS5-CK3.ps1").createFile()
        packageRoot.resolve("binaries/dlss-payload").createDirectories()
        val gameRoot = Files.createTempDirectory("ck3-game")
        gameRoot.resolve("binaries").createDirectories()
        gameRoot.resolve("binaries/ck3.exe").createFile()
        val backend = InstallerBackend(packageRoot)

        assertTrue(backend.validateGameRoot(gameRoot.toString()).valid)
        val binariesResult = backend.validateGameRoot(gameRoot.resolve("binaries").toString())
        assertTrue(binariesResult.valid)
        assertEquals(gameRoot, binariesResult.normalizedRoot)
    }

    @Test
    fun rejectsFolderWithoutCk3Executable() {
        val packageRoot = Files.createTempDirectory("ck3-package")
        val backend = InstallerBackend(packageRoot)
        val result = backend.validateGameRoot(Files.createTempDirectory("not-ck3").toString())

        assertFalse(result.valid)
        assertTrue(result.message.contains("binaries\\ck3.exe"))
    }

    @Test
    fun buildsExplicitInstallCommand() {
        val packageRoot = Files.createTempDirectory("ck3-package")
        packageRoot.resolve("DLSS5-CK3.ps1").createFile()
        packageRoot.resolve("binaries/dlss-payload").createDirectories()
        val backend = InstallerBackend(packageRoot)
        val gameRoot = Files.createTempDirectory("ck3-game")
        val command = backend.buildPowerShellCommand(
            InstallerAction.INSTALL,
            InstallProfile.DLSS5_EXTENDED,
            gameRoot,
        )

        assertTrue(command.windowedContains("-Action", "Install"))
        assertTrue(command.windowedContains("-Profile", "DLSS5Extended"))
        assertTrue(command.contains("-AcceptDependencyLicenses"))
        assertTrue(command.contains("-AcceptRuntimeLicenses"))
    }

    @Test
    fun readsActiveProfileReceipt() {
        val packageRoot = Files.createTempDirectory("ck3-package")
        val gameRoot = Files.createTempDirectory("ck3-game")
        val active = gameRoot.resolve("binaries/dlss-active").createDirectories()
        active.resolve("CK3-DLSS-RUNTIME.json").writeText("""{ "Profile": "DLSS5" }""")

        assertEquals(InstallProfile.DLSS5, InstallerBackend(packageRoot).readActiveProfile(gameRoot))
    }

    private fun List<String>.windowedContains(first: String, second: String): Boolean =
        windowed(2).any { it[0] == first && it[1] == second }
}
