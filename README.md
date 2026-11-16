# PocketTrophy

A fork of **[ARMSX3](https://github.com/ARMSX2/ARMSX3)**, the Android PS3 emulator built on
[RPCS3](https://github.com/RPCS3/rpcs3), that adds **custom trophies**: trophy packs for games
that never had trophies, extra trophies for games that do, and a way to hide trophies that can
no longer be earned (for example ones that needed online servers that are gone).

> PocketTrophy is an independent fan project. It is not affiliated with, endorsed by or connected
> to Sony Interactive Entertainment, the RPCS3 project or the ARMSX2/ARMSX3 team. It is a modified
> version of ARMSX3; for the official app, go to the ARMSX3 project. PlayStation, PS3 and XMB are
> trademarks of Sony Interactive Entertainment Inc., used here only to say what the emulator is
> compatible with. **No firmware, games or other files of Sony's are included.**

PocketTrophy installs alongside the official ARMSX3 app and keeps its own data, so you can have
both.

## What PocketTrophy adds

- **Trophy rules**: a small rules file per game awards trophies by watching the game's memory.
  No game code is patched, and the game's own trophy file is never changed.
- **Custom trophies**, including a custom platinum and trophy sets for games without trophies.
- **Remove trophies** that can't be earned any more, so they no longer block the platinum.
- **Memory search** in the in-game menu, to find the number behind a new trophy (a counter, a
  score, a timer) without leaving the game, and turn it into a trophy with one tap.
- **Trophy packs**: share a game's rules and icons as one `.zip`, and import other people's.

The full rules format is in [CUSTOM_TROPHIES.md](CUSTOM_TROPHIES.md). Ready-made packs are in
[packs/](packs/).

## Getting started

**1. Install.** Download the APK from the latest build (GitHub **Actions** → *ARMSX3* →
artifact `pockettrophy-android-arm64`) and open it on your phone. It needs a 64-bit Android 13+
phone.

**2. Set up.** The setup wizard asks for the PS3 system software (firmware) and your games
folder. Install your own copy of the firmware; it is never included.

**3. Play.** Pick a game from the library. Trophies work as on the console, and the
**Trophies** screen lists what you've earned in every game.

**4. Install a trophy pack.** Open **Trophies → Import custom trophy pack** and choose the pack's
`.zip` (for example `packs/arkedo-custom-trophies.zip` for the Arkedo Series). Start the game and
the new trophies are there.

**5. Make your own trophies.**
1. Start the game and open the in-game menu → **Memory search**.
2. Search for the number you want a trophy for (enemies killed, coins, laps...). If you don't
   know its value, choose *I don't know the number* and filter by *It going up* / *It going down*
   as you play. Turn on *Show values and filter buttons over the game* to filter without leaving it.
3. When only a few results are left, tap one: the **Create trophy** form opens with the rule filled
   in. Give it a name, a description, a grade and, if you like, an icon, then save.
4. Restart the game to load the new trophy.

**6. Share.** **Trophies → Export pack** saves a game's rules and icons as a `.zip` anyone can
import.

## Credits

- **[RPCS3](https://rpcs3.net)** by the RPCS3 team and contributors (GPL-2.0): the PS3 emulator at
  the heart of this app.
- **[ARMSX3](https://github.com/ARMSX2/ARMSX3)** by the ARMSX2 team: the Android port, its
  interface and everything else PocketTrophy is built on. Almost all of the code in this
  repository is theirs or RPCS3's; PocketTrophy's own work is the custom trophy system.
- The third-party libraries inside the app are listed in the app under **About → Licences**.
- The in-app patch list and GPU driver downloads come from their own projects, credited in the
  app where they appear.

## AI use

PocketTrophy's own additions (the custom trophy system, memory search, trophy packs and the
Arkedo pack) were built with heavy use of AI: most of that code and documentation was written by
Anthropic's Claude, working from my direction, testing and review. Please report anything that
doesn't work to this repository, not to ARMSX3 or RPCS3.

## Building

 arm64-v8a and armv8.2 is supported. You need the Android SDK with NDK r27 or newer,
CMake 3.30 or newer, and a JDK 17. Android Studio ships all of these.

Clone with submodules, then fetch the two third party checkouts that are not
submodules:

    git clone --recursive https://github.com/Hcode91/PocketTrophy.git
    cd PocketTrophy
    git clone https://github.com/SnowflakePowered/librashader 3rdparty/librashader
    git clone https://github.com/bylaws/libadrenotools android/armsx3-ui/app/src/main/cpp/libadrenotools

Build the core. This is the long part and produces an unstripped library of
around 1.3 GB:

    export ANDROID_HOME=$HOME/Library/Android/sdk
    cmake -B build-android -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=$ANDROID_HOME/ndk/<version>/build/cmake/android.toolchain.cmake \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-31 \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build build-android --target armsx3-core -j8

Strip it and put it where the app expects it:

    llvm-strip --strip-unneeded build-android/android/libarmsx3-core.so
    cp build-android/android/libarmsx3-core.so \
       android/armsx3-ui/app/src/main/jniLibs/arm64-v8a/

Then build the app:

    cd android/armsx3-ui
    export JAVA_HOME="/Applications/Android Studio.app/Contents/jbr/Contents/Home"
    ./gradlew :app:assembleRelease

The apk lands in app/build/outputs/apk/release/.

Note that the core library has to be rebuilt and copied again whenever anything
under rpcs3/ or android/src/ changes. Gradle does not build it for you.

The Discord Social SDK is proprietary and is not redistributed here. Get it from
Discord's developer portal and drop it in app/libs/ and
app/src/main/cpp/discord_sdk/ if you want that feature. The build skips it
otherwise.

Running it needs PS3 firmware, which is not included.

## Legal (from ARMSX3)

ARMSX3 includes no PS3 firmware, games or other files of Sony's. Install your own
copy of the PS3 firmware and play games you own.

Installing the firmware decrypts Sony's official update and unpacks the console's
internal flash (dev_flash) into ARMSX3's data folder, as RPCS3 does on every platform.
Boot XMB runs the PS3's own system software from there. XMB Mode runs none of Sony's
code: it is ARMSX3's own interface, which reads the PS3's words, icons, fonts, startup
sound and the settings behind its wave from that firmware install. None of those
files ship with ARMSX3.

ARMSX3 is not affiliated with, endorsed by or sponsored by Sony Interactive
Entertainment. PlayStation, PS3 and XMB (XrossMediaBar) are trademarks or registered
trademarks of Sony Interactive Entertainment Inc., used here only to say what ARMSX3
is compatible with.

## Licence

GPL-2.0-only, the same as RPCS3. See LICENSE. Some files may be licensed
differently, check the file headers.

Based on RPCS3, https://github.com/RPCS3/rpcs3
