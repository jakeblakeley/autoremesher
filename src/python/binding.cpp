/*
 *  Copyright (c) 2026 AutoRemesher Contributors.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:

 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.

 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 */
#include <AutoRemesher/AutoRemesher>

#include <geogram/basic/common.h>

#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/string.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace nb = nanobind;

namespace {

void ensureGeogramInitialized()
{
    static std::once_flag flag;
    std::call_once(flag, [] { GEO::initialize(); });
}

using VertexArray = nb::ndarray<nb::numpy, double, nb::shape<-1, 3>, nb::c_contig, nb::device::cpu>;
using FaceArray = nb::ndarray<nb::numpy, uint32_t, nb::shape<-1, 4>, nb::c_contig, nb::device::cpu>;

// Wraps AutoRemesher::AutoRemesher for use from a Python worker thread:
// run() blocks with the GIL released while progress/status stay readable
// from other Python threads.
class Remesher {
public:
    Remesher(nb::ndarray<double, nb::shape<-1, 3>, nb::c_contig, nb::device::cpu> vertices,
        nb::ndarray<uint32_t, nb::shape<-1, 3>, nb::c_contig, nb::device::cpu> triangles)
    {
        m_vertices.reserve(vertices.shape(0));
        const double* v = vertices.data();
        for (size_t i = 0; i < vertices.shape(0); ++i)
            m_vertices.emplace_back(v[i * 3], v[i * 3 + 1], v[i * 3 + 2]);

        m_triangles.reserve(triangles.shape(0));
        const uint32_t* t = triangles.data();
        for (size_t i = 0; i < triangles.shape(0); ++i)
            m_triangles.push_back({ static_cast<size_t>(t[i * 3]),
                static_cast<size_t>(t[i * 3 + 1]),
                static_cast<size_t>(t[i * 3 + 2]) });
    }

    size_t targetQuadCount = 50000;
    double scaling = 1.0;
    double adaptivity = 1.0;
    double sharpEdgeDegrees = 90.0;
    double smoothNormalDegrees = 0.0;
    bool hardSurface = false;

    bool run()
    {
        if (m_running.exchange(true))
            throw std::runtime_error("Remesher.run() is already in progress");

        ensureGeogramInitialized();

        AutoRemesher::AutoRemesher remesher(m_vertices, m_triangles);
        // The desktop UI counts quads; the core counts triangles (2 per quad).
        remesher.setTargetTriangleCount(targetQuadCount * 2);
        remesher.setScaling(scaling);
        remesher.setGradientAdaptivity(adaptivity);
        remesher.setSharpEdgeDegrees(sharpEdgeDegrees);
        remesher.setSmoothNormalDegrees(smoothNormalDegrees);
        remesher.setModelType(hardSurface ? AutoRemesher::ModelType::HardSurface
                                          : AutoRemesher::ModelType::Organic);
        remesher.setTag(this);
        remesher.setProgressHandler(&Remesher::progressHandler);

        bool succeed = remesher.remesh();
        if (succeed) {
            m_resultVertices = remesher.remeshedVertices();
            m_resultQuads = remesher.remeshedQuads();
        }
        m_finished = true;
        m_running = false;
        return succeed;
    }

    float progress() const { return m_progress.load(); }

    std::string status() const
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        return m_status;
    }

    VertexArray vertices() const
    {
        requireFinished();
        double* data = new double[m_resultVertices.size() * 3];
        for (size_t i = 0; i < m_resultVertices.size(); ++i) {
            data[i * 3] = m_resultVertices[i].x();
            data[i * 3 + 1] = m_resultVertices[i].y();
            data[i * 3 + 2] = m_resultVertices[i].z();
        }
        nb::capsule owner(data, [](void* p) noexcept { delete[] static_cast<double*>(p); });
        return VertexArray(data, { m_resultVertices.size(), 3 }, owner);
    }

    FaceArray quads() const
    {
        requireFinished();
        uint32_t* data = new uint32_t[m_resultQuads.size() * 4];
        for (size_t i = 0; i < m_resultQuads.size(); ++i)
            for (size_t j = 0; j < 4; ++j)
                data[i * 4 + j] = static_cast<uint32_t>(m_resultQuads[i][j]);
        nb::capsule owner(data, [](void* p) noexcept { delete[] static_cast<uint32_t*>(p); });
        return FaceArray(data, { m_resultQuads.size(), 4 }, owner);
    }

private:
    static void progressHandler(void* tag, float progress, const char* status)
    {
        // Called from the remeshing thread (and TBB workers); must not touch Python.
        Remesher* self = static_cast<Remesher*>(tag);
        self->m_progress.store(progress);
        if (status != nullptr) {
            std::lock_guard<std::mutex> lock(self->m_statusMutex);
            self->m_status = status;
        }
    }

    void requireFinished() const
    {
        if (!m_finished.load())
            throw std::runtime_error("Remesh result is not available until run() has finished");
    }

    std::vector<AutoRemesher::Vector3> m_vertices;
    std::vector<std::vector<size_t>> m_triangles;
    std::vector<AutoRemesher::Vector3> m_resultVertices;
    std::vector<std::vector<size_t>> m_resultQuads;
    std::atomic<float> m_progress { 0.0f };
    std::atomic<bool> m_running { false };
    std::atomic<bool> m_finished { false };
    mutable std::mutex m_statusMutex;
    std::string m_status;
};

} // namespace

NB_MODULE(autoremesher_core, m)
{
    m.doc() = "Automatic quad remeshing (github.com/huxingyi/autoremesher core)";

    nb::class_<Remesher>(m, "Remesher")
        .def(nb::init<nb::ndarray<double, nb::shape<-1, 3>, nb::c_contig, nb::device::cpu>,
                 nb::ndarray<uint32_t, nb::shape<-1, 3>, nb::c_contig, nb::device::cpu>>(),
            nb::arg("vertices"), nb::arg("triangles"),
            "vertices: float64 array of shape (n, 3); triangles: uint32 array of shape (m, 3)")
        .def_rw("target_quad_count", &Remesher::targetQuadCount)
        .def_rw("scaling", &Remesher::scaling)
        .def_rw("adaptivity", &Remesher::adaptivity)
        .def_rw("sharp_edge_degrees", &Remesher::sharpEdgeDegrees)
        .def_rw("smooth_normal_degrees", &Remesher::smoothNormalDegrees)
        .def_rw("hard_surface", &Remesher::hardSurface)
        .def("run", &Remesher::run, nb::call_guard<nb::gil_scoped_release>(),
            "Run remeshing (blocking; releases the GIL). Returns True on success.")
        .def_prop_ro("progress", &Remesher::progress,
            "Progress in [0, 1]; safe to read from another thread while run() is blocking.")
        .def_prop_ro("status", &Remesher::status,
            "Human-readable status of the current stage.")
        .def("vertices", &Remesher::vertices, "Remeshed vertices, float64 (n, 3).")
        .def("quads", &Remesher::quads, "Remeshed quads, uint32 (m, 4).");
}
