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
zips = [z for z in sorted(dist.glob(f"autoremesher-*-{tag}.zip"))
        if "-blender42-" not in z.name]
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
sizes = [len(p.vertices) for p in mesh.polygons]
quad_faces = sum(1 for s in sizes if s == 4)
print(f"result {remeshed.name!r}: {len(mesh.vertices)} verts, "
      f"{len(mesh.polygons)} faces ({quad_faces} quads)")
assert len(mesh.polygons) > 100, "suspiciously few faces"
# Mostly quads; hole fixing can add triangles up to 7-gons.
assert quad_faces / len(mesh.polygons) > 0.5, "output is not quad-dominant"
assert max(sizes) <= 7, f"unexpected {max(sizes)}-gon in output"
# A vertex shared by a huge number of faces means degenerate hub geometry.
import numpy as np
loops = np.zeros(len(mesh.loops), dtype=np.int32)
mesh.loops.foreach_get("vertex_index", loops)
assert np.bincount(loops).max() < 50, "degenerate hub vertex in output"
print("BLENDER E2E TEST PASSED")
