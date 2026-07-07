#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Package the Blender extension into per-platform zips.

Collects autoremesher_core wheels from dist/, groups them by Blender platform
tag, and produces one extension zip per platform in dist/ (the layout
`blender --command extension build --split-platforms` would create).

Usage: python scripts/package_extension.py [--wheel-dir dist]
"""

import argparse
import re
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ADDON_DIR = ROOT / "blender_addon"

WHEEL_PLATFORM_TO_BLENDER = [
    (re.compile(r"win_amd64"), "windows-x64"),
    (re.compile(r"win_arm64"), "windows-arm64"),
    (re.compile(r"macosx_[\d_]+_arm64"), "macos-arm64"),
    (re.compile(r"macosx_[\d_]+_x86_64"), "macos-x64"),
    (re.compile(r"manylinux.*x86_64"), "linux-x64"),
]


def blender_platform(wheel_name: str) -> str:
    for pattern, platform in WHEEL_PLATFORM_TO_BLENDER:
        if pattern.search(wheel_name):
            return platform
    raise SystemExit(f"Unrecognized wheel platform tag: {wheel_name}")


def patch_manifest(manifest: str, platform: str, wheel_names: list[str]) -> str:
    wheels = ", ".join(f'"./wheels/{name}"' for name in sorted(wheel_names))
    manifest = re.sub(r"^platforms = .*$", f'platforms = ["{platform}"]',
                      manifest, count=1, flags=re.M)
    manifest = re.sub(r"^wheels = .*$", f"wheels = [{wheels}]",
                      manifest, count=1, flags=re.M)
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--wheel-dir", default=ROOT / "dist", type=Path)
    args = parser.parse_args()

    wheels = sorted(args.wheel_dir.glob("autoremesher_core-*.whl"))
    if not wheels:
        raise SystemExit(f"No autoremesher_core wheels in {args.wheel_dir}")

    manifest_template = (ADDON_DIR / "blender_manifest.toml").read_text()
    version = re.search(r'^version = "([^"]+)"', manifest_template, re.M).group(1)
    addon_files = [p for p in ADDON_DIR.rglob("*")
                   if p.is_file() and p.name != "blender_manifest.toml"
                   and "wheels" not in p.parts and "__pycache__" not in p.parts]

    by_platform: dict[str, list[Path]] = {}
    for wheel in wheels:
        by_platform.setdefault(blender_platform(wheel.name), []).append(wheel)

    for platform, platform_wheels in sorted(by_platform.items()):
        zip_path = args.wheel_dir / f"autoremesher-{version}-{platform.replace('-', '_')}.zip"
        manifest = patch_manifest(manifest_template, platform,
                                  [w.name for w in platform_wheels])
        with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
            zf.writestr("blender_manifest.toml", manifest)
            for path in addon_files:
                zf.write(path, path.relative_to(ADDON_DIR))
            for wheel in platform_wheels:
                zf.write(wheel, f"wheels/{wheel.name}")
        print(f"wrote {zip_path.name}: {', '.join(w.name for w in platform_wheels)}")


if __name__ == "__main__":
    main()
