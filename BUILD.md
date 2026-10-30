# Building OpenAscendance

How to build, run and test OpenAscendance on each platform and make its
packages (adapted from SilentHeritage's), and how its sources are organised;
[RELEASING.md](RELEASING.md) describes how the release packages are made and
published.

The game never ships the original files: every build reads the player's
own Ascendancy install at run time (the install setup asks for its folder
on the first start).

## Linux

CMake 3.25 or later, Ninja, clang (and lld for the release packages), git;
SDL3 also needs the usual X11 or Wayland, audio and GL development packages
(`packaging/linux/install_build_deps.sh` lists them).

    cmake -S . -B build/debug -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
    ninja -C build/debug
    ASAN_OPTIONS=detect_leaks=0 OA_DATA=<install dir> ctest --test-dir build/debug -j <processors>

`-DFETCHCONTENT_SOURCE_DIR_SDL3=<dir>` (and `_CATCH2`) reuses a checkout
instead of downloading it. `-DOA_VERSION=` sets the version
(`MAJOR.MINOR.PATCH[-suffix]`, `0.0.0-dev` by default).

Debug builds (the default) enable ASan and UBSan; under WSLg set
`ASAN_OPTIONS=detect_leaks=0`. Add `-DCMAKE_BUILD_TYPE=Release` for a release
build.

## Running

    build/debug/src/app/open_ascendance [--data <install dir> | --setup]
        [--sharp | --smooth] [--fullscreen | --window] [--variant standard|antagonizer|pecustom]

Without `--data` the game asks once for the install folder and remembers it.
Alt+Enter toggles fullscreen. `--screenshot <file.bmp> [--frames N]` with
`--keys <k1,k2,...>` runs headless and saves a frame, `--save-dir <dir>`
keeping its saves and Alt+G screenshots, and `--seed N` starts the galaxy of
the oracle's runs; `--trace <file.bin>` replays a window-event trace (an
oracle run's `trace.bin`) from the opening menu
([`src/app/command_line.h`](src/app/command_line.h)).

The opening menu has an **Options** button, the remake's own (the seven
original buttons are a little shorter to make room). Its screen sets:

- **Game variant**: the rules and opponents of the next new game, Standard
  (Ascendancy 1.6.5) or Antagonizer (the Antagonizer edition's). A game keeps
  its variant in its saves.
- **Display**: the full screen (the default) or a window (Alt+Enter also
  switches).
- **Scaling**: smooth or sharp pixels.
- **Music** and **Sound effects**: on or off (also Alt+M and Alt+N).
- **Language**: English or German texts, from the next start; saves load in
  either. The translations are built in from `src/lang/<language>/*.tsv` (one
  table per data file: number, English, translation); the fonts get the
  German letters.

OK (Enter) keeps the settings, Cancel (Esc) leaves them; Shift and a click on
a row explains it. They are kept in `options.cfg` in the user folder (the
sound in the original's `ascend.cfg`, which stays readable by the original);
`--sharp`/`--smooth`, `--fullscreen`/`--window` and `--variant` override them
for a run. While a tutorial plays, the opening menu has its original layout.

## Tests

    ninja -C build/debug oa_tests oa_game_tests
    OA_DATA=<install dir> ctest --test-dir build/debug -j <processors>

Tests of the original files' content are skipped when `OA_DATA` is unset;
the comparisons with the original's saves when `OA_ORACLE` does not name the
folder of the original's reference runs (the reverse-engineering
repository's `work/oracle`, made by its `tools/oracle.py`,
`docs/12-oracle.md`). The day tests (`tests/day_test.cpp`, tag `[day]`)
replay every case of the corpus; with `OA_DUMP_DIR` set they write the
remake's save of the first diverging day, to compare with that repository's
`tools/savdiff.py`. `tests/day_ui_test.cpp` plays the oracle's runs through
the user interface from their own traces (new games, days with their
game-event boxes, the tutorials) and compares the saves and Alt+G
screenshots (`OA_DAY_UI_RUN` picks runs by name, `OA_DUMP_DIR` keeps the
remake's files).

## Sources

The modules follow the original's (the reverse-engineering repository's
`docs/05-subsystems.md`), comments citing the functions they port with their
`PATCH.EXE` address:

- `src/core`: install access, the Watcom `rand` and `qsort`, logging.
- `src/assets`: the COB archives (`cfile`), shapes, palettes, haze tables,
  fonts, GIF, FLC, sounds and the text tables.
- `src/game`: the headless game core (no SDL): the game tables, the state
  (`Cosmos`, races, stars, lanes, planets, ships, research), the saves, the
  galaxy generation and the day (`cosmos.h`: `cosmos_end_day` and the
  contested-star walk; planets, research, ships and gizmos, diplomacy, the
  battle rules and the System Display's turn loop `battle_control.h`), with
  each variant's AI behind `ai.h` (Standard: `race_ai`, `battle`;
  Antagonizer: `antag_*`).
- `src/gfx`: the VFX blitters, the fonts, `GSystem` (screen, DAC, fades).
- `src/audio`: the mixer and `SoundSystem` (music and effects).
- `src/platform`: the SDL window, input, clock and sound output.
- `src/ui`: `WinMgr` (with the game events and the trace replay), `GWindow`
  and the toolkit's window classes, the game-event box, the FLIC player and
  window, `GameSession` (the game core as the screens see it)
  and `GameCoreSession` (it over `src/game`: saves, new game, the day).
- `src/screens`: the screens (new game, species history, load/save and
  tutorial lists, the star map, the System Display, planets, research,
  negotiation, intelligence, end of game).
- `src/app`: `main`, the command line and the install setup.

## Windows

Cross-compiled from Linux with clang, lld and the mingw-w64 runtime
(`packaging/windows/install_build_deps.sh`), without the tests (Catch2's
test discovery runs the test program at build time):

    cmake -S . -B build/windows -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-clang.cmake \
      -DCMAKE_BUILD_TYPE=Release -DOA_BUILD_TESTS=OFF
    ninja -C build/windows open_ascendance

The executable is self-contained (no DLL beyond Windows' own), windowed
(started from a console, it writes to that console), with the icon of
`packaging/icon/icon.ico` (`-DOA_WINDOWS_ICON=`), its version and its
manifest in its resources. Under WSL it runs directly:

    build/windows/src/app/open_ascendance.exe --data "$(wslpath -w <install dir>)" \
      --screenshot "$(wslpath -w /tmp)\\shot.bmp" --frames 200

## macOS

Apple's clang and Ninja (`brew install ninja`), for macOS 13.3 or later:

    cmake -S . -B build/debug -G Ninja -DCMAKE_OSX_DEPLOYMENT_TARGET=13.3
    ninja -C build/debug

The app bundle and the disk image are made by
`packaging/build_desktop.sh macos` (below), with a universal executable
(arm64 and x86_64).

## Android

[`android/build_apk.sh`](android/build_apk.sh) builds a package for
arm64-v8a and x86_64 (Android 7.0 or later) without Gradle: CMake with the
NDK for the native library (`cmake/android.cmake`, `libmain.so` loaded by
SDL's Java activity), javac and d8 for SDL's activity and ours
(`android/java`), aapt2, zipalign and apksigner.

    ANDROID_HOME=<sdk> android/build_apk.sh [--unsigned]

It needs the Android SDK (`platforms/android-36`, `build-tools/36.1.0`), NDK
29.0.14206865, a JDK 17 or later, ninja and zip; the other settings are at
the top of the script (`OA_SDL_SRC=<dir>` reuses an SDL checkout). On this
machine the SDK is SilentHeritage's:

    ANDROID_HOME=~/projects/SH/work/android-sdk android/build_apk.sh

The package is `build/android/OpenAscendance-debug.apk`, signed with
`debug.keystore` at the project's root (created on first use, not
committed; keep it, since an installed package only accepts updates signed
with the same key). On the first start, the game asks for the install's
folder through the system's folder picker and copies its files into the
app's storage (`src/app/android_documents.h`); the folder may then be
removed. Install and follow the log with:

    adb install -r build/android/OpenAscendance-debug.apk
    adb logcat -s SDL SDL/APP GameActivity

## Packages

`packaging/build_desktop.sh linux|windows|macos` builds the release
packages into `dist/` (described at its top): the Linux tarball and
AppImage, the Windows zip, the macOS disk image (on a Mac). `OA_VERSION`
sets their version.

The AppImage needs squashfs-tools' `mksquashfs` (4.4 or later, with zstd).
Without root, build it from its sources (the zlib, liblzma and libzstd
development headers installed) and give its path in `OA_MKSQUASHFS`:

    curl -fsSL https://github.com/plougher/squashfs-tools/archive/refs/tags/4.6.1.tar.gz | tar -xz
    make -C squashfs-tools-4.6.1/squashfs-tools ZSTD_SUPPORT=1
    OA_MKSQUASHFS=$PWD/squashfs-tools-4.6.1/squashfs-tools/mksquashfs packaging/build_desktop.sh linux

The AppImage runs without FUSE too, from a temporary copy:
`APPIMAGE_EXTRACT_AND_RUN=1 dist/OpenAscendance-<version>-linux-x86_64.AppImage`.

## Icon

The icon is an original design (a rising star, not the game's art):
`packaging/icon/open_ascendance.svg`. `packaging/icon/render_icons.sh`
renders the committed pictures from it (the desktop PNGs, the Windows
`.ico`, Android's launcher icons in `android/res/mipmap-*`), with python3
and the librsvg and cairo libraries, and ImageMagick 7.
