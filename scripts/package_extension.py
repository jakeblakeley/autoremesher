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


# Blender rejects extensions containing any wheel its Python can't load, so
# each zip carries exactly one Python generation, scoped to the Blender
# releases shipping that Python (3.11 up to Blender 5.0; 3.13 from 5.1).
PYTHON_TAG_BLENDER_RANGE = {
    "cp312": ("5.1.0", None),     # stable-ABI wheel, Python 3.12+
    "cp311": ("4.2.0", "5.1.0"),  # Python 3.11; version_max is exclusive
}


def patch_manifest(manifest: str, platform: str, wheel_names: list[str],
                   python_tag: str) -> str:
    wheels = ", ".join(f'"./wheels/{name}"' for name in sorted(wheel_names))
    manifest = re.sub(r"^platforms = .*$", f'platforms = ["{platform}"]',
                      manifest, count=1, flags=re.M)
    manifest = re.sub(r"^wheels = .*$", f"wheels = [{wheels}]",
                      manifest, count=1, flags=re.M)
    version_min, version_max = PYTHON_TAG_BLENDER_RANGE[python_tag]
    replacement = f'blender_version_min = "{version_min}"'
    if version_max is not None:
        replacement += f'\nblender_version_max = "{version_max}"'
    manifest = re.sub(r"^blender_version_min = .*$", replacement,
                      manifest, count=1, flags=re.M)
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--wheel-dir", default=ROOT / "dist", type=Path)
    parser.add_argument("--python-tag", choices=sorted(PYTHON_TAG_BLENDER_RANGE),
                        default="cp312",
                        help="which wheel generation to package (default: cp312)")
    args = parser.parse_args()

    wheels = sorted(args.wheel_dir.glob(f"autoremesher_core-*-{args.python_tag}-*.whl"))
    if not wheels:
        raise SystemExit(
            f"No {args.python_tag} autoremesher_core wheels in {args.wheel_dir}")

    manifest_template = (ADDON_DIR / "blender_manifest.toml").read_text()
    tagline = re.search(r'^tagline = "([^"]*)"', manifest_template, re.M).group(1)
    if len(tagline) > 64 or tagline.endswith("."):
        raise SystemExit(f"manifest tagline invalid (max 64 chars, no trailing "
                         f"period): {len(tagline)} chars")
    version = re.search(r'^version = "([^"]+)"', manifest_template, re.M).group(1)
    addon_files = [p for p in ADDON_DIR.rglob("*")
                   if p.is_file() and p.name != "blender_manifest.toml"
                   and "wheels" not in p.parts and "__pycache__" not in p.parts]

    by_platform: dict[str, list[Path]] = {}
    for wheel in wheels:
        by_platform.setdefault(blender_platform(wheel.name), []).append(wheel)

    # The Blender 4.2-5.0 (cp311) zips carry a marker in the filename; the
    # extensions platform needs them uploaded as a separate version anyway.
    marker = "" if args.python_tag == "cp312" else "-blender42"
    for platform, platform_wheels in sorted(by_platform.items()):
        zip_path = (args.wheel_dir /
                    f"autoremesher-{version}{marker}-{platform.replace('-', '_')}.zip")
        manifest = patch_manifest(manifest_template, platform,
                                  [w.name for w in platform_wheels], args.python_tag)
        with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
            zf.writestr("blender_manifest.toml", manifest)
            for path in addon_files:
                zf.write(path, path.relative_to(ADDON_DIR))
            for wheel in platform_wheels:
                zf.write(wheel, f"wheels/{wheel.name}")
        print(f"wrote {zip_path.name}: {', '.join(w.name for w in platform_wheels)}")


if __name__ == "__main__":
    main()
