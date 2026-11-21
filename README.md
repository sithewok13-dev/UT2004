# Unreal Tournament 2004 Modernized

An unofficial, non-commercial port of Unreal Tournament 2004 (build 3369) to modern hardware and operating systems. It is a fan project, made out of appreciation for a game that is now more than twenty years old.

## What it is

- Windows (x86, x64), Linux (x86, x64, arm64), Android (arm64), iOS and iPadOS (arm64) and the web (WebAssembly).
- SDL3 for windows and input, OpenGL ES 3 for rendering (ANGLE on Windows, WebGL 2 on the web, Apple's OpenGL ES on iOS), OpenAL for sound, Bullet behind the Karma physics API.
- Network compatible with retail build 3369. WebRTC transport next to UDP, so browsers can join and host.
- No editor. The command line tools of `ucc` are kept.

The game content is not part of this repository or its releases. The first start downloads it or takes it from a UT2004 install.

## Download

The [releases](https://github.com/K-Adam/UT2004/releases) carry a build for each platform, and the web version runs at <https://ut2004.dev>.

| File | Platform |
|---|---|
| `UT2004-<version>-win64.msi` | Windows x64 installer, for everyone on the PC or only for you. Remove it under Installed apps. |
| `UT2004-<version>-win64.zip` | The same without an installer. Unpack it and start `System/UT2004.exe`. |
| `UT2004-<version>-linux64.tar.gz`, `-linux-arm64.tar.gz` | Linux x64 and arm64, built on Ubuntu 22.04. Unpack it and run `./install.sh` (or `sudo ./install.sh --system` for everyone), then start `ut2004`. The installed folder has an `uninstall.sh`. SDL 3 is included, the other libraries come from your distribution: OpenAL, Vorbis, Bullet, squish, OpenGL ES, speexdsp, OpenSSL and curl. |
| `UT2004-<version>-android.apk` | Android 9 or newer on arm64. |
| `UT2004-<version>-ios.ipa` | iOS and iPadOS 15 or newer. Unsigned: install it with a sideloading tool such as AltStore or Sideloadly, which signs it with your Apple ID. |
| `UT2004-<version>-web.zip` | The files of the web page. Only the page at `ut2004.dev` gets the hosted content: on another domain the browser blocks the downloads from `assets.ut2004.dev`, and on localhost the page looks for a local content link instead. In both cases it offers to import a zip of a retail install. Only pages on the origins the web master allows get the server list and can host or join games through it. |

On the first start the game asks for its content. It can download the full game from the [Internet Archive](https://archive.org/details/ut2004-3369) (3.8 GB, about 13 GB of free space while it unpacks), or use the folder of a UT2004 install in place. The Android and iOS apps also offer a set of ten maps (1 GB). Android takes an archive or folder on the device. On iOS, copy a UT2004 folder or a `.zip`, `.tar` or `.tar.xz` archive of it into *On My iPhone > UT2004* with the Files app (or Finder on a computer) and pick *Use files I copied in*.

## Build

Third party libraries come from [vcpkg](https://github.com/microsoft/vcpkg) on Windows, Android, iOS and the web (`VCPKG_ROOT` must point to it), and from the system packages on Linux. Clone with `git submodule update --init`.

| Target | Needs | Configure | Build |
|---|---|---|---|
| Windows x64 | Visual Studio 2022 | `cmake --preset win64` | `cmake --build --preset win64-release` |
| Windows x86 | Visual Studio 2022 | `cmake --preset win32` | `cmake --build --preset win32-release` |
| Linux | GCC, Ninja, CMake 3.26, SDL 3.2 | `cmake --preset linux64` | `cmake --build --preset linux64-release` |
| Android arm64 | JDK 17, SDK platform 36, NDK r28c | `cmake --preset android-arm64` | `cmake --build --preset android-arm64-release` |
| iOS arm64 | macOS with Xcode 16, Ninja, autoconf, automake, libtool, pkg-config | `cmake --preset ios-arm64` | `cmake --build --preset ios-arm64-release` |
| Web | emsdk 7.0.0 | `cmake --preset web` | `cmake --build --preset web-release` |

Linux packages on Debian and Ubuntu: `build-essential ninja-build pkg-config zlib1g-dev libvorbis-dev libopenal-dev libbullet-dev libbullet-extras-dev libgles-dev libsquish-dev libssl-dev libcurl4-openssl-dev nlohmann-json3-dev libspeexdsp-dev`, plus SDL 3.2 or newer.

The output lands in `build/<preset>/System/`. The Android build writes `build/android-arm64/UT2004.apk`, the iOS build `build/ios-arm64/UT2004.ipa` and the web build `build/web/`.

The iOS app is unsigned unless the configure step gets `-DUT_IOS_SIGN_IDENTITY="Apple Development: …" -DUT_IOS_PROVISIONING_PROFILE=<file>.mobileprovision`, with `-DUT_IOS_BUNDLE_ID=` set to the profile's app ID. iOS can't host a dedicated server, as it suspends apps in the background, so it hosts listen servers only.

### Checks and releases

`python3 Tools/tidy.py` runs clang-tidy with the checks in `.clang-tidy` (Linux, needs Clang). GitHub Actions runs it on every push to `main` and every pull request and then builds and packages every platform. A tag like `v1.0.0` or `v1.0.0-rc.1` also publishes the files as a release and the web build to GitHub Pages.

The Windows installer is the `msi` target (`cmake --build --preset win64-release --target msi`). It needs the [WiX](https://wixtoolset.org) .NET tool with its UI and Util extensions on the path.

### Script packages

Online play needs UnrealScript packages identical to retail, so `Scripts/` holds the retail packages plus this port's two fix packages, and every build copies them to `build/Scripts/`. After changing `GUI2K4Fix` or `XAdminFix`, set `UT_RETAIL_DIR` to your UT2004 install (the compiler loads its content) and run `cmake --build --preset <build preset> --target ucc_make`. It compiles those two and puts them back into `Scripts/`.

## Run

Without an installer, start `UT2004.exe` (`ut2004-bin` on Linux) in the `System` folder of a release, or in `build/<preset>/System/` of your own build.

A dedicated server is `UCC server DM-Rankin` (`ucc-bin` on Linux). It does not ask for content, so start the game once on that machine or add the `Paths=` lines of your content to its `UT2004.ini`.

The web page is served from any static host, for example `python -m http.server 8000 -d build/web`.

## Files and logs

| Platform | Settings, saves and `UT2004.log` |
|---|---|
| Windows | the `System` folder the game runs from, or `%LocalAppData%\UT2004\System` when that folder is read-only, as in an install for everyone |
| Linux | `~/.ut2004/System/` (next to the binary with `-nohomedir`) |
| Android | `Android/data/org.ut2004.game/files/UT2004/System/` |
| iOS | `UT2004/System/` in the app's folder, which the Files app shows under *On My iPhone > UT2004* |
| Web | the browser's IndexedDB for the page |

Downloaded game content goes next to that `System` folder. Uninstalling keeps your settings, saves and the downloaded content, delete the folder to remove them.

## Network connections

The game only connects when you use an online feature. These are the defaults in `System/Default.ini`, and all of them can be changed or emptied there.

| What | Where | When |
|---|---|---|
| Community master servers | `utmaster.openspy.net`, `ut2004master.333networks.com` (port 28902) | The server browser is open, or you host an advertised server |
| Web master | `master.ut2004.dev` | The same, for servers and clients that use WebRTC |
| STUN | `stun.cloudflare.com` | Joining or hosting a browser hosted game |
| TURN relay | `turn.cloudflare.com`, with a credential from the web master | Joining a browser hosted game. The game data goes through it only when the host can't be reached directly |
| Content host | `assets.ut2004.dev` | First launch on Android, iOS and the web |
| Internet Archive | `archive.org` | First start, if you pick the download of the full game |
| Game servers | The server you pick | Joining a game |

The first start makes up a player name and a random stats password, so servers that track stats can list you without a prompt. Turn it off under Settings, Game, if you do not want that. There is no telemetry.

## Licensing

See `NOTICE.md`. In short: Epic's code and content stay Epic's, the files this port added are under the Mozilla Public License 2.0, and third party libraries keep their own licenses (`System/THIRD_PARTY.md`).

Security reports: see `SECURITY.md`.

## Ownership

Unreal, Unreal Tournament, Unreal Tournament 2004 and the Unreal Engine are trademarks or registered trademarks of Epic Games, Inc. The original source code and all game content belong to Epic Games, Inc. or their respective owners. This project is not affiliated with, endorsed by, or sponsored by Epic Games, Inc.

This repository grants no license to the original code or content. Third-party components remain under their own licenses.

I could not reach Epic Games about this project. If you represent Epic Games and have any request or concern about this repository, including a request to take it down, please contact me at contact@ut2004.dev. I will respond promptly and comply with any reasonable request.

## Free of charge

This project is free of charge and is not sold or monetized in any way.
