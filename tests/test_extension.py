# SPDX-License-Identifier: GPL-3.0-or-later
"""Headless end-to-end test for the Blender extension.

Installs the packaged extension zip from dist/ and remeshes a subdivided
Suzanne through the operator. Run inside Blender with a throwaway
extensions directory:

    BLENDER_USER_EXTENSIONS=$(mktemp -d) blender -b --factory-startup \
        --python tests/test_extension.py
"""
import platform
from pathlib import Path

import bpy

MACHINE_TO_TAG = {"arm64": "arm64", "aarch64": "arm64", "amd64": "x64", "x86_64": "x64"}
SYSTEM_TO_TAG = {"Darwin": "macos", "Windows": "windows", "Linux": "linux"}

tag = f"{SYSTEM_TO_TAG[platform.system()]}_{MACHINE_TO_TAG[platform.machine().lower()]}"
dist = Path(__file__).resolve().parent.parent / "dist"
zips = sorted(dist.glob(f"autoremesher-*-{tag}.zip"))
assert zips, f"no extension zip for {tag} in {dist}; run scripts/package_extension.py"

bpy.ops.extensions.package_install_files(
    filepath=str(zips[-1]), repo="user_default", enable_on_install=True)
print("extension installed:", zips[-1].name)

import autoremesher_core  # noqa: E402  (importable once the extension is enabled)

for obj in list(bpy.data.objects):
    bpy.data.objects.remove(obj, do_unlink=True)
bpy.ops.mesh.primitive_monkey_add()
suzanne = bpy.context.active_object
suzanne.modifiers.new("subsurf", 'SUBSURF').levels = 2

bpy.context.scene.autoremesher.target_quad_count = 2000

result = bpy.ops.object.autoremesher_remesh()
assert result == {'FINISHED'}, f"operator returned {result}"

remeshed = bpy.context.view_layer.objects.active
assert remeshed is not None and remeshed.name != suzanne.name, "no new object created"
mesh = remeshed.data
quad_faces = sum(1 for p in mesh.polygons if len(p.vertices) == 4)
print(f"result {remeshed.name!r}: {len(mesh.vertices)} verts, "
      f"{len(mesh.polygons)} faces ({quad_faces} quads)")
assert len(mesh.polygons) > 100, "suspiciously few faces"
assert quad_faces == len(mesh.polygons), "non-quad faces in output"
print("BLENDER E2E TEST PASSED")
