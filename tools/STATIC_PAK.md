# Static pak rebuild

`build_static_pak.py` is the authoritative regeneration path for `BloodstainedAP.pak`.
It starts from the clean, seed-independent True Randomizer-derived pak, overlays the
current vanilla assets that Archipelago owns, applies every permanent cooked-asset
patch through `static_pak_patcher`, removes seed-specific generated assets, and
requires the resulting pak to contain 788 files.

The patcher includes and validates:

- ship wall and hidden-chest minimap marker proxies;
- the native map treasure registry entries for those proxies;
- the vanilla minimap Blueprint override used by native AP marker rendering;
- the client-controlled post-Den chest gate;
- O.D.'s initial lending returns raised from 1/2 to 3, without changing the
  unlimited post-defeat branch.
- cooked chest, wall, and shard marker textures owned by Unreal rather than
  imported from native-client resources at runtime.

Example from the workspace root:

```powershell
& "C:\Users\fabia\AppData\Local\Programs\Python\Python313\python.exe" `
  "D:\Bloodstained\repos\BloodstainedModdingSDK\tools\build_static_pak.py" `
  --source-pak "D:\Bloodstained\packages\BloodstainedAP-title-clean.pak" `
  --game-pak "C:\Program Files (x86)\Steam\steamapps\common\Bloodstained Ritual of the Night\BloodstainedRotN\Content\Paks\pakchunk0-WindowsNoEditor.pak" `
  --output "D:\Bloodstained\packages\BloodstainedAP.pak" `
  --repak "D:\Bloodstained\tools\repak\repak.exe" `
  --uasset-api-dir "D:\Bloodstained\randomizer\True-Randomization\Tools\UAssetAPI" `
  --ue4-dds-tools-dir "D:\Bloodstained\randomizer\True-Randomization-Release\Tools\UE4DDSTools"
```

After regeneration, update the pinned SHA-256 together in
`release/static_pak_manifest.json`, `tools/build_release.py`, and
`Mod/MainMenuStatus.cpp`, then clean-build the ASI so its runtime pak check matches.
