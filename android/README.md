# Android build

A 32-bit ARM (`armeabi-v7a`) APK. The codebase needs 32-bit pointers (see the top of `CMakeLists.txt`), so there is no `arm64-v8a` build. A 64-bit Android device can still run the game if it also runs 32-bit apps. Check with:

```
adb shell getprop ro.product.cpu.abilist
```

If the output lists `armeabi-v7a`, the APK will run. Tested on a Retroid Pocket Nova (Snapdragon QCS8550, Android 13, OpenGL ES 3.2).

## What you need

- The Android SDK with NDK `27.3.13750724`, CMake `3.31.5` and platform `android-34`, all installable with the SDK Manager. Also JDK 17.
- Python 3, used at build time by `scripts/port/asset_cc_ordered.py`.
- The assets extracted from `baserom.u.z64`, as in the main readme. Run these in the repository root:
  ```
  ./scripts/extract_baserom.u.sh
  make convert_props convert_chrs convert_guns combine_images
  ```
- The SDL2 2.30.11 source in `third_party/`. This is the same version the Dockerfile pins, with the same SHA-1:
  ```
  mkdir -p third_party && cd third_party
  wget https://github.com/libsdl-org/SDL/releases/download/release-2.30.11/SDL2-2.30.11.tar.gz
  echo "f3cdd91df44653518d4f3db5fdc775170016aa85  SDL2-2.30.11.tar.gz" | sha1sum -c -
  tar -xzf SDL2-2.30.11.tar.gz
  ```

## Build

Create `android/local.properties` with your SDK path, for example `sdk.dir=/home/you/Android/Sdk`. Then:

```
cd android
./gradlew assembleRelease
```

The APK is `android/app/build/outputs/apk/release/app-release.apk`. It is signed with the debug key so it installs directly. Sign it with your own key before you share it.

## Install and play

```
adb install -r app/build/outputs/apk/release/app-release.apk
adb push baserom.u.z64 /sdcard/Android/data/com.projectmidas.game/files/Data/
```

The game's folder on the device is `/sdcard/Android/data/com.projectmidas.game/files/`. It plays the part of the folder next to the desktop executable: `midas.ini`, `Data/` (the cart image, saves, profiles), `Data/Logs/ge_direct.log`. You can also copy the cart there over USB. Any of `.z64`, `.v64` or `.n64` works. Start the game once first so that the folder exists.

Logs also go to logcat:

```
adb logcat -s ProjectMidas
```

## How it differs from the desktop builds

- **Compiler.** The NDK uses clang. `CMakeLists.txt` drops the x86-only flags, and adds `-ffp-contract=off` so that float rounding matches the SSE builds.
- **Asset layout.** Clang has no `-fno-toplevel-reorder`. The compiled-in asset objects go through `scripts/port/asset_cc_ordered.py`, which restores source order. It also fails the build if any `*_0x<offset>` symbol lands anywhere but its N64 offset.
- **Glue code.** `port/src/android_pc.c` provides `SDL_main`. It points the game's folder at app storage, sends stdout and stderr to logcat and the log file, and unpacks the controller and shader files bundled in the APK. It also runs the game on a 16 MB stack, since SDL's Java thread has about 1 MB.
- **Rendering.** It uses OpenGL ES 3.0 (`gfx_sdl2.cpp`) and always runs fullscreen.
- **Crashes.** The crash handler logs the fault site, then hands over to Android's crash dumper. The full backtrace appears in `adb logcat -s DEBUG`, and its offsets decode against the unstripped `libmain.so` under `android/app/build/intermediates/cxx/`.
