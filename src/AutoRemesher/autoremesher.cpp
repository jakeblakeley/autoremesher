/*
 *  Copyright (c) 2020 Jeremy HU <jeremy-at-dust3d dot org>. All rights reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 */
#include <AutoRemesher/AutoRemesher>
#include <AutoRemesher/IsotropicRemesher>
#include <AutoRemesher/MeshSeparator>
#include <AutoRemesher/Parameterizer>
#include <AutoRemesher/QuadExtractor>
#include <atomic>
#include <chrono>
#include <cmath>
#include <geogram_report_progress.h>
#include <iostream>
#include <limits>
#include <queue>
#include <thread>
// Qt defines `emit` as a macro, which collides with TBB profiling.h's `void emit()`.
// macOS `<mach/mach.h>` also defines `emit`. Undefine before including TBB headers.
#if defined(__APPLE__) || defined(emit)
#undef emit
#endif

// oneAPI TBB (2021+) moved headers under <oneapi/tbb/>. Use __has_include where
// available (clang + GCC) to pick the right path, falling back to the legacy path.
#if defined(__has_include)
#if __has_include(<oneapi/tbb/blocked_range.h>)
#include <oneapi/tbb/blocked_range.h>
#else
#include <tbb/blocked_range.h>
#endif
#if __has_include(<oneapi/tbb/mutex.h>)
#include <oneapi/tbb/mutex.h>
#else
#include <tbb/mutex.h>
#endif
#if __has_include(<oneapi/tbb/parallel_for.h>)
#include <oneapi/tbb/parallel_for.h>
#else
#include <tbb/parallel_for.h>
#endif
#else
#include <tbb/blocked_range.h>
#include <tbb/mutex.h>
#include <tbb/parallel_for.h>
#endif
#include <unordered_map>
#include <unordered_set>

thread_local void* geogram_report_progress_tag;
thread_local int geogram_report_progress_round;
thread_local int geogram_report_miq_iter = 0;
thread_local geogram_report_progress_handler geogram_report_progress_callback;

static std::atomic_flag s_geogramProgressLock = ATOMIC_FLAG_INIT;

struct GeogramProgressLockGuard {
    GeogramProgressLockGuard()
    {
        constexpr int kMaxAttempts = 6000;
        int attempts = 0;
        while (s_geogramProgressLock.test_and_set(std::memory_order_acquire)) {
            if (++attempts > kMaxAttempts) {
                std::cerr
                    << "Warning: Geogram progress lock appears abandoned "
                    << "(previous run may have crashed). Recovering."
                    << std::endl;
                s_geogramProgressLock.clear(std::memory_order_release);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                attempts = 0;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    }
    ~GeogramProgressLockGuard()
    {
        s_geogramProgressLock.clear(std::memory_order_release);
    }
};

namespace AutoRemesher {

const double AutoRemesher::m_defaultSharpEdgeDegrees = 90;

double AutoRemesher::calculateAverageEdgeLength(const std::vector<Vector3>& vertices,
    const std::vector<std::vector<size_t>>& faces)
{
    double sumOfLength = 0.0;
    size_t edgeCount = 0;
    for (const auto& face : faces) {
        for (size_t i = 0; i < face.size(); ++i) {
            size_t j = (i + 1) % face.size();
            sumOfLength += (vertices[face[i]] - vertices[face[j]]).length();
            ++edgeCount;
        }
    }
    if (0 == edgeCount)
        return 0.0;
    return sumOfLength / edgeCount;
}

void AutoRemesher::initializeVoxelSize()
{
    double area = calculateMeshArea(m_vertices, m_triangles);
    double triangleArea = area / m_targetTriangleCount;
    m_voxelSize = std::sqrt(triangleArea / (0.86602540378 * 0.5));
#if AUTO_REMESHER_DEBUG
    std::cerr << "Area: " << area << " voxelSize: " << m_voxelSize << std::endl;
#endif
}

double AutoRemesher::calculateMeshArea(const std::vector<Vector3>& vertices,
    const std::vector<std::vector<size_t>>& triangles)
{
    double area = 0.0;
    for (const auto& it : triangles) {
        area += Vector3::area(vertices[it[0]], vertices[it[1]], vertices[it[2]]);
    }
    return area;
}

struct ReportProgressContext {
    size_t islandIndex;
    AutoRemesher* autoRemesher;
};

// A collapsed UV region extracts into a fan of quads converging on one hub
// vertex (valences in the hundreds); legitimate extraordinary vertices stay
// far below this. Also rejects out-of-range indices and non-finite positions.
static bool isIslandResultHealthy(QuadExtractor& extractor)
{
    constexpr size_t kMaxReasonableValence = 50;
    const auto& vertices = extractor.remeshedVertices();
    const auto& quads = extractor.remeshedQuads();
    for (const auto& vertex : vertices) {
        if (!std::isfinite(vertex.x()) || !std::isfinite(vertex.y()) || !std::isfinite(vertex.z())) {
            std::cerr << "Island result rejected: non-finite vertex position" << std::endl;
            return false;
        }
    }
    std::vector<size_t> valence(vertices.size(), 0);
    for (const auto& quad : quads) {
        for (const auto& index : quad) {
            if (index >= vertices.size()) {
                std::cerr << "Island result rejected: quad index out of range" << std::endl;
                return false;
            }
            if (++valence[index] > kMaxReasonableValence) {
                std::cerr << "Island result rejected: vertex valence exceeds "
                          << kMaxReasonableValence << " (degenerate hub)" << std::endl;
                return false;
            }
        }
    }
    return true;
}

static void ReportProgress(void* tag, float progress)
{
    ReportProgressContext* context = (ReportProgressContext*)tag;
    if (nullptr == context)
        return;
#if AUTO_REMESHER_DEBUG
    //std::cerr << "Island[" << context->islandIndex << "]: round(" << geogram_report_progress_round << ") progress(" << (100 * progress) << "%)" << std::endl;
#endif
    static const char* qc_stages[] = {
        "brush + cross-field alignment",
        "singular vertex detection",
        "cut graph construction",
        "constraint building",
        "solver passes 0-1",
        "solver passes 2-3",
        "mixed-integer solve",
        "result extraction"
    };
    int r = geogram_report_progress_round;
    if (r >= 0 && r < 8) {
        context->autoRemesher->setCurrentStatus(
            "Island " + std::to_string(context->islandIndex + 1) + ": " + qc_stages[r]);
    }
    float base = 0.0f;
    float span = 0.0f;
    switch (r) {
    case 0:
        base = 0.0f;
        span = 0.015f;
        break;
    case 1:
        base = 0.015f;
        span = 0.01f;
        break;
    case 2:
        base = 0.025f;
        span = 0.015f;
        break;
    case 3:
        base = 0.04f;
        span = 0.02f;
        break;
    default:
        base = 0.06f;
        span = 0.94f;
        break;
    }
    float totalProgress = 0.3f + 0.6f * (base + span * progress);
    context->autoRemesher->updateProgress(context->islandIndex, totalProgress);
}

void AutoRemesher::resample(std::vector<Vector3>& vertices,
    std::vector<std::vector<size_t>>& triangles,
    double voxelSize,
    double adaptivity,
    double sharpEdgeDegrees,
    double smoothNormalDegrees,
    size_t islandIndex)
{
    std::vector<double> vertexTargetLengths;
    if (adaptivity > 0.0 && !vertices.empty()) {
        const double isoAdaptivity = adaptivity;
        const double minRatio = 0.3;
        const double maxRatio = 3.0;

        std::vector<Vector3> normals(vertices.size());
        tbb::parallel_for(tbb::blocked_range<size_t>(0, triangles.size()),
            [&](const tbb::blocked_range<size_t>& range) {
                for (size_t i = range.begin(); i != range.end(); ++i) {
                    const auto& tri = triangles[i];
                    Vector3 n = Vector3::normal(
                        vertices[tri[0]], vertices[tri[1]], vertices[tri[2]]);
                    normals[tri[0]] += n;
                    normals[tri[1]] += n;
                    normals[tri[2]] += n;
                }
            });
        tbb::parallel_for(tbb::blocked_range<size_t>(0, normals.size()),
            [&](const tbb::blocked_range<size_t>& range) {
                for (size_t i = range.begin(); i != range.end(); ++i)
                    normals[i].normalize();
            });

        std::vector<std::vector<size_t>> faceAroundVertex(vertices.size());
        for (size_t i = 0; i < triangles.size(); ++i) {
            for (size_t j = 0; j < 3; ++j)
                faceAroundVertex[triangles[i][j]].push_back(i);
        }

        std::vector<double> vertexCurvature(vertices.size(), 0.0);
        tbb::parallel_for(tbb::blocked_range<size_t>(0, vertices.size()),
            [&](const tbb::blocked_range<size_t>& range) {
                for (size_t v = range.begin(); v != range.end(); ++v) {
                    const auto& faces = faceAroundVertex[v];
                    if (faces.empty())
                        continue;
                    const auto& normalV = normals[v];
                    double maxCurvature = 0.0;
                    for (const auto& faceIndex : faces) {
                        for (const auto& u : triangles[faceIndex]) {
                            if (u == v)
                                continue;
                            double dist = (vertices[u] - vertices[v]).length();
                            if (dist <= 0.0)
                                continue;
                            double cosA = Vector3::dotProduct(normalV, normals[u]);
                            if (cosA > 1.0)
                                cosA = 1.0;
                            else if (cosA < -1.0)
                                cosA = -1.0;
                            double curv = std::acos(cosA) / dist;
                            if (curv > maxCurvature)
                                maxCurvature = curv;
                        }
                    }
                    vertexCurvature[v] = maxCurvature;
                }
            });

        double sumCurvature = 0.0;
        for (const auto& c : vertexCurvature)
            sumCurvature += c;
        double avgCurvature = sumCurvature / vertexCurvature.size();

        if (avgCurvature > 0.0) {
            vertexTargetLengths.resize(vertices.size());
            tbb::parallel_for(tbb::blocked_range<size_t>(0, vertices.size()),
                [&](const tbb::blocked_range<size_t>& range) {
                    for (size_t v = range.begin(); v != range.end(); ++v) {
                        double normalized = vertexCurvature[v] / avgCurvature;
                        if (normalized < 1e-3)
                            normalized = 1e-3;
                        double multiplier = std::pow(normalized, -isoAdaptivity);
                        if (multiplier < minRatio)
                            multiplier = minRatio;
                        else if (multiplier > maxRatio)
                            multiplier = maxRatio;
                        vertexTargetLengths[v] = voxelSize * multiplier;
                    }
                });
        }
    }

#if AUTO_REMESHER_DEBUG
    std::cerr << "Island[" << islandIndex << "]: Uniformly remeshing on target edge length: " << voxelSize << std::endl;
#endif
    IsotropicRemesher isotropicRemesher(vertices, triangles);
    isotropicRemesher.setTargetEdgeLength(voxelSize);
    if (!vertexTargetLengths.empty())
        isotropicRemesher.setVertexTargetEdgeLengths(&vertexTargetLengths);
    isotropicRemesher.setSharpEdgeDegrees(sharpEdgeDegrees);
    isotropicRemesher.setSmoothNormalDegrees(smoothNormalDegrees);
    isotropicRemesher.remesh();
    vertices = isotropicRemesher.remeshedVertices();
    triangles = isotropicRemesher.remeshedTriangles();
#if AUTO_REMESHER_DEBUG
    std::cerr << "Island[" << islandIndex << "]: Uniformly remesh done, vertex count: " << vertices.size() << " triangle count: " << triangles.size() << std::endl;
#endif
}

void AutoRemesher::updateProgress(size_t threadIndex, float progress)
{
    if (nullptr == m_progressHandler)
        return;

    if (progress > m_threadProgress[threadIndex])
        m_threadProgress[threadIndex] = progress;
    float islandWeightedAvg = 0.0;
    for (size_t i = 0; i < m_threadProgress.size(); ++i)
        islandWeightedAvg += m_threadProgress[i] * m_threadProgressWeights[i];
    std::string statusCopy;
    {
        std::lock_guard<std::mutex> lock(m_currentStatusMutex);
        statusCopy = m_currentStatus;
    }
    m_progressHandler(m_tag, islandWeightedAvg, statusCopy.c_str());
}

bool AutoRemesher::remesh()
{
    geogram_report_progress_tag = nullptr;
    geogram_report_progress_round = 0;
    geogram_report_progress_callback = nullptr;

    if (nullptr != m_progressHandler)
        m_progressHandler(m_tag, 0.0, "Initializing...");

    auto t_start = std::chrono::high_resolution_clock::now();

    // Normalize the input into a canonical ~2-unit box centered at the
    // origin. Parts of the pipeline are not scale-invariant (PositionKey
    // welds by fixed 1e-5 quantization, and the solvers degrade on very
    // small or very large coordinates), so remesh in canonical space and
    // map the result back afterwards.
    Vector3 normalizeOrigin;
    double normalizeFactor = 1.0;
    if (!m_vertices.empty()) {
        Vector3 minPosition = m_vertices.front();
        Vector3 maxPosition = m_vertices.front();
        for (const auto& position : m_vertices) {
            for (size_t i = 0; i < 3; ++i) {
                if (position[i] < minPosition[i])
                    minPosition[i] = position[i];
                if (position[i] > maxPosition[i])
                    maxPosition[i] = position[i];
            }
        }
        normalizeOrigin = (minPosition + maxPosition) * 0.5;
        double maxExtent = std::max(std::max(maxPosition[0] - minPosition[0],
                                        maxPosition[1] - minPosition[1]),
            maxPosition[2] - minPosition[2]);
        if (maxExtent > 0)
            normalizeFactor = 2.0 / maxExtent;
        for (auto& position : m_vertices)
            position = (position - normalizeOrigin) * normalizeFactor;
    }

    auto t_voxelStart = std::chrono::high_resolution_clock::now();
    setCurrentStatus("Computing voxel size...");
    initializeVoxelSize();
    if (nullptr != m_progressHandler)
        m_progressHandler(m_tag, 0.01f, "Computing voxel size...");
    auto t_voxelEnd = std::chrono::high_resolution_clock::now();

    std::vector<std::vector<std::vector<size_t>>> trianglesIslands;
    auto t_splitStart = std::chrono::high_resolution_clock::now();
    setCurrentStatus("Splitting mesh into islands...");
    MeshSeparator::splitToIslands(m_triangles, trianglesIslands);
    if (nullptr != m_progressHandler)
        m_progressHandler(m_tag, 0.02f, "Splitting mesh into islands...");
    auto t_afterSplit = std::chrono::high_resolution_clock::now();

    if (trianglesIslands.empty()) {
        std::cerr << "Input mesh is empty" << std::endl;
        if (nullptr != m_progressHandler)
            m_progressHandler(m_tag, 1.0, "Input mesh is empty");
        return false;
    }

#if AUTO_REMESHER_DEBUG
    std::cerr << "Split to islands: " << trianglesIslands.size() << std::endl;
#endif

    struct IslandContext {
        std::vector<Vector3> vertices;
        std::vector<std::vector<size_t>> triangles;
        double voxelSize;
        double scaling;
        double adaptivity;
        double sharpEdgeDegrees;
        double smoothNormalDegrees;
    };

    std::vector<IslandContext> islandContexes;
    islandContexes.reserve(trianglesIslands.size());
    size_t raisedIslandCount = 0;
    for (size_t islandIndex = 0; islandIndex < trianglesIslands.size(); ++islandIndex) {
        const auto& island = trianglesIslands[islandIndex];
        IslandContext context;
        std::unordered_set<size_t> addedIndices;
        std::unordered_map<size_t, size_t> oldToNewVertexMap;
        for (const auto& face : island) {
            std::vector<size_t> triangle;
            for (size_t i = 0; i < 3; ++i) {
                auto insertResult = addedIndices.insert(face[i]);
                if (insertResult.second) {
                    oldToNewVertexMap.insert({ face[i], context.vertices.size() });
                    context.vertices.push_back(m_vertices[face[i]]);
                }
                triangle.push_back(oldToNewVertexMap[face[i]]);
            }
            context.triangles.push_back(triangle);
        }

        context.scaling = m_scaling;
        context.voxelSize = m_voxelSize;
        context.adaptivity = m_adaptivity;
        context.sharpEdgeDegrees = m_sharpEdgeDegrees;
        context.smoothNormalDegrees = m_smoothNormalDegrees;

        // Detail floor: with one global edge length, islands smaller than a
        // few edge lengths (teeth, spikes) collapse into blobs. Use a finer
        // edge length on such islands so each keeps at least
        // m_minIslandTriangleCount triangles — but never more triangles
        // than it originally had, so micro-debris isn't up-resed.
        if (m_minIslandTriangleCount > 0) {
            size_t floorTriangles = std::min(context.triangles.size(), m_minIslandTriangleCount);
            if (floorTriangles > 0) {
                double islandArea = calculateMeshArea(context.vertices, context.triangles);
                double islandVoxelSize = std::sqrt((islandArea / floorTriangles) / (0.86602540378 * 0.5));
                if (islandVoxelSize > 0 && islandVoxelSize < context.voxelSize) {
                    context.voxelSize = islandVoxelSize;
                    ++raisedIslandCount;
                }
            }
        }

        islandContexes.push_back(context);
    }
    if (raisedIslandCount > 0) {
        std::cerr << "Raised resolution of " << raisedIslandCount
                  << " small island(s) to preserve detail" << std::endl;
    }
    setCurrentStatus("Building island contexts...");
    if (nullptr != m_progressHandler)
        m_progressHandler(m_tag, 0.03f, "Building island contexts...");
    auto t_buildEnd = std::chrono::high_resolution_clock::now();

    {
        m_threadProgressWeights.resize(islandContexes.size(), 1.0);
        for (size_t i = 0; i < islandContexes.size(); ++i) {
            if (!m_triangles.empty())
                m_threadProgressWeights[i] = (float)(((double)islandContexes[i].triangles.size() / m_triangles.size()));
        }
        m_threadProgress.resize(islandContexes.size());

        struct IsotropicPhase {
            IsotropicPhase(std::vector<IslandContext>* contexts,
                AutoRemesher* remesher,
                std::atomic<long long>* resampleTime)
                : m_contexts(contexts)
                , m_remesher(remesher)
                , m_resampleTime(resampleTime)
            {
            }

            void operator()(const tbb::blocked_range<size_t>& range) const
            {
                for (size_t i = range.begin(); i != range.end(); ++i) {
                    auto& ctx = (*m_contexts)[i];

                    m_remesher->setCurrentStatus(
                        "Island " + std::to_string(i + 1) + ": isotropic remeshing...");
                    m_remesher->updateProgress(i, 0.0f);

                    auto t0 = std::chrono::high_resolution_clock::now();
                    resample(ctx.vertices, ctx.triangles, ctx.voxelSize, ctx.adaptivity, ctx.sharpEdgeDegrees, ctx.smoothNormalDegrees, i);
                    auto t1 = std::chrono::high_resolution_clock::now();
                    *m_resampleTime += std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

                    m_remesher->updateProgress(i, 0.3f);
                }
            }

        private:
            std::vector<IslandContext>* m_contexts = nullptr;
            AutoRemesher* m_remesher = nullptr;
            std::atomic<long long>* m_resampleTime = nullptr;
        };

        std::atomic<long long> resampleTime(0);

        tbb::parallel_for(tbb::blocked_range<size_t>(0, islandContexes.size()),
            IsotropicPhase(&islandContexes, this, &resampleTime));
    }

    class ParameterizationThread {
    public:
        ~ParameterizationThread()
        {
            delete parameterizer;
            delete remesher;
        }

        size_t islandIndex = 0;
        IslandContext* island = nullptr;
        Parameterizer* parameterizer = nullptr;
        QuadExtractor* remesher = nullptr;
        AutoRemesher* autoRemesher = nullptr;
    };

    std::vector<ParameterizationThread> parameterizationThreads(islandContexes.size());
    for (size_t i = 0; i < islandContexes.size(); ++i) {
        auto& thread = parameterizationThreads[i];
        auto& context = islandContexes[i];
        thread.islandIndex = i;
        thread.island = &context;
        thread.autoRemesher = this;
    }

    class SurfaceParameterizer {
    public:
        SurfaceParameterizer(std::vector<ParameterizationThread>* parameterizationThreads,
            std::atomic<long long>* parameterizeTime,
            std::atomic<long long>* extractTime)
            : m_parameterizationThreads(parameterizationThreads)
            , m_parameterizeTime(parameterizeTime)
            , m_extractTime(extractTime)
        {
        }

        void operator()(const tbb::blocked_range<size_t>& range) const
        {
            for (size_t i = range.begin(); i != range.end(); ++i) {
                auto& thread = (*m_parameterizationThreads)[i];

                auto t0 = std::chrono::high_resolution_clock::now();

                const auto& vertices = thread.island->vertices;
                const auto& triangles = thread.island->triangles;

                if (vertices.empty() || triangles.empty())
                    continue;

                ReportProgressContext reportProgressContext;
                reportProgressContext.islandIndex = i;
                reportProgressContext.autoRemesher = thread.autoRemesher;
                geogram_report_progress_tag = &reportProgressContext;
                geogram_report_progress_round = 0;
                geogram_report_progress_callback = ReportProgress;

                thread.autoRemesher->setCurrentStatus(
                    "Island " + std::to_string(thread.islandIndex + 1) + ": computing normals & frame field...");
                thread.autoRemesher->updateProgress(thread.islandIndex, 0.3f);

                // On marginal geometry the parameterization can fail an
                // internal geogram assertion, or "succeed" with a collapsed
                // UV region that extracts into a garbage fan of quads around
                // a single hub vertex. Both outcomes are nondeterministic
                // (the solver's parallel reductions change the float rounding
                // per run), so retry the island a few times and validate the
                // result before accepting it; a persistently failing island
                // is skipped rather than failing the whole remesh.
                constexpr int kMaxIslandAttempts = 3;
                auto t1 = t0;
                for (int attempt = 0;; ++attempt) {
                    std::vector<std::vector<Vector2>>* uvs = nullptr;
                    try {
                        thread.parameterizer = new Parameterizer(&vertices,
                            &triangles,
                            nullptr);
                        thread.parameterizer->setScaling(thread.island->scaling);
                        thread.parameterizer->setGradientAdaptivity(thread.island->adaptivity);
                        thread.parameterizer->setSharpEdgeDegrees(thread.island->sharpEdgeDegrees);
                        {
                            GeogramProgressLockGuard lock;
                            thread.parameterizer->parameterize();
                        }

                        t1 = std::chrono::high_resolution_clock::now();

                        thread.autoRemesher->setCurrentStatus(
                            "Island " + std::to_string(thread.islandIndex + 1) + ": extracting quads...");
                        thread.autoRemesher->updateProgress(thread.islandIndex, 0.9f);
                        uvs = thread.parameterizer->takeTriangleUvs();
                        thread.remesher = new QuadExtractor(&vertices,
                            &triangles,
                            uvs);
                        if (!thread.remesher->extract() || !isIslandResultHealthy(*thread.remesher)) {
                            delete thread.remesher;
                            thread.remesher = nullptr;
                        }
                    } catch (const std::exception& e) {
                        std::cerr << "Island " << (thread.islandIndex + 1)
                                  << ": parameterization failed: " << e.what() << std::endl;
                        delete thread.remesher;
                        thread.remesher = nullptr;
                    }
                    delete uvs;
                    if (nullptr != thread.remesher || attempt + 1 >= kMaxIslandAttempts)
                        break;
                    delete thread.parameterizer;
                    thread.parameterizer = nullptr;
                    std::cerr << "Island " << (thread.islandIndex + 1)
                              << ": retrying (attempt " << (attempt + 2) << " of "
                              << kMaxIslandAttempts << ")" << std::endl;
                    thread.autoRemesher->setCurrentStatus(
                        "Island " + std::to_string(thread.islandIndex + 1) + ": retrying...");
                }
                if (nullptr == thread.remesher) {
                    std::cerr << "Island " << (thread.islandIndex + 1)
                              << ": giving up after " << kMaxIslandAttempts
                              << " attempts, skipping" << std::endl;
                }
                *m_parameterizeTime += std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
                thread.autoRemesher->updateProgress(thread.islandIndex, 1.0f);
                thread.autoRemesher->setCurrentStatus(
                    "Island " + std::to_string(thread.islandIndex + 1) + ": done");
                auto t2 = std::chrono::high_resolution_clock::now();
                *m_extractTime += std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();

                // reportProgressContext dies with this iteration, but this
                // worker thread can later steal quad_cover's inner TBB tasks
                // from another island and fire the progress callback through
                // these thread_locals — clear them so a stale (dangling) tag
                // is never dereferenced.
                geogram_report_progress_tag = nullptr;
                geogram_report_progress_callback = nullptr;
            }
        }

    private:
        std::vector<ParameterizationThread>* m_parameterizationThreads = nullptr;
        std::atomic<long long>* m_parameterizeTime = nullptr;
        std::atomic<long long>* m_extractTime = nullptr;
    };
    std::atomic<long long> parameterizeTimeAccumulated(0);
    std::atomic<long long> extractTimeAccumulated(0);

    tbb::parallel_for(tbb::blocked_range<size_t>(0, parameterizationThreads.size()),
        SurfaceParameterizer(&parameterizationThreads,
            &parameterizeTimeAccumulated,
            &extractTimeAccumulated));
    auto t_parallelEnd = std::chrono::high_resolution_clock::now();

    setCurrentStatus("Merging mesh islands...");
    if (nullptr != m_progressHandler)
        m_progressHandler(m_tag, 0.95f, "Merging mesh islands...");
    for (size_t i = 0; i < parameterizationThreads.size(); ++i) {
        auto& thread = parameterizationThreads[i];
        bool remeshed = nullptr != thread.remesher && !thread.remesher->remeshedQuads().empty();
        // An island whose parameterization failed keeps its (isotropically
        // resampled) triangles: a triangulated patch in the result beats a
        // missing piece of the model.
        const auto& vertices = remeshed ? thread.remesher->remeshedVertices()
                                        : thread.island->vertices;
        const auto& faces = remeshed ? thread.remesher->remeshedQuads()
                                     : thread.island->triangles;
        if (faces.empty())
            continue;
        if (!remeshed) {
            std::cerr << "Island " << (thread.islandIndex + 1)
                      << ": keeping original triangles ("
                      << faces.size() << ")" << std::endl;
        }
        size_t vertexStartIndex = m_remeshedVertices.size();
        m_remeshedVertices.reserve(m_remeshedVertices.size() + vertices.size());
        for (const auto& it : vertices) {
            m_remeshedVertices.push_back(it);
        }
        for (const auto& it : faces) {
            std::vector<size_t> face;
            face.reserve(it.size());
            for (const auto& v : it)
                face.push_back(vertexStartIndex + v);
            m_remeshedQuads.push_back(face);
        }
    }

    // Map the result back from canonical space to the input's original
    // position and scale.
    if (normalizeFactor != 0) {
        double invFactor = 1.0 / normalizeFactor;
        for (auto& position : m_remeshedVertices)
            position = position * invFactor + normalizeOrigin;
    }

    auto t_mergeEnd = std::chrono::high_resolution_clock::now();

    auto t_voxelMs = std::chrono::duration_cast<std::chrono::milliseconds>(t_voxelEnd - t_voxelStart).count();
    auto t_splitMs = std::chrono::duration_cast<std::chrono::milliseconds>(t_afterSplit - t_splitStart).count();
    auto t_buildMs = std::chrono::duration_cast<std::chrono::milliseconds>(t_buildEnd - t_afterSplit).count();
    auto t_parallelWallMs = std::chrono::duration_cast<std::chrono::milliseconds>(t_parallelEnd - t_buildEnd).count();
    auto t_mergeMs = std::chrono::duration_cast<std::chrono::milliseconds>(t_mergeEnd - t_parallelEnd).count();
    auto t_totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(t_mergeEnd - t_start).count();

    std::cerr << "Quad mesh breakdown: total " << t_totalMs << " ms"
              << " | voxel " << t_voxelMs << " ms"
              << " | split " << t_splitMs << " ms"
              << " | build " << t_buildMs << " ms"
              << " | parallel " << t_parallelWallMs << " ms"
              << " (param " << parameterizeTimeAccumulated.load() << " ms"
              << " | extract " << extractTimeAccumulated.load() << " ms)"
              << " | merge " << t_mergeMs << " ms" << std::endl;

#if AUTO_REMESHER_DEBUG
    std::cerr << "Remesh done" << std::endl;
#endif

    if (nullptr != m_progressHandler)
        m_progressHandler(m_tag, 1.0, "Done");

    return true;
}

}