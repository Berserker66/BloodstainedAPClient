[![CI](https://github.com/vgfreak95/BloodstainedModdingSDK/actions/workflows/msbuild.yml/badge.svg)](https://github.com/vgfreak95/BloodstainedModdingSDK/actions/workflows/msbuild.yml)
[![License](https://img.shields.io/github/license/vgfreak95/BloodstainedModdingSDK)](LICENSE)
[![Last Commit](https://img.shields.io/github/last-commit/vgfreak95/BloodstainedModdingSDK)](https://github.com/vgfreak95/BloodstainedModdingSDK/commits/main)

# BloodstainedModdingSDK
The BloodstainedModdingSDK is an SDK (Source Development Kit) which allows Developers to start Modding Bloodstained.
In the future, the project will be forked, and converted into an Archipelago. Given all tests are run on Steam version, there are no guarantees
the SDK will function on other platforms.

## Terms
- Game Shipping Directory = `steamapps\common\Bloodstained Ritual of the Night\BloodstainedRotN\Binaries\Win64`
- Plugins Directory = `steamapps\common\Bloodstained Ritual of the Night\BloodstainedRotN\Binaries\Win64\plugins`

## Installation
1. Download the Bloodstained AP 1.1.0 bundle from [Releases](https://github.com/vgfreak95/BloodstainedModdingSDK/releases/latest).
2. Copy the bundle's `Game` directory into the game installation directory, preserving its folder structure.
3. Remove `Content\Paks\~mods\Randomizer.pak` if it exists; the bundle supplies the pinned `BloodstainedAP.pak`.
4. If upgrading from the old proxy-DLL installation, remove `version.dll` from the Game Shipping Directory.
5. Launch the game, create a new story save, then press `F5` to open the mod menu.

True Randomizer, .NET, and UE4SS are not required. Pre-1.1.0 AP story saves are intentionally unsupported.

## Archipelago:
Instructions can be found here: [Archipelago Setup Guide for Bloodstained: Ritual of the Night](https://github.com/vgfreak95/BloodstainedAP/blob/bloodstained/worlds/bloodstained_rotn/docs/setup_en.md)

## Troubleshooting
The Release client writes connection transitions and location-check reconciliation to
`%LOCALAPPDATA%\BloodstainedRotN\Saved\Logs\BloodstainedAP.log`. Include that file with the matching Archipelago
server log when reporting connection or missing-check problems.

## Building from Source:

### Prerequisites:
- Visual Studio version that supports .slnx
- MSBuild tools v143

### Building:
1. Clone/Download the `main` branch of the repository with command `git clone --recurse-submodules=subprojects`. `main` will always be stable latest functional code.
2. Open a VS Administrator Terminal, and `cd` into the project root directory/folder.
3. Run `vcpkg install --triplet x64-windows-static-md` to install openssl and zlib. The packages are defined in `vcpkg.json` in project root directory/folder.
4. Open the `BloodstainedModdingSDK.vcxproj` and modify the `<BSGamePath>` sections to match your Games target destination.
5. Exit the game, then run `python tools/build_mod.py` for a parallel incremental Release x64 build. The helper uses 16 compiler workers by default and installs `BloodstainedAP.asi` into the Plugins Directory.
6. Use `python tools/build_mod.py --jobs 8` to choose another worker limit. Use `python tools/build_mod.py --clean` for a full rebuild before final in-game validation.
7. The helper refuses to build while the game is running and verifies that the built and installed plugins have matching SHA-256 hashes.
8. If there are any errors check FAQ section (WIP).
9. Launch the game and load a save, then press `F5` to open the mod menu. Explore the GUI and add custom content inside `Mod\Gui.cpp`.

## Contributing:
Fork the main repository, and make a PR. I haven't made a thorough enough system, and don't believe it will get to that point.

## Shoutouts:
- Trexounay: For building EnderMagnolia Randomizer, its architecture is used as a foundation for this project.
- Lakifume: For building the Bloodstained TrueRandomizer, will be a huge help for research
- Tourmi: For creating an Baseline APWorld, various modding repositories and attempting this themselves. Their Archipelago foundation will help jumpstart the Archipelago portion of this project.
