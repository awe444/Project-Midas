plugins {
    id("com.android.application")
}

// android/ sits one level under the repository root.
val repoRoot: File = rootProject.projectDir.parentFile

// SDL2 from source: CMakeLists.txt builds libSDL2.so from this tree and the
// Java half below comes from the same one (android/README.md fetches it).
val sdlRoot: File = File(repoRoot, "third_party/SDL2-2.30.11")

// The files a desktop release ships beside the exe, packaged as APK assets;
// port/src/android_pc.c copies them into the game's root dir at start.
val shippedAssets: File = layout.buildDirectory.dir("generated/midas-assets").get().asFile

val checkPrerequisites by tasks.registering {
    description = "Fails early, with directions, when the SDL2 source or the extracted assets are missing."
    doLast {
        if (!File(sdlRoot, "CMakeLists.txt").isFile) {
            error("SDL2 source not found at $sdlRoot - see android/README.md")
        }
        if (!File(repoRoot, "assets/images/combined/combined.bin").isFile) {
            error(
                "Extracted assets not found under ${File(repoRoot, "assets")}.\n" +
                    "Run ./scripts/extract_baserom.u.sh and " +
                    "make convert_props convert_chrs convert_guns combine_images in the repository root first."
            )
        }
    }
}

val stageShippedAssets by tasks.registering(Sync::class) {
    description = "Stages the controller mappings, the ini help and the base shader list as APK assets."
    from(File(repoRoot, "tools")) {
        include("gamecontrollerdb.txt", "gamecontrollerdefault.txt", "midas_ini_help.txt")
    }
    from(File(repoRoot, "tools/shaders_base.txt")) {
        into("Data")
    }
    into(shippedAssets)
}

android {
    namespace = "com.projectmidas.game"
    compileSdk = 34
    ndkVersion = "27.3.13750724"

    defaultConfig {
        applicationId = "com.projectmidas.game"
        minSdk = 24
        targetSdk = 34
        versionCode = 101
        versionName = "1.0.1"

        ndk {
            // 32-bit only: the codebase needs 32-bit pointers (CMakeLists.txt).
            // A 64-bit device runs it if it lists armeabi-v7a in
            // `adb shell getprop ro.product.cpu.abilist`.
            abiFilters += "armeabi-v7a"
        }

        externalNativeBuild {
            cmake {
                arguments += listOf(
                    // fast3d is the only C++; nothing else in the APK needs a shared libc++
                    "-DANDROID_STL=c++_static",
                    "-DSDL2_ANDROID_SRC=${sdlRoot.absolutePath}",
                )
                targets += listOf("ge")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = File(repoRoot, "CMakeLists.txt")
            version = "3.31.5"
        }
    }

    sourceSets["main"].apply {
        java.srcDir(File(sdlRoot, "android-project/app/src/main/java"))
        assets.srcDir(shippedAssets)
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            // signed with the debug key so `assembleRelease` installs directly;
            // use your own key before sharing an APK
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    lint {
        abortOnError = false
        // the release-time lint pass downloads its own toolchain and has
        // nothing to say about a wrapper around SDL's activity
        checkReleaseBuilds = false
    }
}

tasks.named("preBuild") { dependsOn(checkPrerequisites, stageShippedAssets) }
// the staged assets come from a task, so the asset merge waits for it
tasks.matching { it.name.startsWith("merge") && it.name.endsWith("Assets") }
    .configureEach { dependsOn(stageShippedAssets) }
