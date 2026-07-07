# SPDX-License-Identifier: GPL-3.0-or-later
#
# AutoRemesher for Blender — automatic quad remeshing.
# UI and parameters mirror the AutoRemesher desktop app
# (https://github.com/huxingyi/autoremesher); the remeshing itself runs in
# the bundled autoremesher_core native module, executed in a subprocess of
# Blender's own Python so a crash in the native core can never take down
# Blender (and Esc genuinely cancels by terminating the process).

import json
import math
import os
import subprocess
import sys
import tempfile
import threading

import bpy
import numpy as np

# State of the current background job, if any. Only one remesh runs at a time.
_active_job = None


class AutoRemesherSettings(bpy.types.PropertyGroup):
    target_quad_count: bpy.props.IntProperty(
        name="Target Quads",
        description="Approximate number of quads to generate",
        default=50000, min=1000, max=1000000,
    )
    edge_scaling: bpy.props.FloatProperty(
        name="Edge Scaling",
        description="Edge scaling factor",
        default=1.0, min=1.0, max=4.0,
    )
    sharp_edge: bpy.props.FloatProperty(
        name="Sharp Edge",
        description="Dihedral angle threshold. Edges sharper than this are "
        "preserved as feature edges",
        subtype='ANGLE',
        default=math.radians(90.0), min=math.radians(30.0), max=math.radians(180.0),
    )
    smooth_normal: bpy.props.FloatProperty(
        name="Smooth Normal",
        description="Smooth normal angle threshold. 0 keeps the surface "
        "faceted; larger values respect the original vertex normals for a "
        "smoother remeshed surface",
        subtype='ANGLE',
        default=0.0, min=0.0, max=math.radians(180.0),
    )
    adaptivity: bpy.props.FloatProperty(
        name="Adaptivity",
        description="Curvature-adaptive quad density. 0 is uniform, 1 puts "
        "finer quads in high-curvature areas",
        subtype='FACTOR',
        default=1.0, min=0.0, max=1.0,
    )
    island_detail: bpy.props.IntProperty(
        name="Island Detail",
        description="Minimum quads across each small disconnected part "
        "(teeth, spikes), measured across the part's diagonal so it adapts "
        "to the part's size. Small parts are remeshed at higher density so "
        "they keep their shape instead of collapsing into blobs; never adds "
        "more detail than the original part had. 0 disables",
        default=10, min=0, max=100,
    )
    preserve_thin: bpy.props.BoolProperty(
        name="Preserve Thin Features",
        description="Use finer quads on thin features (claws, horns) based "
        "on the distance to the mesh's medial axis, so they keep their shape "
        "instead of being averaged away",
        default=True,
    )
    weld_shells: bpy.props.BoolProperty(
        name="Weld Shells",
        description="Voxel-remesh the input first (like ZBrush's DynaMesh) "
        "to fuse intersecting parts into one watertight surface before quad "
        "remeshing. Helps kitbashed meshes where teeth/plates float as "
        "separate shells; interior geometry is removed",
        default=False,
    )


class _Job:
    """A remesh subprocess plus a reader thread collecting its progress."""

    def __init__(self, command, env):
        creationflags = 0
        if os.name == "nt":
            creationflags = subprocess.CREATE_NO_WINDOW
        self.process = subprocess.Popen(
            command, env=env, text=True, creationflags=creationflags,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.progress = 0.0
        self.status = "Starting…"
        self.tail = []  # last output lines, for error reporting
        self._reader = threading.Thread(target=self._read, daemon=True)
        self._reader.start()

    def _read(self):
        for line in self.process.stdout:
            parts = line.rstrip().split(" ", 2)
            if parts[0] == "PROGRESS" and len(parts) >= 2:
                try:
                    self.progress = float(parts[1])
                except ValueError:
                    continue
                if len(parts) > 2 and parts[2]:
                    self.status = parts[2]
            else:
                self.tail = (self.tail + [line.rstrip()])[-15:]

    def finished(self):
        return self.process.poll() is not None

    def cancel(self):
        if self.process.poll() is None:
            self.process.terminate()


def _mesh_to_arrays(mesh):
    mesh.calc_loop_triangles()
    triangle_count = len(mesh.loop_triangles)
    vertices = np.empty(len(mesh.vertices) * 3, dtype=np.float32)
    mesh.vertices.foreach_get("co", vertices)
    triangles = np.empty(triangle_count * 3, dtype=np.int32)
    mesh.loop_triangles.foreach_get("vertices", triangles)
    return (vertices.reshape(-1, 3).astype(np.float64),
            triangles.reshape(-1, 3).astype(np.uint32))


def _weld_shells(context, vertices, triangles, target_quad_count):
    """Voxel-remesh the input into one watertight surface (DynaMesh-style),
    fusing intersecting shells before quad remeshing."""
    corners = vertices[triangles]
    area = np.linalg.norm(
        np.cross(corners[:, 1] - corners[:, 0], corners[:, 2] - corners[:, 0]),
        axis=1).sum() / 2.0
    # Half the target quad edge length, bounded so the voxel grid stays sane.
    edge = math.sqrt(max(area, 1e-12) / (0.866 * 0.5 * target_quad_count * 2))
    extent = float((vertices.max(axis=0) - vertices.min(axis=0)).max())
    voxel = max(edge * 0.5, extent / 700.0, 1e-6)

    mesh = bpy.data.meshes.new("autoremesher_weld")
    mesh.vertices.add(vertices.shape[0])
    mesh.vertices.foreach_set("co", vertices.astype(np.float32).ravel())
    mesh.loops.add(triangles.size)
    mesh.loops.foreach_set("vertex_index", triangles.astype(np.int32).ravel())
    mesh.polygons.add(triangles.shape[0])
    mesh.polygons.foreach_set(
        "loop_start", np.arange(0, triangles.size, 3, dtype=np.int32))
    mesh.polygons.foreach_set(
        "loop_total", np.full(triangles.shape[0], 3, dtype=np.int32))
    mesh.validate()
    mesh.update()

    temp = bpy.data.objects.new("autoremesher_weld", mesh)
    context.collection.objects.link(temp)
    try:
        modifier = temp.modifiers.new("weld", 'REMESH')
        modifier.mode = 'VOXEL'
        modifier.voxel_size = voxel
        depsgraph = context.evaluated_depsgraph_get()
        eval_temp = temp.evaluated_get(depsgraph)
        welded_mesh = eval_temp.to_mesh()
        welded = _mesh_to_arrays(welded_mesh)
        eval_temp.to_mesh_clear()
    finally:
        bpy.data.objects.remove(temp)
        bpy.data.meshes.remove(mesh)
    return welded


def _worker_command(vertices, triangles, params):
    """Write the job input and return (command, env, output_path)."""
    import autoremesher_core

    fd, in_path = tempfile.mkstemp(suffix=".npz", prefix="autoremesher_in_")
    os.close(fd)
    out_path = in_path.replace("_in_", "_out_")
    np.savez(in_path, vertices=vertices, triangles=triangles,
             params_json=json.dumps(params))

    worker = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "remesh_worker.py")
    env = os.environ.copy()
    core_dir = os.path.dirname(os.path.abspath(autoremesher_core.__file__))
    env["PYTHONPATH"] = core_dir + os.pathsep + env.get("PYTHONPATH", "")
    return [sys.executable, worker, in_path, out_path], env, in_path, out_path


class OBJECT_OT_autoremesher_remesh(bpy.types.Operator):
    bl_idname = "object.autoremesher_remesh"
    bl_label = "Remesh"
    bl_description = "Generate a quad remesh of the active object as a new object"
    bl_options = {'REGISTER', 'UNDO'}

    _timer = None
    _job = None
    _in_path = None
    _out_path = None
    _source_name = None

    @classmethod
    def poll(cls, context):
        obj = context.active_object
        return (
            _active_job is None
            and obj is not None
            and obj.type == 'MESH'
            and context.mode == 'OBJECT'
        )

    def execute(self, context):
        global _active_job

        settings = context.scene.autoremesher
        obj = context.active_object
        depsgraph = context.evaluated_depsgraph_get()
        eval_obj = obj.evaluated_get(depsgraph)
        mesh = eval_obj.to_mesh()
        vertices, triangles = _mesh_to_arrays(mesh)
        eval_obj.to_mesh_clear()

        if triangles.shape[0] == 0:
            self.report({'ERROR'}, "Mesh has no faces")
            return {'CANCELLED'}

        if settings.weld_shells:
            vertices, triangles = _weld_shells(
                context, vertices, triangles, settings.target_quad_count)
            if triangles.shape[0] == 0:
                self.report({'ERROR'}, "Weld Shells produced an empty mesh")
                return {'CANCELLED'}

        params = {
            "target_quad_count": settings.target_quad_count,
            "island_detail_spans": settings.island_detail,
            "feature_size_factor": 1.0 if settings.preserve_thin else 0.0,
            "scaling": settings.edge_scaling,
            "adaptivity": settings.adaptivity,
            "sharp_edge_degrees": math.degrees(settings.sharp_edge),
            "smooth_normal_degrees": math.degrees(settings.smooth_normal),
        }
        command, env, self._in_path, self._out_path = _worker_command(
            vertices, triangles, params)
        self._source_name = obj.name

        if bpy.app.background:
            # No modal timers in background mode; run synchronously.
            completed = subprocess.run(command, env=env, text=True,
                                       stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT)
            ok = completed.returncode == 0 and self._link_result(context)
            self._remove_temp_files()
            if not ok:
                print(completed.stdout[-2000:])
                self.report({'ERROR'}, "Remesh failed")
                return {'CANCELLED'}
            return {'FINISHED'}

        self._job = _Job(command, env)
        _active_job = self._job

        wm = context.window_manager
        self._timer = wm.event_timer_add(0.25, window=context.window)
        wm.modal_handler_add(self)
        return {'RUNNING_MODAL'}

    def modal(self, context, event):
        if event.type == 'ESC':
            self._job.cancel()
            self._cleanup(context)
            self.report({'WARNING'}, "Remesh cancelled")
            return {'CANCELLED'}

        if event.type != 'TIMER':
            return {'PASS_THROUGH'}

        if not self._job.finished():
            context.workspace.status_text_set(
                f"AutoRemesher: {self._job.status} — "
                f"{self._job.progress * 100.0:.0f}%  (Esc to cancel)")
            for area in context.screen.areas:
                if area.type == 'VIEW_3D':
                    area.tag_redraw()
            return {'RUNNING_MODAL'}

        exit_code = self._job.process.returncode
        succeeded = exit_code == 0
        detail = " | ".join(self._job.tail[-3:])
        try:
            if succeeded:
                succeeded = self._link_result(context)
        finally:
            self._cleanup(context)
        if not succeeded:
            if exit_code not in (0, 2):
                self.report({'ERROR'},
                            f"Remeshing process crashed (exit {exit_code}). "
                            "Blender is unaffected. " + detail)
            else:
                self.report({'ERROR'}, "Remesh failed. " + detail)
            return {'CANCELLED'}
        return {'FINISHED'}

    def _link_result(self, context):
        if not os.path.exists(self._out_path):
            return False
        data = np.load(self._out_path, allow_pickle=False)
        remeshed_vertices = data["vertices"]
        # Mostly quads, but hole fixing can emit triangles up to 7-gons.
        face_indices = data["face_indices"].astype(np.int32)
        face_sizes = data["face_sizes"].astype(np.int32)
        face_count = face_sizes.shape[0]
        if face_count == 0:
            return False

        mesh = bpy.data.meshes.new(f"{self._source_name} Remesh")
        mesh.vertices.add(remeshed_vertices.shape[0])
        mesh.vertices.foreach_set("co", remeshed_vertices.astype(np.float32).ravel())
        mesh.loops.add(face_indices.shape[0])
        mesh.loops.foreach_set("vertex_index", face_indices)
        mesh.polygons.add(face_count)
        loop_start = np.zeros(face_count, dtype=np.int32)
        np.cumsum(face_sizes[:-1], out=loop_start[1:])
        mesh.polygons.foreach_set("loop_start", loop_start)
        mesh.polygons.foreach_set("loop_total", face_sizes)
        mesh.validate()
        mesh.update(calc_edges=True)

        result = bpy.data.objects.new(mesh.name, mesh)
        source = bpy.data.objects.get(self._source_name)
        if source is not None:
            result.matrix_world = source.matrix_world
            collections = source.users_collection
        else:
            collections = ()
        for collection in collections or (context.collection,):
            collection.objects.link(result)

        for obj in context.selected_objects:
            obj.select_set(False)
        result.select_set(True)
        context.view_layer.objects.active = result
        quad_count = int((face_sizes == 4).sum())
        self.report({'INFO'},
                    f"Remeshed to {face_count} faces ({quad_count} quads)")
        return True

    def _remove_temp_files(self):
        for path in (self._in_path, self._out_path):
            if path and os.path.exists(path):
                try:
                    os.remove(path)
                except OSError:
                    pass

    def _cleanup(self, context):
        global _active_job
        _active_job = None
        self._remove_temp_files()
        context.workspace.status_text_set(None)
        if self._timer is not None:
            context.window_manager.event_timer_remove(self._timer)
            self._timer = None


class VIEW3D_PT_autoremesher(bpy.types.Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "AutoRemesher"
    bl_label = "AutoRemesher"

    def draw(self, context):
        layout = self.layout
        settings = context.scene.autoremesher

        column = layout.column(align=True)
        column.prop(settings, "target_quad_count")
        column.prop(settings, "edge_scaling")

        column = layout.column(align=True)
        column.prop(settings, "sharp_edge")
        column.prop(settings, "smooth_normal")
        column.prop(settings, "adaptivity")

        column = layout.column(align=True)
        column.prop(settings, "island_detail")
        column.prop(settings, "preserve_thin")
        column.prop(settings, "weld_shells")

        if _active_job is not None:
            box = layout.box()
            box.label(text=_active_job.status or "Remeshing…", icon='TIME')
            box.label(text=f"{_active_job.progress * 100.0:.0f}%")
        layout.operator(OBJECT_OT_autoremesher_remesh.bl_idname, icon='MOD_REMESH')


classes = (
    AutoRemesherSettings,
    OBJECT_OT_autoremesher_remesh,
    VIEW3D_PT_autoremesher,
)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)
    bpy.types.Scene.autoremesher = bpy.props.PointerProperty(type=AutoRemesherSettings)


def unregister():
    del bpy.types.Scene.autoremesher
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
