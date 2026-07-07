# SPDX-License-Identifier: GPL-3.0-or-later
#
# AutoRemesher for Blender — automatic quad remeshing.
# UI and parameters mirror the AutoRemesher desktop app
# (https://github.com/huxingyi/autoremesher); the remeshing itself runs in
# the bundled autoremesher_core native module.

import math
import threading

import bpy
import numpy as np

# The current background job, if any. Only one remesh may run at a time:
# the core routes geogram progress reporting through shared state.
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


class OBJECT_OT_autoremesher_remesh(bpy.types.Operator):
    bl_idname = "object.autoremesher_remesh"
    bl_label = "Remesh"
    bl_description = "Generate a quad remesh of the active object as a new object"
    bl_options = {'REGISTER', 'UNDO'}

    _timer = None
    _thread = None
    _remesher = None
    _result = None
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
        import autoremesher_core

        settings = context.scene.autoremesher
        obj = context.active_object
        depsgraph = context.evaluated_depsgraph_get()
        eval_obj = obj.evaluated_get(depsgraph)
        mesh = eval_obj.to_mesh()
        mesh.calc_loop_triangles()

        triangle_count = len(mesh.loop_triangles)
        if triangle_count == 0:
            eval_obj.to_mesh_clear()
            self.report({'ERROR'}, "Mesh has no faces")
            return {'CANCELLED'}

        vertices = np.empty(len(mesh.vertices) * 3, dtype=np.float32)
        mesh.vertices.foreach_get("co", vertices)
        triangles = np.empty(triangle_count * 3, dtype=np.int32)
        mesh.loop_triangles.foreach_get("vertices", triangles)
        eval_obj.to_mesh_clear()

        remesher = autoremesher_core.Remesher(
            vertices.reshape(-1, 3).astype(np.float64),
            triangles.reshape(-1, 3).astype(np.uint32),
        )
        remesher.target_quad_count = settings.target_quad_count
        remesher.scaling = settings.edge_scaling
        remesher.sharp_edge_degrees = math.degrees(settings.sharp_edge)
        remesher.smooth_normal_degrees = math.degrees(settings.smooth_normal)
        remesher.adaptivity = settings.adaptivity

        self._remesher = remesher
        self._result = {}
        self._source_name = obj.name

        if bpy.app.background:
            # No modal timers in background mode; run synchronously.
            if not remesher.run():
                self.report({'ERROR'}, "Remesh failed")
                return {'CANCELLED'}
            self._link_result(context)
            return {'FINISHED'}

        # Daemon: the core has no cancellation hook, so an abandoned job is
        # left to finish (or die with Blender) in the background.
        self._thread = threading.Thread(
            target=lambda: self._result.update(ok=remesher.run()), daemon=True)
        self._thread.start()
        _active_job = remesher

        wm = context.window_manager
        self._timer = wm.event_timer_add(0.25, window=context.window)
        wm.modal_handler_add(self)
        return {'RUNNING_MODAL'}

    def modal(self, context, event):
        if event.type == 'ESC':
            self._cleanup(context)
            self.report({'WARNING'}, "Remesh abandoned (finishing in background)")
            return {'CANCELLED'}

        if event.type != 'TIMER':
            return {'PASS_THROUGH'}

        if self._thread.is_alive():
            status = self._remesher.status or "Remeshing"
            context.workspace.status_text_set(
                f"AutoRemesher: {status} — {self._remesher.progress * 100.0:.0f}%"
                "  (Esc to abandon)")
            for area in context.screen.areas:
                if area.type == 'VIEW_3D':
                    area.tag_redraw()
            return {'RUNNING_MODAL'}

        self._thread.join()
        succeeded = self._result.get("ok", False)
        try:
            if succeeded:
                self._link_result(context)
        finally:
            self._cleanup(context)
        if not succeeded:
            self.report({'ERROR'}, "Remesh failed — see the system console for details")
            return {'CANCELLED'}
        return {'FINISHED'}

    def _link_result(self, context):
        remesher = self._remesher
        remeshed_vertices = remesher.vertices()
        remeshed_quads = remesher.quads()
        quad_count = remeshed_quads.shape[0]

        mesh = bpy.data.meshes.new(f"{self._source_name} Remesh")
        mesh.vertices.add(remeshed_vertices.shape[0])
        mesh.vertices.foreach_set("co", remeshed_vertices.astype(np.float32).ravel())
        mesh.loops.add(quad_count * 4)
        mesh.loops.foreach_set("vertex_index", remeshed_quads.astype(np.int32).ravel())
        mesh.polygons.add(quad_count)
        mesh.polygons.foreach_set(
            "loop_start", np.arange(0, quad_count * 4, 4, dtype=np.int32))
        mesh.polygons.foreach_set(
            "loop_total", np.full(quad_count, 4, dtype=np.int32))
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
        self.report({'INFO'}, f"Remeshed to {quad_count} quads")

    def _cleanup(self, context):
        global _active_job
        _active_job = None
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
