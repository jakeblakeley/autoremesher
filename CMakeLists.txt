# Builds the AutoRemesher core as a Python extension module (autoremesher_core)
# for the Blender add-on. The Qt desktop app still builds with qmake
# (autoremesher.pro); this file intentionally covers only the headless core.
cmake_minimum_required(VERSION 3.18)
project(autoremesher_core LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Development.SABIModule is required for nanobind's STABLE_ABI to take
# effect (otherwise it silently builds a version-specific module).
find_package(Python 3.11 COMPONENTS Interpreter Development.Module
    Development.SABIModule REQUIRED)

# --- nanobind (fetched by pip as a build requirement; located via its CMake config)
execute_process(
    COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE OUTPUT_VARIABLE nanobind_ROOT)
find_package(nanobind CONFIG REQUIRED)

# --- oneTBB, built statically so the wheel is self-contained
include(FetchContent)
set(TBB_TEST OFF CACHE BOOL "" FORCE)
set(TBB_STRICT OFF CACHE BOOL "" FORCE)
set(TBB_INSTALL OFF CACHE BOOL "" FORCE)
set(TBB_ENABLE_IPO OFF CACHE BOOL "" FORCE)
set(TBBMALLOC_BUILD OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF)
FetchContent_Declare(TBB
    URL https://github.com/uxlfoundation/oneTBB/archive/refs/tags/v2022.2.0.tar.gz)
FetchContent_MakeAvailable(TBB)

set(GEOGRAM_DIR thirdparty/geogram/geogram-1.8.3/src/lib)

# Source lists ported from autoremesher.pro (the qmake build compiles the same
# curated geogram subset; GEOGRAM_WITH_TRIANGLE/TETGEN stay undefined).
set(GEOGRAM_SOURCES
    ${GEOGRAM_DIR}/geogram/basic/algorithm.cpp
    ${GEOGRAM_DIR}/geogram/basic/assert.cpp
    ${GEOGRAM_DIR}/geogram/basic/attributes.cpp
    ${GEOGRAM_DIR}/geogram/basic/b_stream.cpp
    ${GEOGRAM_DIR}/geogram/basic/command_line.cpp
    ${GEOGRAM_DIR}/geogram/basic/command_line_args.cpp
    ${GEOGRAM_DIR}/geogram/basic/common.cpp
    ${GEOGRAM_DIR}/geogram/basic/counted.cpp
    ${GEOGRAM_DIR}/geogram/basic/environment.cpp
    ${GEOGRAM_DIR}/geogram/basic/factory.cpp
    ${GEOGRAM_DIR}/geogram/basic/file_system.cpp
    ${GEOGRAM_DIR}/geogram/basic/geofile.cpp
    ${GEOGRAM_DIR}/geogram/basic/geometry.cpp
    ${GEOGRAM_DIR}/geogram/basic/line_stream.cpp
    ${GEOGRAM_DIR}/geogram/basic/logger.cpp
    ${GEOGRAM_DIR}/geogram/basic/numeric.cpp
    ${GEOGRAM_DIR}/geogram/basic/packed_arrays.cpp
    ${GEOGRAM_DIR}/geogram/basic/process.cpp
    ${GEOGRAM_DIR}/geogram/basic/process_unix.cpp
    ${GEOGRAM_DIR}/geogram/basic/process_win.cpp
    ${GEOGRAM_DIR}/geogram/basic/progress.cpp
    ${GEOGRAM_DIR}/geogram/basic/quaternion.cpp
    ${GEOGRAM_DIR}/geogram/basic/stopwatch.cpp
    ${GEOGRAM_DIR}/geogram/basic/string.cpp
    ${GEOGRAM_DIR}/geogram/bibliography/bibliography.cpp
    ${GEOGRAM_DIR}/geogram/bibliography/embedded_references.cpp
    ${GEOGRAM_DIR}/geogram/delaunay/LFS.cpp
    ${GEOGRAM_DIR}/geogram/delaunay/delaunay.cpp
    ${GEOGRAM_DIR}/geogram/delaunay/delaunay_2d.cpp
    ${GEOGRAM_DIR}/geogram/delaunay/delaunay_3d.cpp
    ${GEOGRAM_DIR}/geogram/delaunay/delaunay_nn.cpp
    ${GEOGRAM_DIR}/geogram/delaunay/delaunay_tetgen.cpp
    ${GEOGRAM_DIR}/geogram/delaunay/delaunay_triangle.cpp
    ${GEOGRAM_DIR}/geogram/delaunay/parallel_delaunay_3d.cpp
    ${GEOGRAM_DIR}/geogram/delaunay/periodic.cpp
    ${GEOGRAM_DIR}/geogram/delaunay/periodic_delaunay_3d.cpp
    ${GEOGRAM_DIR}/geogram/image/colormap.cpp
    ${GEOGRAM_DIR}/geogram/image/image.cpp
    ${GEOGRAM_DIR}/geogram/image/image_library.cpp
    ${GEOGRAM_DIR}/geogram/image/image_rasterizer.cpp
    ${GEOGRAM_DIR}/geogram/image/image_serializer.cpp
    ${GEOGRAM_DIR}/geogram/image/image_serializer_pgm.cpp
    ${GEOGRAM_DIR}/geogram/image/image_serializer_stb.cpp
    ${GEOGRAM_DIR}/geogram/image/image_serializer_xpm.cpp
    ${GEOGRAM_DIR}/geogram/image/morpho_math.cpp
    ${GEOGRAM_DIR}/geogram/mesh/mesh.cpp
    ${GEOGRAM_DIR}/geogram/mesh/mesh_AABB.cpp
    ${GEOGRAM_DIR}/geogram/mesh/mesh_fill_holes.cpp
    ${GEOGRAM_DIR}/geogram/mesh/mesh_frame_field.cpp
    ${GEOGRAM_DIR}/geogram/mesh/mesh_geometry.cpp
    ${GEOGRAM_DIR}/geogram/mesh/mesh_halfedges.cpp
    ${GEOGRAM_DIR}/geogram/mesh/mesh_io.cpp
    ${GEOGRAM_DIR}/geogram/mesh/mesh_partition.cpp
    ${GEOGRAM_DIR}/geogram/mesh/mesh_preprocessing.cpp
    ${GEOGRAM_DIR}/geogram/mesh/mesh_reorder.cpp
    ${GEOGRAM_DIR}/geogram/mesh/mesh_repair.cpp
    ${GEOGRAM_DIR}/geogram/mesh/mesh_topology.cpp
    ${GEOGRAM_DIR}/geogram/numerics/expansion_nt.cpp
    ${GEOGRAM_DIR}/geogram/numerics/lbfgs_optimizers.cpp
    ${GEOGRAM_DIR}/geogram/numerics/matrix_util.cpp
    ${GEOGRAM_DIR}/geogram/numerics/multi_precision.cpp
    ${GEOGRAM_DIR}/geogram/numerics/optimizer.cpp
    ${GEOGRAM_DIR}/geogram/numerics/predicates.cpp
    ${GEOGRAM_DIR}/geogram/parameterization/mesh_global_param.cpp
    ${GEOGRAM_DIR}/geogram/points/co3ne.cpp
    ${GEOGRAM_DIR}/geogram/points/colocate.cpp
    ${GEOGRAM_DIR}/geogram/points/kd_tree.cpp
    ${GEOGRAM_DIR}/geogram/points/nn_search.cpp
    ${GEOGRAM_DIR}/geogram/points/principal_axes.cpp
    ${GEOGRAM_DIR}/geogram/voronoi/CVT.cpp
    ${GEOGRAM_DIR}/geogram/voronoi/RVD.cpp
    ${GEOGRAM_DIR}/geogram/voronoi/RVD_callback.cpp
    ${GEOGRAM_DIR}/geogram/voronoi/RVD_mesh_builder.cpp
    ${GEOGRAM_DIR}/geogram/voronoi/convex_cell.cpp
    ${GEOGRAM_DIR}/geogram/voronoi/generic_RVD_cell.cpp
    ${GEOGRAM_DIR}/geogram/voronoi/generic_RVD_polygon.cpp
    ${GEOGRAM_DIR}/geogram/voronoi/integration_simplex.cpp
    ${GEOGRAM_DIR}/exploragram/hexdom/basic.cpp
    ${GEOGRAM_DIR}/exploragram/hexdom/frame.cpp
    ${GEOGRAM_DIR}/exploragram/hexdom/spherical_harmonics_l4.cpp
    ${GEOGRAM_DIR}/exploragram/hexdom/polygon.cpp
    ${GEOGRAM_DIR}/exploragram/hexdom/quad_cover.cpp
    ${GEOGRAM_DIR}/geogram/NL/nl_api.c
    ${GEOGRAM_DIR}/geogram/NL/nl_matrix.c
    ${GEOGRAM_DIR}/geogram/NL/nl_context.c
    ${GEOGRAM_DIR}/geogram/NL/nl_blas.c
    ${GEOGRAM_DIR}/geogram/NL/nl_arpack.c
    ${GEOGRAM_DIR}/geogram/NL/nl_iterative_solvers.c
    ${GEOGRAM_DIR}/geogram/NL/nl_preconditioners.c
    ${GEOGRAM_DIR}/geogram/NL/nl_os.c
    ${GEOGRAM_DIR}/geogram/third_party/libMeshb/sources/libmeshb7.c
    ${GEOGRAM_DIR}/geogram/third_party/rply/rply.c
)

set(ZLIB_SOURCES
    ${GEOGRAM_DIR}/geogram/third_party/zlib/adler32.c
    ${GEOGRAM_DIR}/geogram/third_party/zlib/compress.c
    ${GEOGRAM_DIR}/geogram/third_party/zlib/crc32.c
    ${GEOGRAM_DIR}/geogram/third_party/zlib/deflate.c
    ${GEOGRAM_DIR}/geogram/third_party/zlib/gzclose.c
    ${GEOGRAM_DIR}/geogram/third_party/zlib/gzlib.c
    ${GEOGRAM_DIR}/geogram/third_party/zlib/gzread.c
    ${GEOGRAM_DIR}/geogram/third_party/zlib/gzwrite.c
    ${GEOGRAM_DIR}/geogram/third_party/zlib/inffast.c
    ${GEOGRAM_DIR}/geogram/third_party/zlib/inflate.c
    ${GEOGRAM_DIR}/geogram/third_party/zlib/inftrees.c
    ${GEOGRAM_DIR}/geogram/third_party/zlib/trees.c
    ${GEOGRAM_DIR}/geogram/third_party/zlib/uncompr.c
    ${GEOGRAM_DIR}/geogram/third_party/zlib/zutil.c
)

set(CORE_SOURCES
    src/AutoRemesher/autoremesher.cpp
    src/AutoRemesher/isotropicremesher.cpp
    src/AutoRemesher/parameterizer.cpp
    src/AutoRemesher/quadextractor.cpp
    src/AutoRemesher/positionkey.cpp
    src/AutoRemesher/meshseparator.cpp
    src/AutoRemesher/nl_ext_stubs.c
    thirdparty/isotropicremesher/isotropicremesher.cpp
    thirdparty/isotropicremesher/isotropichalfedgemesh.cpp
    thirdparty/isotropicremesher/axisalignedboundingboxtree.cpp
)

# Vendored zlib only on Windows (as in autoremesher.pro); its old zutil.h
# breaks against modern macOS SDKs, and unix systems ship zlib anyway.
if(WIN32)
    list(APPEND GEOGRAM_SOURCES ${ZLIB_SOURCES})
endif()

add_library(autoremesher_static STATIC
    ${CORE_SOURCES} ${GEOGRAM_SOURCES})
target_compile_features(autoremesher_static PUBLIC cxx_std_14)
target_include_directories(autoremesher_static PUBLIC
    include
    thirdparty/eigen
    thirdparty/isotropicremesher
    thirdparty/geogram
    ${GEOGRAM_DIR}
    ${GEOGRAM_DIR}/geogram/third_party/libMeshb/sources
    ${GEOGRAM_DIR}/geogram/NL
)
target_compile_definitions(autoremesher_static PUBLIC
    _USE_MATH_DEFINES NOMINMAX EIGEN_MPL2_ONLY)
target_link_libraries(autoremesher_static PUBLIC TBB::tbb)
set_target_properties(autoremesher_static PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(WIN32)
    # Static oneTBB: disable the '#pragma comment(lib, "tbb12")' auto-link.
    target_compile_definitions(autoremesher_static PUBLIC __TBB_NO_IMPLICIT_LINKAGE=1)
    target_compile_options(autoremesher_static PRIVATE /bigobj)
    target_link_libraries(autoremesher_static PUBLIC advapi32 shell32 user32)
else()
    target_compile_options(autoremesher_static PRIVATE -O3 -funroll-loops)
    target_link_libraries(autoremesher_static PUBLIC z)
endif()

nanobind_add_module(autoremesher_core STABLE_ABI src/python/binding.cpp)
target_link_libraries(autoremesher_core PRIVATE autoremesher_static)

install(TARGETS autoremesher_core LIBRARY DESTINATION .)
