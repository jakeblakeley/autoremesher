# AutoRemesher for Blender

A Blender extension that runs [AutoRemesher](https://github.com/huxingyi/autoremesher)'s
automatic quad remeshing directly inside Blender, with the desktop app's
parameters (Target Quads, Edge Scaling, Sharp Edge, Smooth Normal,
Adaptivity) in a 3D View sidebar panel (N-panel → AutoRemesher tab).

Select a mesh object and press **Remesh**: the evaluated mesh (modifiers
applied) is remeshed on a background thread — progress shows in the status
bar — and the quad result is linked as a new object.

## Building

The remeshing core is a compiled Python module (`autoremesher_core`,
nanobind + CMake), bundled into the extension as a wheel:

```sh
uv build --wheel --python 3.13     # or: pip wheel . (needs cmake + a C++ toolchain)
python3 scripts/package_extension.py
```

This writes per-platform extension zips to `dist/`; install via Blender →
Preferences → Extensions → Install from Disk. CI
(`.github/workflows/blender-extension.yml`) builds wheels for
windows-x64 / macos-arm64 / macos-x64 / linux-x64 with cibuildwheel and
packages the zips: cp311 wheels for Blender 4.2 LTS–5.0 (Python 3.11) and a
cp312 stable-ABI wheel for Blender 5.1+ (Python 3.13 and any later bump).

Headless smoke test (installs the zip into a throwaway extensions dir and
remeshes Suzanne):

```sh
BLENDER_USER_EXTENSIONS=$(mktemp -d) blender -b --factory-startup --python tests/test_extension.py
```

## Licensing

The extension (this directory) is **GPL-3.0-or-later**, as required for
add-ons on [extensions.blender.org](https://extensions.blender.org).
The bundled `autoremesher_core` wheel is built from GPL-compatible sources:
AutoRemesher and isotropicremesher (MIT), geogram (BSD-3-Clause),
Eigen (MPL-2.0, compiled with `EIGEN_MPL2_ONLY`), oneTBB (Apache-2.0).
