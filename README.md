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
1. Download the latest `BloodstainedAP.asi` from [Releases](https://github.com/vgfreak95/BloodstainedModdingSDK/releases/latest).
2. Download the latest x64 build of [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/latest).
3. Extract the loader's `dinput8.dll`, rename it to `winhttp.dll`, and place it in the Game Shipping Directory.
4. Create the Plugins Directory if it does not already exist, then place `BloodstainedAP.asi` inside it.
5. If upgrading from the old proxy-DLL installation, remove the old AP client `version.dll` from the Game Shipping Directory. `version.dll` is no longer the AP client.
6. Launch the game and load a save, then press `F5` to open the mod menu. The menu appearing confirms that the ASI plugin loaded.

## Archipelago:
Instructions can be found here: [Archipelago Setup Guide for Bloodstained: Ritual of the Night](https://github.com/vgfreak95/BloodstainedAP/blob/bloodstained/worlds/bloodstained_rotn/docs/setup_en.md)

## Building from Source:

### Prerequisites:
- Visual Studio version that supports .slnx
- MSBuild tools v143

### Building:
1. Clone/Download the `main` branch of the repository with command `git clone --recurse-submodules=subprojects`. `main` will always be stable latest functional code.
2. Open a VS Administrator Terminal, and `cd` into the project root directory/folder.
3. Run `vcpkg install --triplet x64-windows-static-md` to install openssl and zlib. The packages are defined in `vcpkg.json` in project root directory/folder.
4. Open the `BloodstainedModdingSDK.vcxproj` and modify the `<BSGamePath>` sections to match your Games target destination.
5. There are 2 Configurations available (Release WIP), use Debug x64 (should be default), then in Visual Studio, at the top Build -> Build Solution.
6. If there are any errors check FAQ section (WIP).
7. The project currently emits `version.dll`. Rename the built file to `BloodstainedAP.asi` and copy it into the Plugins Directory; do not install it as `version.dll` in the Game Shipping Directory.
8. Launch the game and load a save, then press `F5` to open the mod menu. Explore the GUI and add custom content inside `Mod\Gui.cpp`.

## Contributing:
Fork the main repository, and make a PR. I haven't made a thorough enough system, and don't believe it will get to that point.

## Shoutouts:
- Trexounay: For building EnderMagnolia Randomizer, its architecture is used as a foundation for this project.
- Lakifume: For building the Bloodstained TrueRandomizer, will be a huge help for research
- Tourmi: For creating an Baseline APWorld, various modding repositories and attempting this themselves. Their Archipelago foundation will help jumpstart the Archipelago portion of this project.
