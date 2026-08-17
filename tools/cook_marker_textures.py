#!/usr/bin/env python3
"""Create cooked Bloodstained marker textures from the client PNG sources."""

from __future__ import annotations

import argparse
import struct
import sys
import tempfile
from pathlib import Path


ASSETS = (
    ("AP_ChestMarker", "chest-marker.png"),
    ("AP_WallMarker", "wall-marker.png"),
    ("AP_ShardMarker", "shard-marker.png"),
)


def strip_png_color_profile(source: Path, destination: Path) -> None:
    """Keep encoded pixels intact while preventing WIC from baking PNG gamma into them."""
    data = source.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise RuntimeError(f"not a PNG: {source}")

    output = bytearray(data[:8])
    offset = 8
    while offset < len(data):
        length = struct.unpack(">I", data[offset:offset + 4])[0]
        end = offset + 12 + length
        chunk_type = data[offset + 4:offset + 8]
        if chunk_type not in {b"sRGB", b"gAMA", b"cHRM", b"iCCP"}:
            output.extend(data[offset:end])
        offset = end
    destination.write_bytes(output)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--donor", type=Path, required=True)
    parser.add_argument("--png-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--ue4-dds-tools-dir", type=Path, required=True)
    args = parser.parse_args()

    for path in (args.donor, args.png_dir, args.ue4_dds_tools_dir):
        if not path.exists():
            parser.error(f"missing path: {path}")

    sys.path.insert(0, str(args.ue4_dds_tools_dir.parent))
    from UE4DDSTools.directx.dds import DDS
    from UE4DDSTools.directx.texconv import Texconv
    from UE4DDSTools.unreal.uasset import Uasset

    args.output_dir.mkdir(parents=True, exist_ok=True)
    converter = Texconv()
    old_package = "/Game/Core/UI/K2C/icon_8bitCrown"
    old_object = "icon_8bitCrown"

    for object_name, png_name in ASSETS:
        png = args.png_dir / png_name
        if not png.is_file():
            parser.error(f"missing marker source: {png}")

        asset = Uasset(str(args.donor), version="4.22")
        if asset.get_main_class_name() != "Texture2D" or len(asset.get_texture_list()) != 1:
            raise RuntimeError(f"unexpected marker donor layout: {args.donor}")
        names = [str(name) for name in asset.name_list]
        if old_package not in names or old_object not in names:
            raise RuntimeError(f"unexpected marker donor names: {args.donor}")

        package_name = f"/Game/Archipelago/UI/{object_name}"
        asset.update_name_list(names.index(old_package), package_name)
        asset.update_name_list(names.index(old_object), object_name)

        texture = asset.get_texture_list()[0]
        texture.to_uncompressed()
        with tempfile.TemporaryDirectory(prefix="bloodstained-marker-") as temporary:
            normalized_png = Path(temporary) / png.name
            strip_png_color_profile(png, normalized_png)
            dds_path = converter.convert_to_dds(
                str(normalized_png), texture.dxgi_format, out=temporary, no_mip=True,
                image_filter="linear", allow_slow_codec=True, verbose=False)
            texture.inject_dds(DDS.load(dds_path))
        texture.remove_mipmaps()

        asset.update_package_source(is_official=False)
        output = args.output_dir / f"{object_name}.uasset"
        asset.save(str(output))
        print(f"Cooked {png.name} -> {output}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
