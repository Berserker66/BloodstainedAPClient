#!/usr/bin/env python3
"""Build the pinned static pak without dropping local cooked-asset patches."""

from __future__ import annotations

import argparse
import hashlib
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


VANILLA_OVERLAYS = (
    "BloodstainedRotN/Content/Core/UI/Map/MapManageBlueprint.uasset",
    "BloodstainedRotN/Content/Core/UI/Map/MapManageBlueprint.uexp",
    # Keep the vanilla minimap Blueprint in the override pak. The native client owns
    # AP marker rendering; this prevents stale True Randomizer UI bytecode resurfacing.
    "BloodstainedRotN/Content/Core/UI/Map/MiniMapBlueprint.uasset",
    "BloodstainedRotN/Content/Core/UI/Map/MiniMapBlueprint.uexp",
    "BloodstainedRotN/Content/Core/System/PBBookManager_BP.uasset",
    "BloodstainedRotN/Content/Core/System/PBBookManager_BP.uexp",
    # Start from the current vanilla room and apply only the AP-controlled gate.
    "BloodstainedRotN/Content/Core/Environment/ACT02_VIL/Level/m02VIL_005_Gimmick.umap",
    "BloodstainedRotN/Content/Core/Environment/ACT02_VIL/Level/m02VIL_005_Gimmick.uexp",
)

REQUIRED_OUTPUTS = VANILLA_OVERLAYS + (
    "BloodstainedRotN/Content/Core/Environment/ACT01_SIP/Level/m01SIP_004_Gimmick.umap",
    "BloodstainedRotN/Content/Core/Environment/ACT01_SIP/Level/m01SIP_004_Gimmick.uexp",
    "BloodstainedRotN/Content/Core/Environment/ACT01_SIP/Level/m01SIP_025_Gimmick.umap",
    "BloodstainedRotN/Content/Core/Environment/ACT01_SIP/Level/m01SIP_025_Gimmick.uexp",
    "BloodstainedRotN/Content/Archipelago/UI/AP_ChestMarker.uasset",
    "BloodstainedRotN/Content/Archipelago/UI/AP_ChestMarker.uexp",
    "BloodstainedRotN/Content/Archipelago/UI/AP_WallMarker.uasset",
    "BloodstainedRotN/Content/Archipelago/UI/AP_WallMarker.uexp",
    "BloodstainedRotN/Content/Archipelago/UI/AP_ShardMarker.uasset",
    "BloodstainedRotN/Content/Archipelago/UI/AP_ShardMarker.uexp",
)

FORBIDDEN_GENERATED_STEMS = ("DifficultSelecter", "EntryNameSetter", "VersionNumber")


def run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[bytes]:
    print("+", subprocess.list2cmdline(command))
    return subprocess.run(command, check=True, **kwargs)


def remove_seed_specific_assets(stage: Path) -> None:
    for path in stage.rglob("*"):
        if path.is_file() and path.stem in FORBIDDEN_GENERATED_STEMS:
            path.unlink()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-pak", type=Path, required=True,
                        help="Clean True Randomizer-derived static pak input")
    parser.add_argument("--game-pak", type=Path, required=True,
                        help="Vanilla pakchunk0-WindowsNoEditor.pak")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repak", type=Path, required=True)
    parser.add_argument("--uasset-api-dir", type=Path, required=True)
    parser.add_argument("--ue4-dds-tools-dir", type=Path, required=True)
    parser.add_argument("--dotnet", default="dotnet")
    parser.add_argument("--keep-workdir", action="store_true")
    args = parser.parse_args()

    for path in (args.source_pak, args.game_pak, args.repak):
        if not path.is_file():
            parser.error(f"missing file: {path}")
    for dll in ("UAssetAPI.dll", "UAssetSnippet.dll", "Newtonsoft.Json.dll"):
        if not (args.uasset_api_dir / dll).is_file():
            parser.error(f"missing dependency: {args.uasset_api_dir / dll}")
    if not (args.ue4_dds_tools_dir / "main.py").is_file():
        parser.error(f"missing UE4-DDS-Tools package: {args.ue4_dds_tools_dir}")

    work = Path(tempfile.mkdtemp(prefix="bloodstained-static-pak-",
                                 dir=args.output.resolve().parent))
    stage = work / "stage"
    try:
        run([str(args.repak), "unpack", "-q", "-o", str(stage), str(args.source_pak)])
        overlay_command = [str(args.repak), "unpack", "-q", "-f", "-o", str(stage)]
        for asset in VANILLA_OVERLAYS:
            overlay_command.extend(("-i", asset))
        overlay_command.append(str(args.game_pak))
        run(overlay_command)
        remove_seed_specific_assets(stage)

        project = Path(__file__).with_name("static_pak_patcher") / "StaticPakPatcher.csproj"
        nuget_config = project.with_name("NuGet.Config")
        run([
            args.dotnet, "restore", str(project), "--configfile", str(nuget_config),
            f"-p:UAssetApiDir={args.uasset_api_dir}",
        ])
        run([
            args.dotnet, "run", "--no-restore", "--project", str(project),
            f"-p:UAssetApiDir={args.uasset_api_dir}", "--", str(stage),
        ])
        marker_cooker = Path(__file__).with_name("cook_marker_textures.py")
        marker_donor = stage / "BloodstainedRotN/Content/Core/UI/K2C/icon_8bitCrown.uasset"
        marker_output = stage / "BloodstainedRotN/Content/Archipelago/UI"
        run([
            sys.executable, str(marker_cooker), "--donor", str(marker_donor),
            "--png-dir", str(Path(__file__).parents[1] / "Assets"),
            "--output-dir", str(marker_output),
            "--ue4-dds-tools-dir", str(args.ue4_dds_tools_dir),
        ])

        missing = [asset for asset in REQUIRED_OUTPUTS if not (stage / asset).is_file()]
        if missing:
            raise RuntimeError(f"static pak build is missing required assets: {missing}")

        args.output.parent.mkdir(parents=True, exist_ok=True)
        temporary_output = work / "BloodstainedAP.pak"
        run([
            str(args.repak), "pack", "-q", "--version", "V8A",
            "--compression", "Zlib", str(stage), str(temporary_output),
        ])
        listing = run([str(args.repak), "list", str(temporary_output)],
                      capture_output=True, text=True).stdout.splitlines()
        if len(listing) != 788:
            raise RuntimeError(f"static pak contains {len(listing)} files, expected 788")
        shutil.copy2(temporary_output, args.output)
        print(f"built {args.output} ({len(listing)} files, sha256={sha256(args.output)})")
        return 0
    finally:
        if args.keep_workdir:
            print(f"kept work directory: {work}")
        else:
            shutil.rmtree(work)


if __name__ == "__main__":
    raise SystemExit(main())
