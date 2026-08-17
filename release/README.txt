Bloodstained Archipelago 1.1.0
================================

Requirements
------------
- Bloodstained: Ritual of the Night on Windows
- Archipelago 0.6.7 or newer
- A NEW story save. Saves used by Bloodstained AP before 1.1.0 are intentionally rejected.

Game installation
-----------------
1. Open the game's installation folder (the folder which contains BloodstainedRotN).
2. Copy everything inside this bundle's Game folder there, preserving the folder structure.
3. Remove Content\Paks\~mods\Randomizer.pak if it exists. It conflicts with BloodstainedAP.pak.
4. True Randomizer, .NET, and UE4SS are not required.
5. Start the game, create/load a new story save, and press F5 to open the AP client.

Archipelago world installation
------------------------------
Install Archipelago\bloodstained_rotn.apworld with Archipelago's "Install APWorld" command.

Compatibility and content policy
--------------------------------
- Client/world version: 1.1.0
- Required Archipelago version: 0.6.7
- Static pak/save schema: 1
- Normal, Hard, and Nightmare slots are supported; the story save difficulty must match the slot.
- Paid DLC is excluded. Free update content is included even when the game data marks it as DLC.

BloodstainedAP.pak is a pinned, seed-independent game asset. Do not regenerate it or rename it to Randomizer.pak.

Third-party attribution
-----------------------
BloodstainedAP.pak is derived from the game-asset output of True Randomizer 3.0.7 by Lakifume, with seed-specific
assets and paid-DLC content removed and Archipelago-specific static patches added. True Randomizer is available at
https://github.com/Lakifume/True-Randomization and is distributed under the MIT License; see
LICENSE-True-Randomizer.txt. Ultimate ASI Loader is also distributed under the MIT License; see
LICENSE-Ultimate-ASI-Loader.txt.
