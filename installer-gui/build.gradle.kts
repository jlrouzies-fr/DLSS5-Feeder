import org.jetbrains.compose.desktop.application.dsl.TargetFormat
import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    kotlin("jvm") version "2.4.0"
    id("org.jetbrains.compose") version "1.12.0"
    id("org.jetbrains.kotlin.plugin.compose") version "2.4.0"
}

group = "com.ck3dlss"
version = "1.0.0"

kotlin {
    jvmToolchain(17)
    compilerOptions.jvmTarget.set(JvmTarget.JVM_17)
}

dependencies {
    implementation(compose.desktop.currentOs)
    implementation(compose.material3)
    testImplementation(kotlin("test"))
}

tasks.test {
    useJUnitPlatform()
}

compose.desktop {
    application {
        mainClass = "com.ck3dlss.installer.MainKt"

        nativeDistributions {
            targetFormats(TargetFormat.Exe)
            packageName = "CK3 DLSS Installer"
            packageVersion = project.version.toString()
            description = "Profile installer and validator for CK3 DLSS Vulkan"
            vendor = "CK3 DLSS Feeder"

            windows {
                iconFile.set(project.file("src/main/resources/icon.ico"))
                menuGroup = "CK3 DLSS Feeder"
                upgradeUuid = "ff11f29e-a253-4cfb-9f32-0a868eae438c"
            }
        }
    }
}
