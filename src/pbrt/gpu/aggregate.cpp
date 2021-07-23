// pbrt is Copyright(c) 1998-2020 Matt Pharr, Wenzel Jakob, and Greg Humphreys.
// The pbrt source code is licensed under the Apache License, Version 2.0.
// SPDX: Apache-2.0

#include <pbrt/gpu/aggregate.h>

#include <pbrt/gpu/optix.h>
#include <pbrt/gpu/util.h>
#include <pbrt/lights.h>
#include <pbrt/materials.h>
#include <pbrt/parsedscene.h>
#include <pbrt/textures.h>
#include <pbrt/util/error.h>
#include <pbrt/util/file.h>
#include <pbrt/util/log.h>
#include <pbrt/util/loopsubdiv.h>
#include <pbrt/util/mesh.h>
#include <pbrt/util/parallel.h>
#include <pbrt/util/pstd.h>
#include <pbrt/util/splines.h>
#include <pbrt/util/stats.h>
#include <pbrt/wavefront/intersect.h>

#include <atomic>
#include <mutex>

#include <optix.h>
#include <optix_function_table_definition.h>
#include <optix_stubs.h>

#ifdef NVTX
#include <nvtx3/nvToolsExt.h>
#endif

#define OPTIX_CHECK(EXPR)                                                           \
    do {                                                                            \
        OptixResult res = EXPR;                                                     \
        if (res != OPTIX_SUCCESS)                                                   \
            LOG_FATAL("OptiX call " #EXPR " failed with code %d: \"%s\"", int(res), \
                      optixGetErrorString(res));                                    \
    } while (false) /* eat semicolon */

#define OPTIX_CHECK_WITH_LOG(EXPR, LOG)                                             \
    do {                                                                            \
        OptixResult res = EXPR;                                                     \
        if (res != OPTIX_SUCCESS)                                                   \
            LOG_FATAL("OptiX call " #EXPR " failed with code %d: \"%s\"\nLogs: %s", \
                      int(res), optixGetErrorString(res), LOG);                     \
    } while (false) /* eat semicolon */

namespace pbrt {

OptiXAggregate::ASBuildInput::ASBuildInput(size_t size) {
    optixInputs.resize(size);
    intersectHGRecords.resize(size);
    shadowHGRecords.resize(size);
    randomHitHGRecords.resize(size);
}

// FIXME: copied in wavefront/aggregate.cpp
static void updateMaterialNeeds(Material m, pstd::array<bool, Material::NumTags()> *haveBasicEvalMaterial,
                                pstd::array<bool, Material::NumTags()> *haveUniversalEvalMaterial,
                                bool *haveSubsurface) {
    if (!m)
        return;

    if (MixMaterial *mix = m.CastOrNullptr<MixMaterial>(); mix) {
        updateMaterialNeeds(mix->GetMaterial(0), haveBasicEvalMaterial, haveUniversalEvalMaterial,
                            haveSubsurface);
        updateMaterialNeeds(mix->GetMaterial(1), haveBasicEvalMaterial, haveUniversalEvalMaterial,
                            haveSubsurface);
        return;
    }

    *haveSubsurface |= m.HasSubsurfaceScattering();

    FloatTexture displace = m.GetDisplacement();
    if (m.CanEvaluateTextures(BasicTextureEvaluator()) &&
        (!displace || BasicTextureEvaluator().CanEvaluate({displace}, {})))
        (*haveBasicEvalMaterial)[m.Tag()] = true;
    else
        (*haveUniversalEvalMaterial)[m.Tag()] = true;
}

struct __align__(OPTIX_SBT_RECORD_ALIGNMENT) RaygenRecord {
    __align__(OPTIX_SBT_RECORD_ALIGNMENT) char header[OPTIX_SBT_RECORD_HEADER_SIZE];
};

struct __align__(OPTIX_SBT_RECORD_ALIGNMENT) MissRecord {
    __align__(OPTIX_SBT_RECORD_ALIGNMENT) char header[OPTIX_SBT_RECORD_HEADER_SIZE];
};

struct __align__(OPTIX_SBT_RECORD_ALIGNMENT) OptiXAggregate::HitgroupRecord {
    HitgroupRecord() {}
    HitgroupRecord(const HitgroupRecord &r) { memcpy(this, &r, sizeof(HitgroupRecord)); }
    HitgroupRecord &operator=(const HitgroupRecord &r) {
        if (this != &r)
            memcpy(this, &r, sizeof(HitgroupRecord));
        return *this;
    }

    __align__(OPTIX_SBT_RECORD_ALIGNMENT) char header[OPTIX_SBT_RECORD_HEADER_SIZE];
    union {
        TriangleMeshRecord triRec;
        BilinearMeshRecord bilinearRec;
        QuadricRecord quadricRec;
    };
};

extern "C" {
extern const unsigned char PBRT_EMBEDDED_PTX[];
}

STAT_MEMORY_COUNTER("Memory/Acceleration structures", gpuBVHBytes);

OptixTraversableHandle OptiXAggregate::buildBVH(
    const std::vector<OptixBuildInput> &buildInputs) {
    if (buildInputs.empty())
        return {};

    // Figure out memory requirements.
    OptixAccelBuildOptions accelOptions = {};
    accelOptions.buildFlags =
        (OPTIX_BUILD_FLAG_ALLOW_COMPACTION | OPTIX_BUILD_FLAG_PREFER_FAST_TRACE);
    accelOptions.motionOptions.numKeys = 1;
    accelOptions.operation = OPTIX_BUILD_OPERATION_BUILD;

    OptixAccelBufferSizes blasBufferSizes;
    OPTIX_CHECK(optixAccelComputeMemoryUsage(optixContext, &accelOptions,
                                             buildInputs.data(), buildInputs.size(),
                                             &blasBufferSizes));

    uint64_t *compactedSizeBufferPtr = alloc.new_object<uint64_t>();
    OptixAccelEmitDesc emitDesc;
    emitDesc.type = OPTIX_PROPERTY_TYPE_COMPACTED_SIZE;
    emitDesc.result = (CUdeviceptr)compactedSizeBufferPtr;

    // Allocate buffers.
    void *tempBuffer;
    CUDA_CHECK(cudaMalloc(&tempBuffer, blasBufferSizes.tempSizeInBytes));
    void *outputBuffer;
    CUDA_CHECK(cudaMalloc(&outputBuffer, blasBufferSizes.outputSizeInBytes));

    // Build.
    OptixTraversableHandle traversableHandle{0};
    OPTIX_CHECK(optixAccelBuild(
        optixContext, cudaStream, &accelOptions, buildInputs.data(), buildInputs.size(),
        CUdeviceptr(tempBuffer), blasBufferSizes.tempSizeInBytes,
        CUdeviceptr(outputBuffer), blasBufferSizes.outputSizeInBytes, &traversableHandle,
        &emitDesc, 1));

    CUDA_CHECK(cudaDeviceSynchronize());

    gpuBVHBytes += *compactedSizeBufferPtr;

    // Compact
    void *asBuffer;
    CUDA_CHECK(cudaMalloc(&asBuffer, *compactedSizeBufferPtr));

    OPTIX_CHECK(optixAccelCompact(optixContext, cudaStream, traversableHandle,
                                  CUdeviceptr(asBuffer), *compactedSizeBufferPtr,
                                  &traversableHandle));
    CUDA_CHECK(cudaDeviceSynchronize());

    CUDA_CHECK(cudaFree(tempBuffer));
    CUDA_CHECK(cudaFree(outputBuffer));
    alloc.delete_object(compactedSizeBufferPtr);

    return traversableHandle;
}

static Material getMaterial(
    const ShapeSceneEntity &shape,
    const std::map<std::string, Material> &namedMaterials,
    const std::vector<Material> &materials) {
    if (!shape.materialName.empty()) {
        auto iter = namedMaterials.find(shape.materialName);
        if (iter == namedMaterials.end())
            ErrorExit(&shape.loc, "%s: material not defined", shape.materialName);
        return iter->second;
    } else {
        CHECK_NE(shape.materialIndex, -1);
        return materials[shape.materialIndex];
    }
}

static FloatTexture getAlphaTexture(
    const ShapeSceneEntity &shape,
    const std::map<std::string, FloatTexture> &floatTextures,
    Allocator alloc) {
    FloatTexture alphaTexture;

    std::string alphaTexName = shape.parameters.GetTexture("alpha");
    if (alphaTexName.empty()) {
        if (Float alpha = shape.parameters.GetOneFloat("alpha", 1.f); alpha < 1.f)
            alphaTexture = alloc.new_object<FloatConstantTexture>(alpha);
        else
            return nullptr;
    } else {
        auto iter = floatTextures.find(alphaTexName);
        if (iter == floatTextures.end())
            ErrorExit(&shape.loc, "%s: alpha texture not defined.", alphaTexName);

        alphaTexture = iter->second;
    }

    if (!BasicTextureEvaluator().CanEvaluate({alphaTexture}, {})) {
        // It would be nice to just use the UniversalTextureEvaluator (maybe
        // always), but optix complains "Error: Found call graph recursion"...
        Warning(&shape.loc,
                "%s: alpha texture too complex for BasicTextureEvaluator "
                "(need fallback path). Ignoring for now.",
                alphaTexName);
        alphaTexture = nullptr;
    }

    return alphaTexture;
}

static int getOptixGeometryFlags(bool isTriangle, FloatTexture alphaTexture,
                                 Material material) {
    if (alphaTexture && isTriangle)
        // Need anyhit
        return OPTIX_GEOMETRY_FLAG_NONE;
    else
        return OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT;
}

static MediumInterface *getMediumInterface(
    const ShapeSceneEntity &shape, const std::map<std::string, Medium> &media,
    Allocator alloc) {
    if (shape.insideMedium.empty() && shape.outsideMedium.empty())
        return nullptr;

    auto getMedium = [&](const std::string &name) -> Medium {
        if (name.empty())
            return nullptr;

        auto iter = media.find(name);
        if (iter == media.end())
            ErrorExit(&shape.loc, "%s: medium not defined", name);
        return iter->second;
    };

    return alloc.new_object<MediumInterface>(getMedium(shape.insideMedium),
                                             getMedium(shape.outsideMedium));
}

STAT_COUNTER("Geometry/Triangles added from displacement mapping", displacedTrisDelta);

std::map<int, TriQuadMesh>
OptiXAggregate::PreparePLYMeshes(const std::vector<ShapeSceneEntity> &shapes,
                                 const std::map<std::string, FloatTexture> &floatTextures) const {
    std::map<int, TriQuadMesh> plyMeshes;
    std::mutex mutex;
    ParallelFor(0, shapes.size(), [&](int64_t i) {
        const auto &shape = shapes[i];
        if (shape.name != "plymesh")
            return;

        std::string filename =
            ResolveFilename(shape.parameters.GetOneString("filename", ""));
        if (filename.empty())
            ErrorExit(&shape.loc, "plymesh: \"filename\" must be provided.");
        TriQuadMesh plyMesh = TriQuadMesh::ReadPLY(filename);  // todo: alloc
        if (!plyMesh.triIndices.empty() || !plyMesh.quadIndices.empty()) {
            plyMesh.ConvertToOnlyTriangles();

            Float edgeLength = shape.parameters.GetOneFloat("displacement.edgelength", 1.f);
            edgeLength *= Options->displacementEdgeScale;

            std::string displacementTexName = shape.parameters.GetTexture("displacement");
            if (!displacementTexName.empty()) {
                auto iter = floatTextures.find(displacementTexName);
                if (iter == floatTextures.end())
                    ErrorExit(&shape.loc, "%s: no such texture defined.", displacementTexName);
                FloatTexture displacement = iter->second;

                LOG_VERBOSE("Starting to displace mesh \"%s\" with \"%s\"", filename,
                            displacementTexName);

                size_t origNumTris = plyMesh.triIndices.size() / 3;

                plyMesh =
                    plyMesh.Displace([&](Point3f v0, Point3f v1) {
                        v0 = (*shape.renderFromObject)(v0);
                        v1 = (*shape.renderFromObject)(v1);
                        return Distance(v0, v1);
                    }, edgeLength,
                        [&](Point3f *pCPU, const Normal3f *nCPU, const Point2f *uvCPU, int nVertices) {
                            Point3f *p;
                            Normal3f *n;
                            Point2f *uv;
                            CUDA_CHECK(cudaMallocManaged(&p, nVertices * sizeof(Point3f)));
                            CUDA_CHECK(cudaMallocManaged(&n, nVertices * sizeof(Normal3f)));
                            CUDA_CHECK(cudaMallocManaged(&uv, nVertices * sizeof(Point2f)));

                            std::memcpy(p, pCPU, nVertices * sizeof(Point3f));
                            std::memcpy(n, nCPU, nVertices * sizeof(Normal3f));
                            std::memcpy(uv, uvCPU, nVertices * sizeof(Point2f));

                            GPUParallelFor("Evaluate Displacement", nVertices,
                                           [=] PBRT_GPU (int i) {
                                               TextureEvalContext ctx;
                                               ctx.p = p[i];
                                               ctx.uv = uv[i];
                                               Float d = UniversalTextureEvaluator()(displacement, ctx);
                                               p[i] += Vector3f(d * n[i]);
                                           });
                            GPUWait();

                            std::memcpy(pCPU, p, nVertices * sizeof(Point3f));

                            CUDA_CHECK(cudaFree(p));
                            CUDA_CHECK(cudaFree(n));
                            CUDA_CHECK(cudaFree(uv));
                        }, &shape.loc);

                displacedTrisDelta += plyMesh.triIndices.size() / 3 - origNumTris;

                LOG_VERBOSE("Finished displacing mesh \"%s\" with \"%s\" -> %d tris", filename,
                            displacementTexName, plyMesh.triIndices.size() / 3);
            }
        }

        std::lock_guard<std::mutex> lock(mutex);
        plyMeshes[i] = std::move(plyMesh);
    });

    return plyMeshes;
}

OptiXAggregate::ASBuildInput OptiXAggregate::createBuildInputForTriangles(
    const std::vector<ShapeSceneEntity> &shapes,
    const std::map<int, TriQuadMesh> &plyMeshes,
    const OptixProgramGroup &intersectPG,
    const OptixProgramGroup &shadowPG, const OptixProgramGroup &randomHitPG,
    const std::map<std::string, FloatTexture> &floatTextures,
    const std::map<std::string, Material> &namedMaterials,
    const std::vector<Material> &materials,
    const std::map<std::string, Medium> &media,
    const std::map<int, pstd::vector<Light> *> &shapeIndexToAreaLights,
    Allocator alloc) {
    // Allocate space for potentially all shapes being triangle meshes so
    // that we can write them in order (just potentially sparsely...)
    std::vector<TriangleMesh *> meshes(shapes.size(), nullptr);
    std::vector<Bounds3f> meshBounds(shapes.size());
    std::atomic<int> meshesCreated{0};

    ParallelFor(0, shapes.size(), [&](int64_t shapeIndex) {
        const auto &shape = shapes[shapeIndex];
        if (shape.name == "trianglemesh" || shape.name == "plymesh" ||
            shape.name == "loopsubdiv") {
            TriangleMesh *mesh = nullptr;
            if (shape.name == "trianglemesh") {
                mesh =
                    Triangle::CreateMesh(shape.renderFromObject, shape.reverseOrientation,
                                         shape.parameters, &shape.loc, alloc);
                CHECK(mesh != nullptr);
            } else if (shape.name == "loopsubdiv") {
                // Copied from pbrt/shapes.cpp... :-p
                int nLevels = shape.parameters.GetOneInt("levels", 3);
                std::vector<int> vertexIndices = shape.parameters.GetIntArray("indices");
                if (vertexIndices.empty())
                    ErrorExit(&shape.loc, "Vertex indices \"indices\" not "
                                          "provided for LoopSubdiv shape.");

                std::vector<Point3f> P = shape.parameters.GetPoint3fArray("P");
                if (P.empty())
                    ErrorExit(&shape.loc, "Vertex positions \"P\" not provided "
                                          "for LoopSubdiv shape.");

                // don't actually use this for now...
                std::string scheme = shape.parameters.GetOneString("scheme", "loop");

                mesh = LoopSubdivide(shape.renderFromObject, shape.reverseOrientation,
                                     nLevels, vertexIndices, P, alloc);
                CHECK(mesh != nullptr);
            } else {
                CHECK_EQ(shape.name, "plymesh");
                auto plyIter = plyMeshes.find(shapeIndex);
                CHECK(plyIter != plyMeshes.end());
                const TriQuadMesh &plyMesh = plyIter->second;

                if (!plyMesh.quadIndices.empty() && shape.lightIndex != -1) {
#if 0
                    // If you'd like to know what they are...
                    for (int i = 0; i < plyMesh.quadIndices.size(); ++i)
                        Printf("%s\n", plyMesh.p[plyMesh.quadIndices[i]]);
#endif
                    // This would be nice to fix, but it involves some
                    // plumbing and it's a rare case. The underlying issue
                    // is that when we create AreaLights for emissive
                    // shapes earlier, we're not expecting this..
                    std::string filename =
                        ResolveFilename(shape.parameters.GetOneString("filename", ""));
                    ErrorExit(&shape.loc, "%s: PLY file being used as an area light has quads--"
                              "this is currently unsupported. Please replace them with \"bilinearmesh\" "
                              "shapes as a workaround. (Sorry!).", filename);
                }

                mesh = alloc.new_object<TriangleMesh>(
                    *shape.renderFromObject, shape.reverseOrientation, plyMesh.triIndices,
                    plyMesh.p, std::vector<Vector3f>(), plyMesh.n, plyMesh.uv,
                    plyMesh.faceIndices);
            }

            Bounds3f bounds;
            for (size_t i = 0; i < mesh->nVertices; ++i)
                bounds = Union(bounds, mesh->p[i]);

            meshes[shapeIndex] = mesh;
            meshBounds[shapeIndex] = bounds;
            ++meshesCreated;
        }
    });

    if (meshesCreated.load() == 0)
        return {};

    ASBuildInput buildInput(meshesCreated.load());
    // FIXME: these leak, unfortunately...
    CUdeviceptr *pDeviceDevicePtrs = new CUdeviceptr[meshesCreated.load()];
    uint32_t *triangleInputFlags = new uint32_t[meshesCreated.load()];

    int buildIndex = 0;
    for (int shapeIndex = 0; shapeIndex < meshes.size(); ++shapeIndex) {
        TriangleMesh *mesh = meshes[shapeIndex];
        if (!mesh)
            continue;

        const auto &shape = shapes[shapeIndex];

        FloatTexture alphaTexture = getAlphaTexture(shape, floatTextures, alloc);
        Material material = getMaterial(shape, namedMaterials, materials);

        OptixBuildInput input = {};

        input.type = OPTIX_BUILD_INPUT_TYPE_TRIANGLES;

        input.triangleArray.vertexFormat = OPTIX_VERTEX_FORMAT_FLOAT3;
        input.triangleArray.vertexStrideInBytes = sizeof(Point3f);
        input.triangleArray.numVertices = mesh->nVertices;
        pDeviceDevicePtrs[buildIndex] = CUdeviceptr(mesh->p);
        input.triangleArray.vertexBuffers = &pDeviceDevicePtrs[buildIndex];

        input.triangleArray.indexFormat = OPTIX_INDICES_FORMAT_UNSIGNED_INT3;
        input.triangleArray.indexStrideInBytes = 3 * sizeof(int);
        input.triangleArray.numIndexTriplets = mesh->nTriangles;
        input.triangleArray.indexBuffer = CUdeviceptr(mesh->vertexIndices);

        triangleInputFlags[buildIndex] =
            getOptixGeometryFlags(true, alphaTexture, material);
        input.triangleArray.flags = &triangleInputFlags[buildIndex];

        input.triangleArray.numSbtRecords = 1;
        input.triangleArray.sbtIndexOffsetBuffer = CUdeviceptr(nullptr);
        input.triangleArray.sbtIndexOffsetSizeInBytes = 0;
        input.triangleArray.sbtIndexOffsetStrideInBytes = 0;

        buildInput.optixInputs[buildIndex] = input;

        HitgroupRecord hgRecord;
        OPTIX_CHECK(optixSbtRecordPackHeader(intersectPG, &hgRecord));
        hgRecord.triRec.mesh = mesh;
        hgRecord.triRec.material = material;
        hgRecord.triRec.alphaTexture = alphaTexture;
        hgRecord.triRec.areaLights = {};
        if (shape.lightIndex != -1) {
            if (!material)
                Warning(&shape.loc, "Ignoring area light specification for shape with \"interface\" material.");
            else {
                // Note: this will hit if we try to have an instance as an area
                // light.
                auto iter = shapeIndexToAreaLights.find(shapeIndex);
                CHECK(iter != shapeIndexToAreaLights.end());
                CHECK_EQ(iter->second->size(), mesh->nTriangles);
                hgRecord.triRec.areaLights = pstd::MakeSpan(*iter->second);
            }
        }
        hgRecord.triRec.mediumInterface = getMediumInterface(shape, media, alloc);

        buildInput.bounds = Union(buildInput.bounds, meshBounds[shapeIndex]);

        buildInput.intersectHGRecords[buildIndex] = hgRecord;

        OPTIX_CHECK(optixSbtRecordPackHeader(randomHitPG, &hgRecord));
        buildInput.randomHitHGRecords[buildIndex]= hgRecord;

        OPTIX_CHECK(optixSbtRecordPackHeader(shadowPG, &hgRecord));
        buildInput.shadowHGRecords[buildIndex] = hgRecord;

        ++buildIndex;
    }

    return buildInput;
}

STAT_COUNTER("Geometry/Curves", nCurves);
STAT_COUNTER("Geometry/Bilinear patches created for diced curves", nBLPsForCurves);

BilinearPatchMesh *OptiXAggregate::diceCurveToBLP(const ShapeSceneEntity &shape,
                                                  int nDiceU, int nDiceV,
                                                  Allocator alloc) {
    CHECK_EQ("curve", shape.name);
    const ParameterDictionary &parameters = shape.parameters;
    const FileLoc *loc = &shape.loc;

    ++nCurves;

    // Extract parameters; the following ~90 lines of code are,
    // unfortunately, copied from Curve::Create.  We would like to avoid
    // the overhead of splitting the curve and creating Curve objects, so
    // here we go..
    Float width = parameters.GetOneFloat("width", 1.f);
    Float width0 = parameters.GetOneFloat("width0", width);
    Float width1 = parameters.GetOneFloat("width1", width);

    int degree = parameters.GetOneInt("degree", 3);
    if (degree != 2 && degree != 3) {
        Error(loc, "Invalid degree %d: only degree 2 and 3 curves are supported.",
              degree);
        return {};
    }

    std::string basis = parameters.GetOneString("basis", "bezier");
    if (basis != "bezier" && basis != "bspline") {
        Error(loc,
              "Invalid basis \"%s\": only \"bezier\" and \"bspline\" are "
              "supported.",
              basis);
        return {};
    }

    int nSegments;
    std::vector<Point3f> cp = parameters.GetPoint3fArray("P");
    bool bezierBasis = (basis == "bezier");
    if (bezierBasis) {
        // After the first segment, which uses degree+1 control points,
        // subsequent segments reuse the last control point of the previous
        // one and then use degree more control points.
        if (((cp.size() - 1 - degree) % degree) != 0) {
            Error(loc,
                  "Invalid number of control points %d: for the degree %d "
                  "Bezier basis %d + n * %d are required, for n >= 0.",
                  (int)cp.size(), degree, degree + 1, degree);
            return {};
        }
        nSegments = (cp.size() - 1) / degree;
    } else {
        if (cp.size() < degree + 1) {
            Error(loc,
                  "Invalid number of control points %d: for the degree %d "
                  "b-spline basis, must have >= %d.",
                  int(cp.size()), degree, degree + 1);
            return {};
        }
        nSegments = cp.size() - degree;
    }

    CurveType type;
    std::string curveType = parameters.GetOneString("type", "flat");
    if (curveType == "flat")
        type = CurveType::Flat;
    else if (curveType == "ribbon")
        type = CurveType::Ribbon;
    else if (curveType == "cylinder")
        type = CurveType::Cylinder;
    else {
        Error(loc, R"(Unknown curve type "%s".  Using "cylinder".)", curveType);
        type = CurveType::Cylinder;
    }

    std::vector<Normal3f> n = parameters.GetNormal3fArray("N");
    if (!n.empty()) {
        if (type != CurveType::Ribbon) {
            Warning("Curve normals are only used with \"ribbon\" type curves.");
            n = {};
        } else if (n.size() != nSegments + 1) {
            Error(loc,
                  "Invalid number of normals %d: must provide %d normals for "
                  "ribbon "
                  "curves with %d segments.",
                  int(n.size()), nSegments + 1, nSegments);
            return {};
        }
        for (Normal3f &nn : n)
            Normalize(nn);
    } else if (type == CurveType::Ribbon) {
        Error(loc, "Must provide normals \"N\" at curve endpoints with ribbon "
                   "curves.");
        return {};
    }

    // Start dicing...
    std::vector<int> blpIndices;
    std::vector<Point3f> blpP;
    std::vector<Normal3f> blpN;
    std::vector<Point2f> blpUV;

    int lastCPOffset = -1;
    pstd::array<Point3f, 4> segCpBezier;

    for (int i = 0; i <= nDiceU; ++i) {
        Float u = Float(i) / Float(nDiceU);
        Float width = Lerp(u, width0, width1);

        int segmentIndex = int(u * nSegments);
        if (segmentIndex == nSegments) // u == 1...
            --segmentIndex;

        // Compute offset into original control points for current u
        int cpOffset;
        if (bezierBasis)
            cpOffset = segmentIndex * degree;
        else
            // Uniform b-spline.
            cpOffset = segmentIndex;

        if (cpOffset != lastCPOffset) {
            // update segCpBezier
            if (bezierBasis) {
                if (degree == 2) {
                    // Elevate to degree 3.
                    segCpBezier =
                        ElevateQuadraticBezierToCubic(pstd::MakeConstSpan(cp).subspan(cpOffset, 3));
                } else {
                    // All set.
                    for (int i = 0; i < 4; ++i)
                        segCpBezier[i] = cp[cpOffset + i];
                }
            } else {
                // Uniform b-spline.
                if (degree == 2) {
                    pstd::array<Point3f, 3> bezCp =
                        QuadraticBSplineToBezier(pstd::MakeConstSpan(cp).subspan(cpOffset, 3));
                    segCpBezier = ElevateQuadraticBezierToCubic(pstd::MakeConstSpan(bezCp));
                } else {
                    segCpBezier =
                        CubicBSplineToBezier(pstd::MakeConstSpan(cp).subspan(cpOffset, 4));
                }
            }
            lastCPOffset = cpOffset;
        }

        Float uSeg = (u * nSegments) - segmentIndex;
        CHECK(uSeg >= 0 && uSeg <= 1);

        Vector3f dpdu;
        Point3f p = EvaluateCubicBezier(segCpBezier, uSeg, &dpdu);

        switch (type) {
        case CurveType::Ribbon: {
            Float normalAngle = AngleBetween(n[segmentIndex],
                                             n[segmentIndex + 1]);
            Float invSinNormalAngle = 1 / std::sin(normalAngle);

            Normal3f nu;
            if (normalAngle == 0)
                nu = n[segmentIndex];
            else {
                Float sin0 =
                    std::sin((1 - uSeg) * normalAngle) * invSinNormalAngle;
                Float sin1 =
                    std::sin(uSeg * normalAngle) * invSinNormalAngle;
                nu = sin0 * n[segmentIndex] + sin1 * n[segmentIndex + 1];
            }
            Vector3f dpdv = Normalize(Cross(nu, dpdu)) * width;

            blpP.push_back(p - dpdv / 2);
            blpP.push_back(p + dpdv / 2);
            blpUV.push_back(Point2f(u, 0));
            blpUV.push_back(Point2f(u, 1));

            if (i > 0) {
                blpIndices.push_back(2 * (i - 1));
                blpIndices.push_back(2 * (i - 1) + 1);
                blpIndices.push_back(2 * i);
                blpIndices.push_back(2 * i + 1);
            }
            break;
        }
        case CurveType::Flat:
        case CurveType::Cylinder: {
            Vector3f ortho[2];
            CoordinateSystem(Normalize(dpdu), &ortho[0], &ortho[1]);
            ortho[0] *= width / 2;
            ortho[1] *= width / 2;

            // Repeat the first/last vertex so we can assign different
            // texture coordinates...
            for (int v = 0; v <= nDiceV; ++v) {
                Float angle = Float(v) / nDiceV * 2 * Pi;
                blpP.push_back(p + ortho[0] * std::cos(angle) +
                               ortho[1] * std::sin(angle));
                blpN.push_back(Normal3f(Normalize(blpP.back() - p)));
                blpUV.push_back(Point2f(u, Float(v) / nDiceV));
            }

            if (i > 0) {
                for (int v = 0; v < nDiceV; ++v) {
                    // Indexing is funny due to doubled-up last vertex
                    blpIndices.push_back((nDiceV + 1) * (i - 1) + v);
                    blpIndices.push_back((nDiceV + 1) * (i - 1) + v + 1);
                    blpIndices.push_back((nDiceV + 1) * i + v);
                    blpIndices.push_back((nDiceV + 1) * i + v + 1);
                }
            }
            break;
        }
        }
    }

    nBLPsForCurves += blpIndices.size() / 4;

    return alloc.new_object<BilinearPatchMesh>(
        *shape.renderFromObject, shape.reverseOrientation, blpIndices, blpP, blpN,
        blpUV, std::vector<int>(), nullptr);
}

OptiXAggregate::ASBuildInput OptiXAggregate::createBuildInputForBLPs(
    const std::vector<ShapeSceneEntity> &shapes, const OptixProgramGroup &intersectPG,
    const OptixProgramGroup &shadowPG, const OptixProgramGroup &randomHitPG,
    const std::map<std::string, FloatTexture> &floatTextures,
    const std::map<std::string, Material> &namedMaterials,
    const std::vector<Material> &materials,
    const std::map<std::string, Medium> &media,
    const std::map<int, pstd::vector<Light> *> &shapeIndexToAreaLights,
    Allocator alloc) {
    // Create meshes
    std::vector<BilinearPatchMesh *> meshes(shapes.size(), nullptr);
    std::atomic<int> nPatches = 0, nMeshes = 0;

    for (size_t shapeIndex = 0; shapeIndex < shapes.size(); ++shapeIndex) {
        const auto &shape = shapes[shapeIndex];
        if (shape.name == "bilinearmesh") {
            BilinearPatchMesh *mesh = BilinearPatch::CreateMesh(shape.renderFromObject, shape.reverseOrientation,
                                                                shape.parameters, &shape.loc, alloc);
            meshes[shapeIndex] = mesh;
            nPatches += mesh->nPatches;
            ++nMeshes;
        } else if (shape.name == "curve") {
            BilinearPatchMesh *curveMesh = diceCurveToBLP(shape, 5 /* nseg */, 5 /* nvert */, alloc);
            if (curveMesh) {
                nPatches += curveMesh->nPatches;
                ++nMeshes;
                meshes[shapeIndex] = curveMesh;
            }
        }
    }

    if (nMeshes == 0)
        return {};

    // Create build inputs
    ASBuildInput buildInput(nMeshes);
    int buildInputIndex = 0;
    // FIXME: leaks...
    OptixAabb *aabbs = alloc.allocate_object<OptixAabb>(nPatches);
    int aabbIndex = 0;
    CUdeviceptr *aabbDevicePtrs = new CUdeviceptr[nMeshes];
    uint32_t *flags = new uint32_t[nMeshes];

    for (size_t shapeIndex = 0; shapeIndex < meshes.size(); ++shapeIndex) {
        BilinearPatchMesh *mesh = meshes[shapeIndex];
        if (!mesh)
            continue;

        OptixBuildInput input = {};
        input.type = OPTIX_BUILD_INPUT_TYPE_CUSTOM_PRIMITIVES;
        input.customPrimitiveArray.numSbtRecords = 1;
        input.customPrimitiveArray.numPrimitives = mesh->nPatches;
        aabbDevicePtrs[buildInputIndex] = CUdeviceptr(&aabbs[aabbIndex]);
        input.customPrimitiveArray.aabbBuffers = &aabbDevicePtrs[buildInputIndex];
        input.customPrimitiveArray.flags = &flags[buildInputIndex];

        buildInput.optixInputs[buildInputIndex] = input;

        for (int patchIndex = 0; patchIndex < mesh->nPatches; ++patchIndex) {
            Bounds3f shapeBounds;
            for (int i = 0; i < 4; ++i)
                shapeBounds = Union(shapeBounds, mesh->p[mesh->vertexIndices[4 * patchIndex + i]]);

            OptixAabb aabb = {float(shapeBounds.pMin.x), float(shapeBounds.pMin.y), float(shapeBounds.pMin.z),
                              float(shapeBounds.pMax.x), float(shapeBounds.pMax.y), float(shapeBounds.pMax.z)};
            aabbs[aabbIndex++] = aabb;

            buildInput.bounds = Union(buildInput.bounds, shapeBounds);
        }

        const auto &shape = shapes[shapeIndex];
        Material material = getMaterial(shape, namedMaterials, materials);
        FloatTexture alphaTexture = getAlphaTexture(shape, floatTextures, alloc);

        flags[buildInputIndex] = getOptixGeometryFlags(false, alphaTexture, material);

        HitgroupRecord hgRecord;
        OPTIX_CHECK(optixSbtRecordPackHeader(intersectPG, &hgRecord));
        hgRecord.bilinearRec.mesh = mesh;
        hgRecord.bilinearRec.material = material;
        hgRecord.bilinearRec.alphaTexture = alphaTexture;
        hgRecord.bilinearRec.areaLights = {};
        if (shape.lightIndex != -1) {
            if (!material)
                Warning(&shape.loc, "Ignoring area light specification for shape with \"interface\" material.");
            else {
                auto iter = shapeIndexToAreaLights.find(shapeIndex);
                // Note: this will hit if we try to have an instance as an area
                // light.
                CHECK(iter != shapeIndexToAreaLights.end());
                CHECK_EQ(iter->second->size(), mesh->nPatches);
                hgRecord.bilinearRec.areaLights = pstd::MakeSpan(*iter->second);
            }
        }
        hgRecord.bilinearRec.mediumInterface = getMediumInterface(shape, media, alloc);

        buildInput.intersectHGRecords[buildInputIndex] = hgRecord;

        OPTIX_CHECK(optixSbtRecordPackHeader(randomHitPG, &hgRecord));
        buildInput.randomHitHGRecords[buildInputIndex] = hgRecord;

        OPTIX_CHECK(optixSbtRecordPackHeader(shadowPG, &hgRecord));
        buildInput.shadowHGRecords[buildInputIndex] = hgRecord;

        ++buildInputIndex;
    };

    return buildInput;
}

OptiXAggregate::ASBuildInput OptiXAggregate::createBuildInputForQuadrics(
    const std::vector<ShapeSceneEntity> &shapes, const OptixProgramGroup &intersectPG,
    const OptixProgramGroup &shadowPG, const OptixProgramGroup &randomHitPG,
    const std::map<std::string, FloatTexture> &floatTextures,
    const std::map<std::string, Material> &namedMaterials,
    const std::vector<Material> &materials,
    const std::map<std::string, Medium> &media,
    const std::map<int, pstd::vector<Light> *> &shapeIndexToAreaLights,
    Allocator alloc) {
    int nQuadrics = 0;
    for (size_t shapeIndex = 0; shapeIndex < shapes.size(); ++shapeIndex) {
        const auto &s = shapes[shapeIndex];
        if (s.name == "sphere" || s.name == "cylinder" || s.name == "disk")
            ++nQuadrics;
    }

    if (nQuadrics == 0)
        return {};

    ASBuildInput buildInput(nQuadrics);
    // FIXME: leaks
    OptixAabb *shapeAABBs = alloc.allocate_object<OptixAabb>(nQuadrics);
    CUdeviceptr *aabbDevicePtrs = new CUdeviceptr[nQuadrics];
    unsigned int *flags = new unsigned int[nQuadrics];

    int quadricIndex = 0;
    for (size_t shapeIndex = 0; shapeIndex < shapes.size(); ++shapeIndex) {
        const auto &s = shapes[shapeIndex];
        if (s.name != "sphere" && s.name != "cylinder" && s.name != "disk")
            continue;

        pstd::vector<Shape> shapes = Shape::Create(
            s.name, s.renderFromObject, s.objectFromRender,
            s.reverseOrientation, s.parameters, floatTextures, &s.loc, alloc);
        if (shapes.empty())
            continue;
        CHECK_EQ(1, shapes.size());
        Shape shape = shapes[0];

        OptixBuildInput input = {};

        input.type = OPTIX_BUILD_INPUT_TYPE_CUSTOM_PRIMITIVES;
        input.customPrimitiveArray.numSbtRecords = 1;
        input.customPrimitiveArray.numPrimitives = 1;
        input.customPrimitiveArray.flags = &flags[quadricIndex];

        Bounds3f shapeBounds = shape.Bounds();
        OptixAabb aabb = {float(shapeBounds.pMin.x), float(shapeBounds.pMin.y), float(shapeBounds.pMin.z),
                          float(shapeBounds.pMax.x), float(shapeBounds.pMax.y), float(shapeBounds.pMax.z)};
        shapeAABBs[quadricIndex] = aabb;
        aabbDevicePtrs[quadricIndex] = CUdeviceptr(&shapeAABBs[quadricIndex]);
        input.customPrimitiveArray.aabbBuffers = &aabbDevicePtrs[quadricIndex];

        buildInput.optixInputs[quadricIndex] = input;

        buildInput.bounds = Union(buildInput.bounds, shapeBounds);

        // Find alpha texture, if present.
        Material material = getMaterial(s, namedMaterials, materials);
        FloatTexture alphaTexture = getAlphaTexture(s, floatTextures, alloc);
        flags[quadricIndex] = getOptixGeometryFlags(false, alphaTexture, material);

        HitgroupRecord hgRecord;
        OPTIX_CHECK(optixSbtRecordPackHeader(intersectPG, &hgRecord));
        hgRecord.quadricRec.shape = shape;
        hgRecord.quadricRec.material = material;
        hgRecord.quadricRec.alphaTexture = alphaTexture;
        hgRecord.quadricRec.areaLight = nullptr;
        if (s.lightIndex != -1) {
            if (!material)
                Warning(&s.loc, "Ignoring area light specification for shape with \"interface\" material.");
            else {
                auto iter = shapeIndexToAreaLights.find(shapeIndex);
                // Note: this will hit if we try to have an instance as an area
                // light.
                CHECK(iter != shapeIndexToAreaLights.end());
                CHECK_EQ(iter->second->size(), 1);
                hgRecord.quadricRec.areaLight = (*iter->second)[0];
            }
        }
        hgRecord.quadricRec.mediumInterface = getMediumInterface(s, media, alloc);

        buildInput.intersectHGRecords[quadricIndex] = hgRecord;

        OPTIX_CHECK(optixSbtRecordPackHeader(randomHitPG, &hgRecord));
        buildInput.randomHitHGRecords[quadricIndex] = hgRecord;

        OPTIX_CHECK(optixSbtRecordPackHeader(shadowPG, &hgRecord));
        buildInput.shadowHGRecords[quadricIndex] = hgRecord;

        ++quadricIndex;
    }

    return buildInput;
}

static void logCallback(unsigned int level, const char *tag, const char *message, void *cbdata) {
    if (level <= 2)
        LOG_ERROR("OptiX: %s: %s", tag, message);
    else
        LOG_VERBOSE("OptiX: %s: %s", tag, message);
}

int OptiXAggregate::addHGRecords(const ASBuildInput &buildInput) {
    int sbtOffset = intersectHGRecords.size();
    intersectHGRecords.insert(intersectHGRecords.end(),
                              buildInput.intersectHGRecords.begin(),
                              buildInput.intersectHGRecords.end());
    shadowHGRecords.insert(shadowHGRecords.end(),
                           buildInput.shadowHGRecords.begin(),
                           buildInput.shadowHGRecords.end());
    randomHitHGRecords.insert(randomHitHGRecords.end(),
                              buildInput.randomHitHGRecords.begin(),
                              buildInput.randomHitHGRecords.end());
    return sbtOffset;
}

OptiXAggregate::OptiXAggregate(
    const ParsedScene &scene, Allocator alloc, NamedTextures &textures,
    const std::map<int, pstd::vector<Light> *> &shapeIndexToAreaLights,
    const std::map<std::string, Medium> &media,
    const std::map<std::string, pbrt::Material> &namedMaterials,
    const std::vector<pbrt::Material> &materials)
    : alloc(alloc),
      cudaStream(nullptr),
      intersectHGRecords(alloc),
      shadowHGRecords(alloc),
      randomHitHGRecords(alloc) {
    CUcontext cudaContext;
    CU_CHECK(cuCtxGetCurrent(&cudaContext));
    CHECK(cudaContext != nullptr);

    paramsPool.resize(256);  // should be plenty
    for (ParamBufferState &ps : paramsPool) {
        void *ptr;
        CUDA_CHECK(cudaMalloc(&ptr, sizeof(RayIntersectParameters)));
        ps.ptr = (CUdeviceptr)ptr;
        CUDA_CHECK(cudaEventCreate(&ps.finishedEvent));
        CUDA_CHECK(cudaMallocHost(&ps.hostPtr, sizeof(RayIntersectParameters)));
    }

    // Create OptiX context
    LOG_VERBOSE("Starting OptiX initialization");
    OPTIX_CHECK(optixInit());
    OptixDeviceContextOptions ctxOptions = {};
#ifndef NDEBUG
    ctxOptions.logCallbackLevel = 4;  // status/progress
#else
    ctxOptions.logCallbackLevel = 2;  // error
#endif
    ctxOptions.logCallbackFunction = logCallback;
#if (OPTIX_VERSION >= 70200) && !defined(NDEBUG)
    ctxOptions.validationMode = OPTIX_DEVICE_CONTEXT_VALIDATION_MODE_ALL;
#endif
    OPTIX_CHECK(optixDeviceContextCreate(cudaContext, &ctxOptions, &optixContext));

    LOG_VERBOSE("Optix version %d.%d.%d successfully initialized", OPTIX_VERSION / 10000,
                (OPTIX_VERSION % 10000) / 100, OPTIX_VERSION % 100);

    // OptiX module
    OptixModuleCompileOptions moduleCompileOptions = {};
    // TODO: REVIEW THIS
    moduleCompileOptions.maxRegisterCount = OPTIX_COMPILE_DEFAULT_MAX_REGISTER_COUNT;
#ifndef NDEBUG
    moduleCompileOptions.optLevel = OPTIX_COMPILE_OPTIMIZATION_LEVEL_0;
    moduleCompileOptions.debugLevel = OPTIX_COMPILE_DEBUG_LEVEL_LINEINFO;
#else
    moduleCompileOptions.optLevel = OPTIX_COMPILE_OPTIMIZATION_DEFAULT;
    moduleCompileOptions.debugLevel = OPTIX_COMPILE_DEBUG_LEVEL_NONE;
#endif

    OptixPipelineCompileOptions pipelineCompileOptions = {};
    pipelineCompileOptions.traversableGraphFlags = OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_ANY;
    pipelineCompileOptions.usesMotionBlur = false;
    pipelineCompileOptions.numPayloadValues = 3;
    pipelineCompileOptions.numAttributeValues = 4;
    // OPTIX_EXCEPTION_FLAG_NONE;
    pipelineCompileOptions.exceptionFlags =
        (OPTIX_EXCEPTION_FLAG_STACK_OVERFLOW | OPTIX_EXCEPTION_FLAG_TRACE_DEPTH |
         OPTIX_EXCEPTION_FLAG_DEBUG);
    pipelineCompileOptions.pipelineLaunchParamsVariableName = "params";

    OptixPipelineLinkOptions pipelineLinkOptions = {};
    pipelineLinkOptions.maxTraceDepth = 2;
#ifndef NDEBUG
    pipelineLinkOptions.debugLevel = OPTIX_COMPILE_DEBUG_LEVEL_FULL;
#else
    pipelineLinkOptions.debugLevel = OPTIX_COMPILE_DEBUG_LEVEL_NONE;
#endif

    const std::string ptxCode((const char *)PBRT_EMBEDDED_PTX);

    char log[4096];
    size_t logSize = sizeof(log);
    OPTIX_CHECK_WITH_LOG(
        optixModuleCreateFromPTX(optixContext, &moduleCompileOptions,
                                 &pipelineCompileOptions, ptxCode.c_str(),
                                 ptxCode.size(), log, &logSize, &optixModule),
        log);
    LOG_VERBOSE("%s", log);

    // Optix program groups...
    OptixProgramGroupOptions pgOptions = {};
    OptixProgramGroup raygenPGClosest;
    {
        OptixProgramGroupDesc desc = {};
        desc.kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
        desc.raygen.module = optixModule;
        desc.raygen.entryFunctionName = "__raygen__findClosest";
        OPTIX_CHECK_WITH_LOG(optixProgramGroupCreate(optixContext, &desc, 1, &pgOptions,
                                                     log, &logSize, &raygenPGClosest),
                             log);
        LOG_VERBOSE("%s", log);
    }

    OptixProgramGroup missPGNoOp;
    {
        OptixProgramGroupDesc desc = {};
        desc.kind = OPTIX_PROGRAM_GROUP_KIND_MISS;
        desc.miss.module = optixModule;
        desc.miss.entryFunctionName = "__miss__noop";
        OPTIX_CHECK_WITH_LOG(optixProgramGroupCreate(optixContext, &desc, 1, &pgOptions,
                                                     log, &logSize, &missPGNoOp),
                             log);
        LOG_VERBOSE("%s", log);
    }

    OptixProgramGroup hitPGTriangle;
    {
        OptixProgramGroupDesc desc = {};
        desc.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
        desc.hitgroup.moduleCH = optixModule;
        desc.hitgroup.entryFunctionNameCH = "__closesthit__triangle";
        desc.hitgroup.moduleAH = optixModule;
        desc.hitgroup.entryFunctionNameAH = "__anyhit__triangle";
        OPTIX_CHECK_WITH_LOG(optixProgramGroupCreate(optixContext, &desc, 1, &pgOptions,
                                                     log, &logSize, &hitPGTriangle),
                             log);
        LOG_VERBOSE("%s", log);
    }

    OptixProgramGroup hitPGBilinearPatch;
    {
        OptixProgramGroupDesc desc = {};
        desc.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
        desc.hitgroup.moduleCH = optixModule;
        desc.hitgroup.entryFunctionNameCH = "__closesthit__bilinearPatch";
        desc.hitgroup.moduleIS = optixModule;
        desc.hitgroup.entryFunctionNameIS = "__intersection__bilinearPatch";
        OPTIX_CHECK_WITH_LOG(optixProgramGroupCreate(optixContext, &desc, 1, &pgOptions,
                                                     log, &logSize, &hitPGBilinearPatch),
                             log);
        LOG_VERBOSE("%s", log);
    }

    OptixProgramGroup hitPGQuadric;
    {
        OptixProgramGroupDesc desc = {};
        desc.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
        desc.hitgroup.moduleCH = optixModule;
        desc.hitgroup.entryFunctionNameCH = "__closesthit__quadric";
        desc.hitgroup.moduleIS = optixModule;
        desc.hitgroup.entryFunctionNameIS = "__intersection__quadric";
        OPTIX_CHECK_WITH_LOG(optixProgramGroupCreate(optixContext, &desc, 1, &pgOptions,
                                                     log, &logSize, &hitPGQuadric),
                             log);
        LOG_VERBOSE("%s", log);
    }

    OptixProgramGroup raygenPGShadow;
    {
        OptixProgramGroupDesc desc = {};
        desc.kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
        desc.raygen.module = optixModule;
        desc.raygen.entryFunctionName = "__raygen__shadow";
        OPTIX_CHECK_WITH_LOG(optixProgramGroupCreate(optixContext, &desc, 1, &pgOptions,
                                                     log, &logSize, &raygenPGShadow),
                             log);
        LOG_VERBOSE("%s", log);
    }

    OptixProgramGroup missPGShadow;
    {
        OptixProgramGroupDesc desc = {};
        desc.kind = OPTIX_PROGRAM_GROUP_KIND_MISS;
        desc.miss.module = optixModule;
        desc.miss.entryFunctionName = "__miss__shadow";
        OPTIX_CHECK_WITH_LOG(optixProgramGroupCreate(optixContext, &desc, 1, &pgOptions,
                                                     log, &logSize, &missPGShadow),
                             log);
        LOG_VERBOSE("%s", log);
    }

    OptixProgramGroup anyhitPGShadowTriangle;
    {
        OptixProgramGroupDesc desc = {};
        desc.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
        desc.hitgroup.moduleAH = optixModule;
        desc.hitgroup.entryFunctionNameAH = "__anyhit__shadowTriangle";
        OPTIX_CHECK_WITH_LOG(
            optixProgramGroupCreate(optixContext, &desc, 1, &pgOptions, log, &logSize,
                                    &anyhitPGShadowTriangle),
            log);
        LOG_VERBOSE("%s", log);
    }

    OptixProgramGroup raygenPGShadowTr;
    {
        OptixProgramGroupDesc desc = {};
        desc.kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
        desc.raygen.module = optixModule;
        desc.raygen.entryFunctionName = "__raygen__shadow_Tr";
        OPTIX_CHECK_WITH_LOG(optixProgramGroupCreate(optixContext, &desc, 1, &pgOptions,
                                                     log, &logSize, &raygenPGShadowTr),
                             log);
        LOG_VERBOSE("%s", log);
    }

    OptixProgramGroup missPGShadowTr;
    {
        OptixProgramGroupDesc desc = {};
        desc.kind = OPTIX_PROGRAM_GROUP_KIND_MISS;
        desc.miss.module = optixModule;
        desc.miss.entryFunctionName = "__miss__shadow_Tr";
        OPTIX_CHECK_WITH_LOG(optixProgramGroupCreate(optixContext, &desc, 1, &pgOptions,
                                                     log, &logSize, &missPGShadowTr),
                             log);
        LOG_VERBOSE("%s", log);
    }

    OptixProgramGroup anyhitPGShadowBilinearPatch;
    {
        OptixProgramGroupDesc desc = {};
        desc.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
        desc.hitgroup.moduleIS = optixModule;
        desc.hitgroup.entryFunctionNameIS = "__intersection__bilinearPatch";
        desc.hitgroup.moduleAH = optixModule;
        desc.hitgroup.entryFunctionNameAH = "__anyhit__shadowBilinearPatch";
        OPTIX_CHECK_WITH_LOG(
            optixProgramGroupCreate(optixContext, &desc, 1, &pgOptions, log, &logSize,
                                    &anyhitPGShadowBilinearPatch),
            log);
        LOG_VERBOSE("%s", log);
    }

    OptixProgramGroup anyhitPGShadowQuadric;
    {
        OptixProgramGroupDesc desc = {};
        desc.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
        desc.hitgroup.moduleIS = optixModule;
        desc.hitgroup.entryFunctionNameIS = "__intersection__quadric";
        desc.hitgroup.moduleAH = optixModule;
        desc.hitgroup.entryFunctionNameAH = "__anyhit__shadowQuadric";
        OPTIX_CHECK_WITH_LOG(
            optixProgramGroupCreate(optixContext, &desc, 1, &pgOptions, log, &logSize,
                                    &anyhitPGShadowQuadric),
            log);
        LOG_VERBOSE("%s", log);
    }

    OptixProgramGroup raygenPGRandomHit;
    {
        OptixProgramGroupDesc desc = {};
        desc.kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
        desc.raygen.module = optixModule;
        desc.raygen.entryFunctionName = "__raygen__randomHit";
        OPTIX_CHECK_WITH_LOG(optixProgramGroupCreate(optixContext, &desc, 1, &pgOptions,
                                                     log, &logSize, &raygenPGRandomHit),
                             log);
        LOG_VERBOSE("%s", log);
    }

    OptixProgramGroup hitPGRandomHitTriangle;
    {
        OptixProgramGroupDesc desc = {};
        desc.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
        desc.hitgroup.moduleCH = optixModule;
        desc.hitgroup.entryFunctionNameCH = "__closesthit__randomHitTriangle";
        OPTIX_CHECK_WITH_LOG(
            optixProgramGroupCreate(optixContext, &desc, 1, &pgOptions, log, &logSize,
                                    &hitPGRandomHitTriangle),
            log);
        LOG_VERBOSE("%s", log);
    }

    OptixProgramGroup hitPGRandomHitBilinearPatch;
    {
        OptixProgramGroupDesc desc = {};
        desc.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
        desc.hitgroup.moduleIS = optixModule;
        desc.hitgroup.entryFunctionNameIS = "__intersection__bilinearPatch";
        desc.hitgroup.moduleCH = optixModule;
        desc.hitgroup.entryFunctionNameCH = "__closesthit__randomHitBilinearPatch";
        OPTIX_CHECK_WITH_LOG(
            optixProgramGroupCreate(optixContext, &desc, 1, &pgOptions, log, &logSize,
                                    &hitPGRandomHitBilinearPatch),
            log);
        LOG_VERBOSE("%s", log);
    }

    OptixProgramGroup hitPGRandomHitQuadric;
    {
        OptixProgramGroupDesc desc = {};
        desc.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
        desc.hitgroup.moduleIS = optixModule;
        desc.hitgroup.entryFunctionNameIS = "__intersection__quadric";
        desc.hitgroup.moduleCH = optixModule;
        desc.hitgroup.entryFunctionNameCH = "__closesthit__randomHitQuadric";
        OPTIX_CHECK_WITH_LOG(
            optixProgramGroupCreate(optixContext, &desc, 1, &pgOptions, log, &logSize,
                                    &hitPGRandomHitQuadric),
            log);
        LOG_VERBOSE("%s", log);
    }

    // Optix pipeline...
    OptixProgramGroup allPGs[] = {raygenPGClosest,
                                  missPGNoOp,
                                  hitPGTriangle,
                                  hitPGBilinearPatch,
                                  hitPGQuadric,
                                  raygenPGShadow,
                                  missPGShadow,
                                  anyhitPGShadowTriangle,
                                  anyhitPGShadowBilinearPatch,
                                  anyhitPGShadowQuadric,
                                  raygenPGShadowTr,
                                  missPGShadowTr,
                                  raygenPGRandomHit,
                                  hitPGRandomHitTriangle,
                                  hitPGRandomHitBilinearPatch,
                                  hitPGRandomHitQuadric};
    OPTIX_CHECK_WITH_LOG(
        optixPipelineCreate(optixContext, &pipelineCompileOptions, &pipelineLinkOptions,
                            allPGs, sizeof(allPGs) / sizeof(allPGs[0]), log, &logSize,
                            &optixPipeline),
        log);
    LOG_VERBOSE("%s", log);

#if 0
    OPTIX_CHECK(optixPipelineSetStackSize(
        optixPipeline,
        0, /* direct callables from intersect or any-hit */
        0, /* direct callables from raygen, miss, or closest hit */
        4 * 1024, /* continuation stack */
        2 /* max graph depth. NOTE: this is 3 when we have motion xforms... */));
#endif

    // Shader binding tables...
    // Hitgroups are done as meshes are processed

    // Closest intersection
    RaygenRecord *raygenClosestRecord = alloc.new_object<RaygenRecord>();
    OPTIX_CHECK(optixSbtRecordPackHeader(raygenPGClosest, raygenClosestRecord));
    intersectSBT.raygenRecord = (CUdeviceptr)raygenClosestRecord;

    MissRecord *missNoOpRecord = alloc.new_object<MissRecord>();
    OPTIX_CHECK(optixSbtRecordPackHeader(missPGNoOp, missNoOpRecord));
    intersectSBT.missRecordBase = (CUdeviceptr)missNoOpRecord;
    intersectSBT.missRecordStrideInBytes = sizeof(MissRecord);
    intersectSBT.missRecordCount = 1;

    // Shadow
    RaygenRecord *raygenShadowRecord = alloc.new_object<RaygenRecord>();
    OPTIX_CHECK(optixSbtRecordPackHeader(raygenPGShadow, raygenShadowRecord));
    shadowSBT.raygenRecord = (CUdeviceptr)raygenShadowRecord;

    MissRecord *missShadowRecord = alloc.new_object<MissRecord>();
    OPTIX_CHECK(optixSbtRecordPackHeader(missPGShadow, missShadowRecord));
    shadowSBT.missRecordBase = (CUdeviceptr)missShadowRecord;
    shadowSBT.missRecordStrideInBytes = sizeof(MissRecord);
    shadowSBT.missRecordCount = 1;

    // Shadow + Tr
    RaygenRecord *raygenShadowTrRecord = alloc.new_object<RaygenRecord>();
    OPTIX_CHECK(optixSbtRecordPackHeader(raygenPGShadowTr, raygenShadowTrRecord));
    shadowTrSBT.raygenRecord = (CUdeviceptr)raygenShadowTrRecord;

    MissRecord *missShadowTrRecord = alloc.new_object<MissRecord>();
    OPTIX_CHECK(optixSbtRecordPackHeader(missPGShadowTr, missShadowTrRecord));
    shadowTrSBT.missRecordBase = (CUdeviceptr)missShadowTrRecord;
    shadowTrSBT.missRecordStrideInBytes = sizeof(MissRecord);
    shadowTrSBT.missRecordCount = 1;

    // Random hit
    RaygenRecord *raygenRandomHitRecord = alloc.new_object<RaygenRecord>();
    OPTIX_CHECK(optixSbtRecordPackHeader(raygenPGRandomHit, raygenRandomHitRecord));
    randomHitSBT.raygenRecord = (CUdeviceptr)raygenRandomHitRecord;
    randomHitSBT.missRecordBase = (CUdeviceptr)missNoOpRecord;
    randomHitSBT.missRecordStrideInBytes = sizeof(MissRecord);
    randomHitSBT.missRecordCount = 1;

    LOG_VERBOSE("Finished OptiX initialization");

    LOG_VERBOSE("Starting to create shapes and acceleration structures");
    for (const auto &shape : scene.shapes)
        if (shape.name != "sphere" && shape.name != "cylinder" && shape.name != "disk" &&
            shape.name != "trianglemesh" && shape.name != "plymesh" &&
            shape.name != "loopsubdiv" && shape.name != "bilinearmesh" &&
            shape.name != "curve")
            ErrorExit(&shape.loc, "%s: unknown shape", shape.name);

    LOG_VERBOSE("Starting to read PLY meshes");
    std::map<int, TriQuadMesh> plyMeshes = PreparePLYMeshes(scene.shapes, textures.floatTextures);
    LOG_VERBOSE("Finished reading PLY meshes");

    LOG_VERBOSE("Starting to create GAS for top-level triangles");
    ASBuildInput triangleBuildInput = createBuildInputForTriangles(
        scene.shapes, plyMeshes, hitPGTriangle, anyhitPGShadowTriangle, hitPGRandomHitTriangle,
        textures.floatTextures, namedMaterials, materials, media, shapeIndexToAreaLights, alloc);
    bounds = Union(bounds, triangleBuildInput.bounds);
    (void)addHGRecords(triangleBuildInput);
    OptixTraversableHandle triangleGASTraversable = buildBVH(triangleBuildInput.optixInputs);
    LOG_VERBOSE("Finished creating GAS for top-level triangles");

    LOG_VERBOSE("Starting to create GAS for top-level blps/curves");
    ASBuildInput blpBuildInput =
        createBuildInputForBLPs(scene.shapes, hitPGBilinearPatch, anyhitPGShadowBilinearPatch,
                                hitPGRandomHitBilinearPatch, textures.floatTextures, namedMaterials,
                                materials, media, shapeIndexToAreaLights, alloc);
    bounds = Union(bounds, blpBuildInput.bounds);
    int bilinearSBTOffset = addHGRecords(blpBuildInput);
    OptixTraversableHandle bilinearPatchGASTraversable = buildBVH(blpBuildInput.optixInputs);
    LOG_VERBOSE("Finished creating GAS for top-level blps/curves");

    LOG_VERBOSE("Starting to create GAS for top-level quadrics");
    ASBuildInput quadricBuildInput = createBuildInputForQuadrics(
        scene.shapes, hitPGQuadric, anyhitPGShadowQuadric, hitPGRandomHitQuadric,
        textures.floatTextures, namedMaterials, materials, media, shapeIndexToAreaLights, alloc);

    bounds = Union(bounds, quadricBuildInput.bounds);
    int quadricSBTOffset = addHGRecords(quadricBuildInput);
    OptixTraversableHandle quadricGASTraversable = buildBVH(quadricBuildInput.optixInputs);
    LOG_VERBOSE("Finished creating GAS for top-level quadrics");

    pstd::vector<OptixInstance> iasInstances(alloc);

    OptixInstance gasInstance = {};
    float identity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    memcpy(gasInstance.transform, identity, 12 * sizeof(float));
    gasInstance.visibilityMask = 255;
    gasInstance.flags =
        OPTIX_INSTANCE_FLAG_NONE;  // TODO: OPTIX_INSTANCE_FLAG_DISABLE_ANYHIT
    if (triangleGASTraversable) {
        gasInstance.traversableHandle = triangleGASTraversable;
        gasInstance.sbtOffset = 0;
        iasInstances.push_back(gasInstance);
    }
    if (bilinearPatchGASTraversable) {
        gasInstance.traversableHandle = bilinearPatchGASTraversable;
        gasInstance.sbtOffset = bilinearSBTOffset;
        iasInstances.push_back(gasInstance);
    }
    if (quadricGASTraversable) {
        gasInstance.traversableHandle = quadricGASTraversable;
        gasInstance.sbtOffset = quadricSBTOffset;
        iasInstances.push_back(gasInstance);
    }

    // Create GASs for instance definitions
    // TODO: better name here...
    struct Instance {
        OptixTraversableHandle handle;
        int sbtOffset;
        Bounds3f bounds;
    };

    // Read (and possibly displace!) PLY meshes in parallel.
    std::map<std::string, std::map<int, TriQuadMesh>> instancePLYMeshes;
    std::vector<std::string> allInstanceNames;
    for (const auto &def : scene.instanceDefinitions)
        allInstanceNames.push_back(def.first);

    LOG_VERBOSE("Starting to read PLY meshes for instances");
    std::mutex mutex;
    ParallelFor(0, allInstanceNames.size(), [&](int64_t i) {
        const std::string &name = allInstanceNames[i];
        auto iter = scene.instanceDefinitions.find(name);
        std::map<int, TriQuadMesh> meshes =
            PreparePLYMeshes(iter->second.shapes, textures.floatTextures);

        std::lock_guard<std::mutex> lock(mutex);
        instancePLYMeshes[name] = std::move(meshes);
    });
    LOG_VERBOSE("Finished reading PLY meshes for instances");

    LOG_VERBOSE("Starting to create GASes for instance definitions");
    std::multimap<std::string, Instance> instanceMap;
    for (const auto &def : scene.instanceDefinitions) {
        if (!def.second.animatedShapes.empty())
            Warning("Ignoring %d animated shapes in instance \"%s\".",
                    def.second.animatedShapes.size(), def.first);

        ASBuildInput triangleBuildInput = createBuildInputForTriangles(
            def.second.shapes, instancePLYMeshes[def.first], hitPGTriangle, anyhitPGShadowTriangle,
            hitPGRandomHitTriangle, textures.floatTextures, namedMaterials, materials, media, {},
            alloc);
        instancePLYMeshes[def.first].clear();
        int triSBTOffset = addHGRecords(triangleBuildInput);
        OptixTraversableHandle triHandle = buildBVH(triangleBuildInput.optixInputs);
        if (triHandle)
            instanceMap.insert({def.first, Instance{triHandle, triSBTOffset,
                                                    triangleBuildInput.bounds}});

        ASBuildInput bilinearBuildInput =
            createBuildInputForBLPs(def.second.shapes, hitPGBilinearPatch, anyhitPGShadowBilinearPatch,
                                    hitPGRandomHitBilinearPatch, textures.floatTextures, namedMaterials,
                                    materials, media, {}, alloc);
        int bilinearSBTOffset = addHGRecords(bilinearBuildInput);
        OptixTraversableHandle bilinearHandle = buildBVH(bilinearBuildInput.optixInputs);
        if (bilinearHandle)
            instanceMap.insert({def.first, Instance{bilinearHandle, bilinearSBTOffset,
                                                    bilinearBuildInput.bounds}});

        ASBuildInput quadricBuildInput =
            createBuildInputForQuadrics(def.second.shapes, hitPGQuadric, anyhitPGShadowQuadric,
                                        hitPGRandomHitQuadric, textures.floatTextures, namedMaterials,
                                        materials, media, {}, alloc);
        int quadricSBTOffset = addHGRecords(quadricBuildInput);
        OptixTraversableHandle quadricHandle = buildBVH(quadricBuildInput.optixInputs);
        if (quadricHandle)
            instanceMap.insert({def.first, Instance{quadricHandle, quadricSBTOffset,
                                                    quadricBuildInput.bounds}});

        if (!triHandle && !bilinearHandle && !quadricHandle)
            // empty instance definition... put something there so we can
            // tell the difference between an empty definition and no
            // definition below.
            instanceMap.insert({def.first, Instance{{}, -1, {}}});
    }
    LOG_VERBOSE("Finished creating GASes for instance definitions");

    // Create OptixInstances for instances
    for (const auto &inst : scene.instances) {
        auto iterPair = instanceMap.equal_range(inst.name);
        if (std::distance(iterPair.first, iterPair.second) == 0)
            ErrorExit(&inst.loc, "%s: object instance not defined.", inst.name);

        if (inst.renderFromInstance == nullptr) {
            Warning(&inst.loc, "%s: object instance has animated transformation. TODO",
                    inst.name);
            continue;
        }

        for (auto iter = iterPair.first; iter != iterPair.second; ++iter) {
            const Instance &in = iter->second;
            if (!in.handle)
                // empty instance definition
                continue;

            bounds = Union(bounds, (*inst.renderFromInstance)(in.bounds));

            OptixInstance optixInstance = {};
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 4; ++j)
                    optixInstance.transform[4 * i + j] =
                        inst.renderFromInstance->GetMatrix()[i][j];
            optixInstance.visibilityMask = 255;
            optixInstance.sbtOffset = in.sbtOffset;
            optixInstance.flags =
                OPTIX_INSTANCE_FLAG_NONE;  // TODO:
            // OPTIX_INSTANCE_FLAG_DISABLE_ANYHIT
            optixInstance.traversableHandle = in.handle;
            iasInstances.push_back(optixInstance);
        }
    }

    // Build the top-level IAS
    LOG_VERBOSE("Starting to build top-level IAS");
    OptixBuildInput buildInput = {};
    buildInput.type = OPTIX_BUILD_INPUT_TYPE_INSTANCES;
    buildInput.instanceArray.instances = CUdeviceptr(iasInstances.data());
    buildInput.instanceArray.numInstances = iasInstances.size();
    std::vector<OptixBuildInput> buildInputs = {buildInput};

    rootTraversable = buildBVH({buildInput});
    LOG_VERBOSE("Finished building top-level IAS");

    LOG_VERBOSE("Finished creating shapes and acceleration structures");

    if (!scene.animatedShapes.empty())
        Warning("Ignoring %d animated shapes", scene.animatedShapes.size());

    intersectSBT.hitgroupRecordBase = (CUdeviceptr)intersectHGRecords.data();
    intersectSBT.hitgroupRecordStrideInBytes = sizeof(HitgroupRecord);
    intersectSBT.hitgroupRecordCount = intersectHGRecords.size();

    shadowSBT.hitgroupRecordBase = (CUdeviceptr)shadowHGRecords.data();
    shadowSBT.hitgroupRecordStrideInBytes = sizeof(HitgroupRecord);
    shadowSBT.hitgroupRecordCount = shadowHGRecords.size();

    // Still want to run the closest hit shaders...
    shadowTrSBT.hitgroupRecordBase = (CUdeviceptr)intersectHGRecords.data();
    shadowTrSBT.hitgroupRecordStrideInBytes = sizeof(HitgroupRecord);
    shadowTrSBT.hitgroupRecordCount = intersectHGRecords.size();

    randomHitSBT.hitgroupRecordBase = (CUdeviceptr)randomHitHGRecords.data();
    randomHitSBT.hitgroupRecordStrideInBytes = sizeof(HitgroupRecord);
    randomHitSBT.hitgroupRecordCount = randomHitHGRecords.size();
}

OptiXAggregate::ParamBufferState &OptiXAggregate::getParamBuffer(
    const RayIntersectParameters &params) const {
    CHECK(nextParamOffset < paramsPool.size());

    ParamBufferState &pbs = paramsPool[nextParamOffset];
    if (++nextParamOffset == paramsPool.size())
        nextParamOffset = 0;
    if (!pbs.used)
        pbs.used = true;
    else
        CUDA_CHECK(cudaEventSynchronize(pbs.finishedEvent));

    // Copy to host-side pinned memory
    memcpy(pbs.hostPtr, &params, sizeof(params));
    CUDA_CHECK(cudaMemcpyAsync((void *)pbs.ptr, pbs.hostPtr, sizeof(params),
                               cudaMemcpyHostToDevice));

    return pbs;
}

void OptiXAggregate::IntersectClosest(
    int maxRays, const RayQueue *rayQueue,
    EscapedRayQueue *escapedRayQueue, HitAreaLightQueue *hitAreaLightQueue,
    MaterialEvalQueue *basicEvalMaterialQueue,
    MaterialEvalQueue *universalEvalMaterialQueue,
    MediumSampleQueue *mediumSampleQueue,
    RayQueue *nextRayQueue) const {
    std::pair<cudaEvent_t, cudaEvent_t> events =
        GetProfilerEvents("Tracing closest hit rays");

    cudaEventRecord(events.first);

    if (rootTraversable) {
        RayIntersectParameters params;
        params.traversable = rootTraversable;
        params.rayQueue = rayQueue;
        params.nextRayQueue = nextRayQueue;
        params.escapedRayQueue = escapedRayQueue;
        params.hitAreaLightQueue = hitAreaLightQueue;
        params.basicEvalMaterialQueue = basicEvalMaterialQueue;
        params.universalEvalMaterialQueue = universalEvalMaterialQueue;
        params.mediumSampleQueue = mediumSampleQueue;

        ParamBufferState &pbs = getParamBuffer(params);

#ifndef NDEBUG
        LOG_VERBOSE("Launching intersect closest");
#endif
#ifdef NVTX
        nvtxRangePush("OptiXAggregate::IntersectClosest");
#endif

        OPTIX_CHECK(optixLaunch(optixPipeline, cudaStream, pbs.ptr,
                                sizeof(RayIntersectParameters), &intersectSBT, maxRays, 1,
                                1));
        CUDA_CHECK(cudaEventRecord(pbs.finishedEvent));

#ifdef NVTX
        nvtxRangePop();
#endif
#ifndef NDEBUG
        CUDA_CHECK(cudaDeviceSynchronize());
        LOG_VERBOSE("Post-sync triangle intersect closest");
#endif
    }

    cudaEventRecord(events.second);
};

void OptiXAggregate::IntersectShadow(int maxRays, ShadowRayQueue *shadowRayQueue,
                               SOA<PixelSampleState> *pixelSampleState) const {
    std::pair<cudaEvent_t, cudaEvent_t> events = GetProfilerEvents("Tracing shadow rays");

    cudaEventRecord(events.first);

    if (rootTraversable) {
        RayIntersectParameters params;
        params.traversable = rootTraversable;
        params.shadowRayQueue = shadowRayQueue;
        params.pixelSampleState = *pixelSampleState;

        ParamBufferState &pbs = getParamBuffer(params);

#ifndef NDEBUG
        LOG_VERBOSE("Launching intersect shadow");
#endif
#ifdef NVTX
        nvtxRangePush("OptiXAggregate::IntersectShadow");
#endif

        OPTIX_CHECK(optixLaunch(optixPipeline, cudaStream, pbs.ptr,
                                sizeof(RayIntersectParameters), &shadowSBT, maxRays, 1,
                                1));
        CUDA_CHECK(cudaEventRecord(pbs.finishedEvent));

#ifdef NVTX
        nvtxRangePop();
#endif
#ifndef NDEBUG
        CUDA_CHECK(cudaDeviceSynchronize());
        LOG_VERBOSE("Post-sync intersect shadow");
#endif
    }

    cudaEventRecord(events.second);
}

void OptiXAggregate::IntersectShadowTr(int maxRays, ShadowRayQueue *shadowRayQueue,
                                 SOA<PixelSampleState> *pixelSampleState) const {
    std::pair<cudaEvent_t, cudaEvent_t> events = GetProfilerEvents("Tracing shadow Tr rays");

    cudaEventRecord(events.first);

    if (rootTraversable) {
        RayIntersectParameters params;
        params.traversable = rootTraversable;
        params.shadowRayQueue = shadowRayQueue;
        params.pixelSampleState = *pixelSampleState;

        ParamBufferState &pbs = getParamBuffer(params);

#ifndef NDEBUG
        LOG_VERBOSE("Launching intersect shadow Tr");
#endif
#ifdef NVTX
        nvtxRangePush("OptiXAggregate::IntersectShadowTr");
#endif

        OPTIX_CHECK(optixLaunch(optixPipeline, cudaStream, pbs.ptr,
                                sizeof(RayIntersectParameters), &shadowTrSBT, maxRays, 1,
                                1));
        CUDA_CHECK(cudaEventRecord(pbs.finishedEvent));

#ifdef NVTX
        nvtxRangePop();
#endif
#ifndef NDEBUG
        CUDA_CHECK(cudaDeviceSynchronize());
        LOG_VERBOSE("Post-sync intersect shadow Tr");
#endif
    }

    cudaEventRecord(events.second);
}

void OptiXAggregate::IntersectOneRandom(int maxRays,
                                  SubsurfaceScatterQueue *subsurfaceScatterQueue) const {
    std::pair<cudaEvent_t, cudaEvent_t> events =
        GetProfilerEvents("Tracing subsurface scattering probe rays");

    cudaEventRecord(events.first);

    if (rootTraversable) {
        RayIntersectParameters params;
        params.traversable = rootTraversable;
        params.subsurfaceScatterQueue = subsurfaceScatterQueue;

        ParamBufferState &pbs = getParamBuffer(params);

#ifndef NDEBUG
        LOG_VERBOSE("Launching intersect random");
#endif
#ifdef NVTX
        nvtxRangePush("OptiXAggregate::IntersectOneRandom");
#endif

        OPTIX_CHECK(optixLaunch(optixPipeline, cudaStream, pbs.ptr,
                                sizeof(RayIntersectParameters), &randomHitSBT, maxRays, 1,
                                1));
        CUDA_CHECK(cudaEventRecord(pbs.finishedEvent));

#ifdef NVTX
        nvtxRangePop();
#endif
#ifndef NDEBUG
        CUDA_CHECK(cudaDeviceSynchronize());
        LOG_VERBOSE("Post-sync intersect random");
#endif
    }

    cudaEventRecord(events.second);
}

} // namespace pbrt
