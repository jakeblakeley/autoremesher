# SPDX-License-Identifier: GPL-3.0-or-later
"""Subprocess entry point for the AutoRemesher Blender add-on.

Runs outside Blender (in Blender's bundled Python) so a native crash in the
remeshing core cannot take the Blender process down. Reads an input .npz
with vertices/triangles/parameters, writes the result .npz, and reports
progress on stdout as lines of "PROGRESS <fraction> <status>".
"""
import json
import sys
import threading
import time

import numpy as np


def main() -> int:
    in_path, out_path = sys.argv[1], sys.argv[2]
    data = np.load(in_path, allow_pickle=False)
    params = json.loads(str(data["params_json"]))

    import autoremesher_core

    remesher = autoremesher_core.Remesher(
        np.ascontiguousarray(data["vertices"], dtype=np.float64),
        np.ascontiguousarray(data["triangles"], dtype=np.uint32),
    )
    remesher.target_quad_count = int(params["target_quad_count"])
    remesher.scaling = float(params["scaling"])
    remesher.adaptivity = float(params["adaptivity"])
    remesher.sharp_edge_degrees = float(params["sharp_edge_degrees"])
    remesher.smooth_normal_degrees = float(params["smooth_normal_degrees"])

    result = {}

    def run():
        result["ok"] = remesher.run()

    thread = threading.Thread(target=run)
    thread.start()
    while thread.is_alive():
        print(f"PROGRESS {remesher.progress:.4f} {remesher.status}", flush=True)
        time.sleep(0.25)
    thread.join()

    if not result.get("ok", False):
        print("REMESH FAILED", flush=True)
        return 2

    np.savez(out_path, vertices=remesher.vertices(),
             face_indices=remesher.face_indices(),
             face_sizes=remesher.face_sizes())
    print("PROGRESS 1.0 Done", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
