# AutoRemesher for Blender

> **This is a fork** of [huxingyi/autoremesher](https://github.com/huxingyi/autoremesher) by
> [Jeremy Hu](https://github.com/huxingyi) (the author of [Dust3D](https://dust3d.org/)) that
> ports the remeshing engine into a **Blender extension**. All credit for the remeshing
> algorithm and the original desktop application goes to the original author — if this tool
> is useful to you, please consider supporting him:
>
> Buy Jeremy a coffee for staying up late coding :-) [![](https://www.paypalobjects.com/en_US/i/btn/btn_donate_SM.gif)](https://www.paypal.com/cgi-bin/webscr?cmd=_donations&business=GHALWLWXYGCU6&item_name=Support+me+coding+in+my+spare+time&currency_code=AUD&source=url)

AutoRemesher is an automatic quad remeshing (auto-retopology) tool that converts
high-polygon meshes into clean quad-based topology. It is built on top of
[Geogram](https://github.com/BrunoLevy/geogram),
[isotropicremesher](https://github.com/huxingyi/isotropicremesher),
[Eigen](https://eigen.tuxfamily.org/), [oneTBB](https://github.com/uxlfoundation/oneTBB) and
[others](ACKNOWLEDGEMENTS.html).

<img width="3644" height="2202" alt="autoremesher-1 0-screenshot" src="https://github.com/user-attachments/assets/47851f1e-127c-49af-81b7-0c8ac06fb3ad" />

## Installing the Blender extension

Requires **Blender 5.1+** (or Blender 4.2 LTS–5.0 with the `-blender42-` zips).

1. Download the extension zip for your platform (`autoremesher-<version>-<platform>.zip`)
   from the [blender-extension workflow artifacts](../../actions/workflows/blender-extension.yml)
   (or build it locally, see below).
2. In Blender: `Edit → Preferences → Get Extensions → ▾ (top-right menu) → Install from Disk…`
   and pick the zip.
3. In the 3D Viewport, press `N` and open the **AutoRemesher** tab. Select a mesh object and
   press **Remesh** — the result is added as a new object, progress shows in the status bar,
   and `Esc` cancels.

Parameters mirror the desktop app: **Target Quads**, **Edge Scaling**, **Sharp Edge**,
**Smooth Normal**, **Adaptivity**, plus a Blender-only **Island Detail Floor** that keeps
small disconnected parts (teeth, spikes) from collapsing into blobs.

The remeshing runs in a separate process of Blender's own Python, so a crash in the native
core can never take Blender down.

### Update notes

- **This fork has only been tested on macOS (Apple Silicon) so far.** CI builds
  wheels and extension zips for Windows x64, Linux x64 and macOS x64 as well — they are
  untested, vibe-coded installs; if you try one, reports (and PRs) are very welcome.
- Fork changes over upstream 1.0.0: Python bindings for the core (nanobind, stable ABI),
  the Blender extension, per-island failure recovery (retry + keep-triangles fallback
  instead of holes), input scale normalization, several crash/use-after-free fixes, and
  n-gon-aware output. See the commit history on the `blender-addon` branch for details.

### Building the extension locally

```sh
uv build --wheel --python 3.13        # or: pip wheel . (needs CMake + a C++17 toolchain)
python3 scripts/package_extension.py  # writes per-platform zips to dist/
```

See [blender_addon/README.md](blender_addon/README.md) for details, including the
headless end-to-end test.

## The original desktop application

The Qt desktop app still builds with qmake (`autoremesher.pro`) — see the
[upstream repository](https://github.com/huxingyi/autoremesher) for desktop releases and
build instructions.

### Links about the original AutoRemesher

- [Check out open-source auto-retopology tool AutoRemesher](http://www.cgchannel.com/2020/08/check-out-open-source-auto-retopology-tool-autoremesher/) **cgchannel.com**
- [A New Open-Source Auto-Retopology Tool](https://80.lv/articles/a-new-open-source-auto-retopology-tool/) **80.lv**
- [[Non-Blender] Autoremesher auto-retopology tool released](https://www.blendernation.com/2020/08/18/non-blender-autoremesher-auto-retopology-tool-released/) **blendernation.com**
- [オープンソースの新しいオートリメッシュツール Auto Remesher](https://cginterest.com/2020/08/20/%e3%82%aa%e3%83%bc%e3%83%97%e3%83%b3%e3%82%bd%e3%83%bc%e3%82%b9%e3%81%ae%e6%96%b0%e3%81%97%e3%81%84%e3%82%aa%e3%83%bc%e3%83%88%e3%83%aa%e3%83%a1%e3%83%83%e3%82%b7%e3%83%a5%e3%83%84%e3%83%bc%e3%83%ab-a/) **cginterest.com**
- [AutoRemesher 1.0.0-alpha - 超高速で高品質のクワッドポリゴン生成！Dust3D開発者によるオープンソースの自動リメッシュツール！](https://3dnchu.com/archives/autoremesher-1-0-0-alpha/) **3dnchu.com**
- [Open Source AutoRemesher released](https://cgpress.org/archives/open-source-remesher.html) **cgpress.org**
- [「autoremesher」多角形を自動でリトポしてれる無料トポロジーツール](https://modelinghappy.com/archives/30339) **modelinghappy.com**
- [Open Source Auto Remesher](https://blender-addons.org/open-source-auto-remesher/) **blender-addons.org**
- [AutoRemesher | Auto-Retopology-Tool](https://www.digitalproduction.com/2020/08/05/autoremesher-auto-retopology-tool/) **digitalproduction.com**
- [Autoremesher open source auto-retopology tool](https://blenderartists.org/t/autoremesher-open-source-auto-retopology-tool/1245131/126) **blenderartists.org**

## License

The remeshing core (this repository's original code) is licensed under the **MIT License**
by the original author — see [LICENSE](LICENSE). The Blender extension
(`blender_addon/`) is **GPL-3.0-or-later**, as required for add-ons on
[extensions.blender.org](https://extensions.blender.org/).

Bundled third-party libraries keep their own licenses, all GPL-compatible:
[Geogram](https://github.com/BrunoLevy/geogram) (BSD-3-Clause, Inria),
[Eigen](https://eigen.tuxfamily.org/) (MPL-2.0, built with `EIGEN_MPL2_ONLY`),
[oneTBB](https://github.com/uxlfoundation/oneTBB) (Apache-2.0),
[isotropicremesher](https://github.com/huxingyi/isotropicremesher) (MIT) and
[tinyobjloader](https://github.com/tinyobjloader/tinyobjloader) (MIT).

## Acknowledgements

See the full [ACKNOWLEDGEMENTS](ACKNOWLEDGEMENTS.html) for the list of libraries and
resources used in this project.

<!-- Sponsors begin --><!-- Sponsors end -->
