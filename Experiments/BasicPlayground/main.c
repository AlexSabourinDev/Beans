#define SOKOL_IMPL
#define SOKOL_NOAPI
#include <sokol/sokol_app.h>
#include <sokol/sokol_time.h>

#include <cranberries/cranberry_math.h>

#include <iceberg/ib_core.h>
#include <iceberg/ib_rendergraph.h>

#include "../Shared/ib_imgui.h"
#include <cimgui.h>

#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

static bool PauseTime = false;

static iba_StackAllocator StackAllocator = { 0 };

static ib_Core Core;
static ibr_RenderGraph Graphs[ib_FramebufferCount];
static ib_Surface Surface;

static void* transientAlloc(size_t size, size_t alignment)
{
    iba_StackAllocation allocation = iba_stackAlloc(&StackAllocator, (iba_StackAllocationRequest){ size, alignment });
    return iba_cpuStackAllocToMemory(allocation);
}

// Platform... stuff. I'd like to put this somewhere.
// Maybe something line cranberry_platform
static char CurrentWorkingDirectory[256];
static void readWholeFileFromHandle(FILE* file, void** output, size_t* outputSize)
{
    fseek(file, 0, SEEK_END);
    long fileSize = ftell(file);
    fseek(file, 0, SEEK_SET);

    *output = transientAlloc(fileSize, 1u);
    *outputSize = fileSize;

    fread(*output, fileSize, 1, file);
}

static bool readWholeFile(char const* path, void** output, size_t* outputSize)
{
    FILE *file = fopen(path, "rb");
    if (file != NULL)
    {
        readWholeFileFromHandle(file, output, outputSize);
        fclose(file);
        return true;
    }

    return false;
}

typedef struct
{
    uint16_t* Indices;
    c16i3* Positions;
    c16i2* Normals;
    cv2* UVs;
    uint32_t IndexCount;
    uint32_t VertexCount;
} MeshDesc;

static size_t const GlobalBufferMemorySize = 1024u * 1024u * 32u; // 32 MB of global buffer memory.
static ib_Buffer GlobalBufferMemory;
static iba_TlsfAllocator GlobalBufferMemoryAllocator;

// Quantization: https://cbloomrants.blogspot.com/2020/09/topics-in-quantization-for-games.html
static c16i3 packPosition(cv3 pos)
{
    float const posFixedPointScale = exp2f(7.0f);
    // Rescale such that position is fixed point 8.7
    // As a result, 1 is 1/128 and 128 is 1
    pos = cv3_mulf(pos, posFixedPointScale);
    return cv3_to_c16i3(pos);
}

static c16i2 packNormal(cv3 normal)
{
    float const normalFixedPointScale = exp2f(15.0f);
    // Rescale such that normal is fixed point 1.15
    cv2 octahedral = cv3_to_octahedral(normal);
    ib_assert(octahedral.x >= 0.0f && octahedral.y >= 0.0f);
    ib_assert(octahedral.x <= 1.0f && octahedral.y <= 1.0f);
    octahedral = cv2_mulf(octahedral, normalFixedPointScale);
    return cv2_to_c16i2(octahedral);
}

static MeshDesc createSphereMesh(uint32_t latitudeSegments, uint32_t longitudeSegments, float radius)
{
    MeshDesc mesh = {0};
    uint32_t segmentQuadCount = latitudeSegments * longitudeSegments;
    uint32_t segmentIndexCount = segmentQuadCount * 6u;

    uint32_t latitudeVertexCount = latitudeSegments + 1u;
    uint32_t longitudeVertexCount = longitudeSegments + 1u;
    uint32_t vertexCount = latitudeVertexCount * longitudeVertexCount;

    mesh.Indices = (uint16_t*)transientAlloc(segmentIndexCount * sizeof(uint16_t), alignof(uint16_t));
    mesh.Positions = (c16i3*)transientAlloc(vertexCount * sizeof(c16i3), alignof(c16i3));
    mesh.Normals = (c16i2*)transientAlloc(vertexCount * sizeof(c16i2), alignof(c16i2));
    mesh.UVs = (cv2*)transientAlloc(vertexCount * sizeof(cv2), alignof(cv2));
    mesh.IndexCount = segmentIndexCount;
    mesh.VertexCount = vertexCount;

    uint32_t indexWriter = 0;
    uint32_t vertexWriter = 0;

    // First, build up our vertex lattice
    float deltaTheta = cran_pi * cf_rcp((float)longitudeSegments);
    float deltaPhi = cran_tao * cf_rcp((float)latitudeSegments);
    float theta = 0.0f;
    for (uint32_t thetaI = 0; thetaI < longitudeVertexCount; theta += deltaTheta, thetaI++)
    {
        float phi = 0.0f;
        for (uint32_t phiI = 0; phiI < latitudeVertexCount; phi += deltaPhi, phiI++)
        {
            cv3 pos = cv3_from_spherical(phi, theta, 1.0f);

            mesh.Positions[vertexWriter] = packPosition(cv3_mulf(pos, radius));
            mesh.Normals[vertexWriter] = packNormal(pos);
            mesh.UVs[vertexWriter] = (cv2) { phi / cran_tao, theta / cran_pi };

            vertexWriter++;
        }
    }

    for (uint32_t thetaI = 0; thetaI < longitudeSegments; thetaI++)
    {
        uint32_t thetaVertStart = latitudeVertexCount * thetaI;
        for (uint32_t phiI = 0; phiI < latitudeSegments; phiI++)
        {
            uint32_t rootVertex = thetaVertStart;
            uint32_t quadIndices[4] =
            {
                thetaVertStart + phiI,
                thetaVertStart + phiI + 1,
                thetaVertStart + latitudeVertexCount + phiI,
                thetaVertStart + latitudeVertexCount + phiI + 1,
            };

            mesh.Indices[indexWriter + 0] = (uint16_t)quadIndices[0];
            mesh.Indices[indexWriter + 1] = (uint16_t)quadIndices[1];
            mesh.Indices[indexWriter + 2] = (uint16_t)quadIndices[2];

            mesh.Indices[indexWriter + 3] = (uint16_t)quadIndices[1];
            mesh.Indices[indexWriter + 4] = (uint16_t)quadIndices[3];
            mesh.Indices[indexWriter + 5] = (uint16_t)quadIndices[2];

            indexWriter += 6;
        }
    }

    ib_assert(vertexWriter == vertexCount);
    ib_assert(indexWriter == segmentIndexCount);

    return mesh;
}

static MeshDesc createPlane(float width)
{
    MeshDesc mesh = {0};
    uint32_t quadCount = 1u;
    uint32_t indexCount = quadCount * 6;
    uint32_t vertexCount = quadCount * 4;

    mesh.Indices = (uint16_t*)transientAlloc(indexCount * sizeof(uint16_t), alignof(uint16_t));
    mesh.Positions = (c16i3*)transientAlloc(vertexCount * sizeof(c16i3), alignof(c16i3));
    mesh.Normals = (c16i2*)transientAlloc(vertexCount * sizeof(c16i2), alignof(c16i2));
    mesh.UVs = (cv2*)transientAlloc(vertexCount * sizeof(cv2), alignof(cv2));
    mesh.IndexCount = indexCount;
    mesh.VertexCount = vertexCount;

    float halfWidth = width * 0.5f;
    cv3 pos[] =
    {
        (cv3) { -halfWidth, -halfWidth, 0.0f },
        (cv3) { halfWidth, -halfWidth, 0.0f },
        (cv3) { -halfWidth, halfWidth, 0.0f },
        (cv3) { halfWidth, halfWidth, 0.0f },
    };

    cv2 uv[] =
    {
        (cv2) { 0.0f, 0.0f },
        (cv2) { 1.0f, 0.0f },
        (cv2) { 0.0f, 1.0f },
        (cv2) { 1.0f, 1.0f },
    };

    uint16_t quadIndices[6] = { 0, 1, 3, 0, 3, 2 };
    for (uint32_t i = 0; i < 6; i++)
    {
        mesh.Indices[i] = quadIndices[i];
    }

    for (uint32_t i = 0; i < 4; i++)
    {
        mesh.Positions[i] = packPosition(pos[i]);
        mesh.Normals[i] = packNormal((cv3){0.0f, 0.0f, 1.0f});
        mesh.UVs[i] = uv[i];
    }

    return mesh;
}

static MeshDesc createBox(float width)
{
    MeshDesc mesh = {0};
    uint32_t quadCount = 6u;
    uint32_t indexCount = quadCount * 6;
    uint32_t vertexCount = quadCount * 4;

    mesh.Indices = (uint16_t*)transientAlloc(indexCount * sizeof(uint16_t), alignof(uint16_t));
    mesh.Positions = (c16i3*)transientAlloc(vertexCount * sizeof(c16i3), alignof(c16i3));
    mesh.Normals = (c16i2*)transientAlloc(vertexCount * sizeof(c16i2), alignof(c16i2));
    mesh.UVs = (cv2*)transientAlloc(vertexCount * sizeof(cv2), alignof(cv2));
    mesh.IndexCount = indexCount;
    mesh.VertexCount = vertexCount;

    float halfWidth = width * 0.5f;
    cv3 pos[] =
    {
        (cv3) { -halfWidth, -halfWidth, -halfWidth },
        (cv3) { halfWidth, -halfWidth, -halfWidth },
        (cv3) { -halfWidth, halfWidth, -halfWidth },
        (cv3) { halfWidth, halfWidth, -halfWidth },

        (cv3) { -halfWidth, -halfWidth, halfWidth },
        (cv3) { halfWidth, -halfWidth, halfWidth },
        (cv3) { -halfWidth, halfWidth, halfWidth },
        (cv3) { halfWidth, halfWidth, halfWidth },
    };

    cv2 uv[] =
    {
        (cv2) { 0.0f, 0.0f },
        (cv2) { 1.0f, 0.0f },
        (cv2) { 0.0f, 1.0f },
        (cv2) { 1.0f, 1.0f },

        (cv2) { 0.0f, 1.0f },
        (cv2) { 1.0f, 1.0f },
        (cv2) { 0.0f, 0.0f },
        (cv2) { 1.0f, 0.0f },
    };

    uint16_t quadIndices[] =
    {
        // Front face
        0, 1, 3, 0, 3, 2,
        // Back face
        4, 7, 5, 4, 6, 7,
        // Left Face
        4, 0, 2, 4, 2, 6,
        // Right Face
        1, 5, 7, 1, 7, 3,
        // Top Face
        2, 3, 7, 2, 7, 6,
        // Bottom Face
        4, 5, 1, 4, 1, 0
    };
    for (uint32_t i = 0; i < ib_arrayCount(quadIndices); i++)
    {
        mesh.Indices[i] = quadIndices[i];
    }

    for (uint32_t i = 0; i < ib_arrayCount(pos); i++)
    {
        mesh.Positions[i] = packPosition(pos[i]);
        mesh.Normals[i] = packNormal(cv3_normalize(pos[i]));
        mesh.UVs[i] = uv[i];
    }

    return mesh;
}

typedef struct
{
    iba_TlsfAllocation Alloc;
    uint32_t IndexCount;
    uint32_t VertexCount;
} Mesh;

static Mesh allocMesh(MeshDesc desc)
{
    Mesh mesh;

    uint32_t indexSize = sizeof(uint16_t) * desc.IndexCount;
    uint32_t positionSize = sizeof(c16i3) * desc.VertexCount;
    uint32_t normalSize = sizeof(c16i2) * desc.VertexCount;
    uint32_t uvSize = sizeof(cv2) * desc.VertexCount;
    uint32_t allocationSize = indexSize + positionSize + normalSize + uvSize;
    mesh.Alloc = iba_tlsfAlloc(&GlobalBufferMemoryAllocator, allocationSize, alignof(float));

    uint32_t writeOffset = (uint32_t)mesh.Alloc.Offset;
    memcpy(GlobalBufferMemory.Allocation.CPUMemory + writeOffset, desc.Indices, indexSize);
    writeOffset += indexSize;

    memcpy(GlobalBufferMemory.Allocation.CPUMemory + writeOffset, desc.Positions, positionSize);
    writeOffset += positionSize;

    memcpy(GlobalBufferMemory.Allocation.CPUMemory + writeOffset, desc.Normals, normalSize);
    writeOffset += normalSize;

    // Align on 4 byte boundary.
    uint32_t alignementMask = sizeof(float) - 1;
    writeOffset = (writeOffset + alignementMask) & ~alignementMask;
    memcpy(GlobalBufferMemory.Allocation.CPUMemory + writeOffset, desc.UVs, uvSize);

    mesh.IndexCount = desc.IndexCount;
    mesh.VertexCount = desc.VertexCount;

    return mesh;
}

static void freeMesh(Mesh* mesh)
{
    iba_tlsfFree(&GlobalBufferMemoryAllocator, mesh->Alloc.Block);
}

static void compileShader(char const* shader, char const *shaderOutput, char const* entryPoint, char const* shaderType)
{
    char shaderCompilation[1024];
    snprintf(shaderCompilation, ib_arrayCount(shaderCompilation),
        "py %s/../Assets/compile_shaders.py -i %s/../Assets/Shaders/%s -e %s -t %s -o %s/../CompiledAssets/Shaders/%s \n",
        CurrentWorkingDirectory, CurrentWorkingDirectory, shader, entryPoint, shaderType, CurrentWorkingDirectory, shaderOutput);
    printf("Running: %s\n", shaderCompilation);
    system(shaderCompilation);
}

typedef struct
{
    cv2 OutputDimensions;
    uint64_t MeshAddress;
    uint64_t StackAddress;
    uint64_t RasterTileAddress;
    uint32_t IndexCount;
    uint32_t VertexCount;
} RasterizerParams;

enum
{
    Rasterizer_Params = 0,
    Rasterizer_Input,
    Rasterizer_Samplers,
    Rasterizer_Output,
};

ib_ShaderInputDesc const RasterizerInputs[] =
{
    [Rasterizer_Params] = { .Shaders = VK_SHADER_STAGE_COMPUTE_BIT, .Type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER },
    [Rasterizer_Input] = { .Shaders = VK_SHADER_STAGE_COMPUTE_BIT, .Type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE },
    [Rasterizer_Samplers] = { .Shaders = VK_SHADER_STAGE_COMPUTE_BIT, .Type = VK_DESCRIPTOR_TYPE_SAMPLER, .UseImmutableSamplers = true },
    [Rasterizer_Output] = { .Shaders = VK_SHADER_STAGE_COMPUTE_BIT, .Type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE },
};

ib_ComputePipeline RasterizerCompute;

typedef struct
{
    cm4 ProjectionFromWorld;
    cv2 OutputDimensions;
    uint64_t MeshAddress;
    uint64_t StackAddress;
    uint64_t RasterTileAddress;
    cu2 TileCount;
    cv2 InvTileDims;
    uint32_t IndexCount;
    uint32_t VertexCount;
} TriangleCullingParams;

enum
{
    TriangleCulling_Params = 0,
};

ib_ShaderInputDesc const TriangleCullingInputs[] =
{
    [TriangleCulling_Params] = { .Shaders = VK_SHADER_STAGE_COMPUTE_BIT, .Type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER },
};

ib_ComputePipeline TriangleCullingCompute;

Mesh SphereMesh;
Mesh PlaneMesh;
Mesh BoxMesh;

ibr_RenderGraph* beginRenderGraph(ib_Surface* surface)
{
    static uint32_t activeFrame = 0;
    ibr_RenderGraph* graph = &Graphs[activeFrame]; 
    bool frameBegun = ibr_beginFrame(graph, (ibr_BeginFrameDesc)
                   {
                       .FrameIndex = activeFrame,
                       .Surface = surface
                   });

    activeFrame = (activeFrame + 1) % ib_FramebufferCount;
    return frameBegun ? graph : NULL;
}

void loadShaders()
{
    compileShader("triangle_culling.hlsl", "triangle_culling.spv", "CS", "compute");
    compileShader("rasterizer.hlsl", "rasterizer.spv", "CS", "compute");

    void* rasterizerSpv;
    size_t rasterizerSpvSize;
    readWholeFile("../CompiledAssets/Shaders/rasterizer.spv", &rasterizerSpv, &rasterizerSpvSize);
    ib_freeComputePipeline(&Core, &RasterizerCompute);
    RasterizerCompute = ib_allocComputePipeline(&Core, (ib_ComputePipelineDesc)
                            {
                                .ShaderDesc = (ib_ShaderDesc)
                                {
                                    .EntryPoint = "CS",
                                    .Code = rasterizerSpv,
                                    .CodeSize = rasterizerSpvSize,
                                    .Stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                },
                                .ShaderInputs =
                                {
                                    { ib_staticArrayRange(RasterizerInputs) }
                                }
                            });

    void* cullingSpv;
    size_t cullingSpvSize;
    readWholeFile("../CompiledAssets/Shaders/triangle_culling.spv", &cullingSpv, &cullingSpvSize);
    ib_freeComputePipeline(&Core, &TriangleCullingCompute);
    TriangleCullingCompute = ib_allocComputePipeline(&Core, (ib_ComputePipelineDesc)
                            {
                                .ShaderDesc = (ib_ShaderDesc)
                                {
                                    .EntryPoint = "CS",
                                    .Code = cullingSpv,
                                    .CodeSize = cullingSpvSize,
                                    .Stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                },
                                .ShaderInputs =
                                {
                                    { ib_staticArrayRange(TriangleCullingInputs) }
                                }
                            });
}

ibr_DefaultResources DefaultResources;
static void init(void)
{
    stm_setup();

    GetCurrentDirectoryA(ib_arrayCount(CurrentWorkingDirectory), CurrentWorkingDirectory);

    ib_initCore((ib_CoreDesc)
                {
                    .Win32MainWindowHandle = sapp_win32_get_hwnd(),
                    .Win32MainInstanceHandle = GetModuleHandle(NULL)
                },
                &Core);

    ibr_initRenderGraphs(&Core, Graphs, ib_arrayCount(Graphs));
    Surface = ib_allocWin32Surface(&Core, (ib_SurfaceDesc)
                                   {
                                       .Win32WindowHandle = sapp_win32_get_hwnd(),
                                       .Win32InstanceHandle = GetModuleHandle(NULL),
                                       .UseVSync = true,
                                       .SRGB = true
                                   });

    GlobalBufferMemory = ib_allocBuffer(&Core, (ib_BufferDesc)
                   {
                       .Usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                       .Size = GlobalBufferMemorySize,
                       .RequiredMemoryFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                       .DebugName = "Global Buffer",
                       .AllocationType = iba_GpuAllocationType_Device,
                   });
    iba_initTlsfAllocator(&GlobalBufferMemoryAllocator);
    iba_tlsfAddRoot(&GlobalBufferMemoryAllocator, 0u, GlobalBufferMemorySize);

    // Use this stack allocator during init for any temporary memory.
    // Might use frame 0's stack allocator instead in the future.
    size_t const transientStackPageSize = 1024u * 1024u;
    iba_initCpuStackAllocator((iba_CpuStackAllocatorDesc)
                           {
                               .PageSize = transientStackPageSize
                           },
                           &StackAllocator);

    MeshDesc sphereMeshDesc = createSphereMesh(16, 16, 1.0f);
    SphereMesh = allocMesh(sphereMeshDesc);

    MeshDesc planeMeshDesc = createPlane(1.0f);
    PlaneMesh = allocMesh(planeMeshDesc);

    MeshDesc boxMeshDesc = createBox(1.0f);
    BoxMesh = allocMesh(boxMeshDesc);

    loadShaders();

    imgui_init(&Core, Surface.Format.format);

    // Spin up a graph for GPU-side initialization
    ibr_RenderGraph* graph = beginRenderGraph(NULL);
    if (graph != NULL)
    {
        VkCommandBuffer commands = ibr_allocTransientCommandBuffer(graph, ib_Queue_Graphics);
        ib_beginCommandBuffer(&Core, commands);
        ibr_beginUpload(graph, commands);

        ibr_initDefaultResources(graph, commands, &DefaultResources);

        ib_vkCheck(vkEndCommandBuffer(commands));

        ibr_submitCommandBuffers(graph, (ibr_SubmitCommandBufferDesc)
                                 {
                                     .Queue = ib_Queue_Graphics,
                                     .CommandBuffers = commands,
                                     .SubmitFence = graph->FrameFence
                                 });

        ibr_endUpload(graph);
        ibr_endFrame(graph);
    }
}

static void kill(void)
{
    iba_killStackAllocator(&StackAllocator);
    vkDeviceWaitIdle(Core.LogicalDevice);

    imgui_kill();

    freeMesh(&SphereMesh);
    freeMesh(&PlaneMesh);
    freeMesh(&BoxMesh);
    ib_freeComputePipeline(&Core, &RasterizerCompute);
    ib_freeComputePipeline(&Core, &TriangleCullingCompute);
    ibr_killDefaultResources(&Core, &DefaultResources);

    iba_killTlsfAllocator(&GlobalBufferMemoryAllocator);
    ib_freeBuffer(&Core, &GlobalBufferMemory);
    ib_freeSurface(&Core, &Surface);
    ibr_killRenderGraphs(&Core, Graphs, ib_arrayCount(Graphs));
    ib_killCore(&Core);
}

static void update(void)
{
    iba_stackReset(&StackAllocator);

    ibr_RenderGraph* graph = beginRenderGraph(&Surface); 
    if (graph != NULL)
    {
        imgui_beginFrame();

        if (igBegin("Main Window", NULL, ImGuiWindowFlags_None))
        {
            igCheckbox("Pause Time", &PauseTime);

            if (igButton("Reload Shaders", (ImVec2_c) { 0, 0 }))
            {
                vkDeviceWaitIdle(Core.LogicalDevice);
                loadShaders();
            }

            for (ibr_TransientScopeTiming* iter = graph->PreviousFrameTimings;
                iter != NULL;
                iter = iter->Next)
            {
                igLabelText("", "%s: %f", iter->Timing.Name, iter->Timing.Timing);
            }
        }
        igEnd();

        VkCommandBuffer commands = ibr_allocTransientCommandBuffer(graph, ib_Queue_Graphics);
        ib_beginCommandBuffer(&Core, commands);
        ibr_beginUpload(graph, commands);

        static uint64_t currentTime = 0u;
        static float rotation = 0.0f;

        uint64_t lapTime = stm_laptime(&currentTime);
        float deltaTime = (float)stm_sec(lapTime);
        if (!PauseTime)
        {
            rotation += deltaTime;
        }

        typedef struct
        {
            cv4 Verts[3];
            uint32_t TriangleIndex;
        } NDCTri;
        uint32_t triangleCount = SphereMesh.IndexCount / 3;
        ibr_Resource gpuStack = ibr_allocPassResource(graph, commands,
                ibr_transientBufferResourceDesc(1024u * 1024u, ibr_TransientBufferFlag_Device | ibr_TransientBufferFlag_StorageBufferBit, "GPUStack"));

        ibr_writeResource(graph, commands, &gpuStack, (ibr_WriteData)
                          {
                              .Data = &(uint32_t){ 0u },
                              .Size = sizeof(uint32_t)
                          });


        cu2 rasterTileCount =
        {
            // TODO: Reference shader dimensions
            cu_div_ceil(graph->ScreenExtent.width, 8u), cu_div_ceil(graph->ScreenExtent.height, 8u)
        };

        size_t rasterBatchPtrSize = sizeof(uint32_t) * rasterTileCount.x * rasterTileCount.y;
        ibr_Resource rasterBatches = ibr_allocPassResource(graph, commands,
                ibr_transientBufferResourceDesc(rasterBatchPtrSize, ibr_TransientBufferFlag_Device | ibr_TransientBufferFlag_StorageBufferBit, "RasterBatches"));
        
        uint64_t* rasterBatchData = (uint32_t*)transientAlloc(rasterBatchPtrSize, sizeof(uint32_t));
        memset(rasterBatchData, 0u, rasterBatchPtrSize);
        ibr_writeResource(graph, commands, &rasterBatches, (ibr_WriteData)
                          {
                              .Data = rasterBatchData,
                              .Size = rasterBatchPtrSize
                          });

        // Triangle Culling
        {
            ibr_Resource cullingParams = ibr_allocPassResource(graph, commands,
                ibr_transientBufferResourceDesc(sizeof(TriangleCullingParams), ibr_TransientBufferFlag_Device | ibr_TransientBufferFlag_UniformBufferBit, "CullingParams"));

            float aspectRatio = (float)graph->ScreenExtent.width / (float)graph->ScreenExtent.height;
            cm4 projection = cm4_perspective_projection(tanf(cran_pi / 4.0f), 0.1f, 1000.0f, aspectRatio);
            cm4 view = cm4_translate((cv3) { 0.0f, 0.0f, 2.0f });
            cm4 rotate = cm4_mul(cm4_rotate_xz(rotation), cm4_rotate_xy(rotation));
            cm4 projectionFromWorld = cm4_mul(cm4_mul(projection, view), rotate);

            ibr_writeResource(graph, commands, &cullingParams, (ibr_WriteData)
                          {
                              .Data = &(TriangleCullingParams)
                              {
                                  .OutputDimensions = (cv2) { (float)graph->ScreenExtent.width, (float)graph->ScreenExtent.height },
                                  .MeshAddress = GlobalBufferMemory.DeviceAddress + SphereMesh.Alloc.Offset,
                                  .StackAddress = gpuStack.Buffer->DeviceAddress,
                                  .RasterTileAddress = rasterBatches.Buffer->DeviceAddress,
                                  .TileCount = rasterTileCount,
                                  .InvTileDims = (cv2){1.0f / 8.0f, 1.0f / 8.0f },
                                  .IndexCount = SphereMesh.IndexCount,
                                  .VertexCount = SphereMesh.VertexCount,
                                  .ProjectionFromWorld = projectionFromWorld,
                              },
                              .Size = sizeof(TriangleCullingParams)
                          });

            ib_ShaderInput cullingInput = ibr_resourcesToShaderInput(graph, (ibr_ResourceToShaderInputDesc)
                                   {
                                       .Layout = &TriangleCullingCompute.InlineShaderInputLayouts[0],
                                       .ShaderInputs = ib_staticArrayRange(TriangleCullingInputs),
                                       .Resources = ib_staticArrayRange((ibr_Resource*[])
                                       {
                                           [TriangleCulling_Params] = &cullingParams,
                                       })
                                   });

            ibr_beginComputePass(graph, commands, (ibr_BeginComputePassDesc)
                                 {
                                     .ResourceStates =
                                     {
                                         ibr_bufferState(&cullingParams, ibr_BufferState_Read, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT),
                                         ibr_bufferState(&gpuStack, ibr_BufferState_ReadWrite, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT),
                                         ibr_bufferState(&rasterBatches, ibr_BufferState_ReadWrite, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT),
                                     },
                                     .PassName = "TriangleCulling"
                                 });
            ib_bindShaderInputToCompute(commands, &TriangleCullingCompute, &cullingInput);
            vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, TriangleCullingCompute.VulkanPipeline);
            vkCmdDispatch(commands, cu_div_ceil(triangleCount, 64u), 1u, 1u);
            ibr_endComputePass(graph, commands);
        }

        ibr_Resource swapchainResource = ibr_allocPassResource(graph, commands,
            ibr_textureResourceDesc(graph->SwapchainTexture, VK_IMAGE_LAYOUT_UNDEFINED));
        ibr_Resource inputTexture = ibr_allocPassResource(graph, commands,
            ibr_textureResourceDesc(&DefaultResources.Textures[ibr_DefaultTexture_Checkerboard], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
        ibr_Resource renderOutput = ibr_allocPassResource(graph, commands,
            ibr_transientTextureResourceDesc(ibr_ScreenExtent, VK_FORMAT_R8G8B8A8_UNORM, ibr_TransientTextureFlag_StorageBit | ibr_TransientTextureFlag_TransferSrcBit, "Render Output"));

        // Rasterize
        {
            ibr_Resource rasterizerParams = ibr_allocPassResource(graph, commands,
                                                                  ibr_transientBufferResourceDesc(sizeof(RasterizerParams), ibr_TransientBufferFlag_Device | ibr_TransientBufferFlag_UniformBufferBit, "RasterizerParams"));

            ibr_writeResource(graph, commands, &rasterizerParams, (ibr_WriteData)
                              {
                                  .Data = &(RasterizerParams)
                                  {
                                      .OutputDimensions = (cv2) { (float)graph->ScreenExtent.width, (float)graph->ScreenExtent.height },
                                      .MeshAddress = GlobalBufferMemory.DeviceAddress + SphereMesh.Alloc.Offset,
                                      .StackAddress = gpuStack.Buffer->DeviceAddress,
                                      .RasterTileAddress = rasterBatches.Buffer->DeviceAddress,
                                      .IndexCount = SphereMesh.IndexCount,
                                      .VertexCount = SphereMesh.VertexCount,
                                  },
                                  .Size = sizeof(RasterizerParams)
                              });

            ibr_beginComputePass(graph, commands, (ibr_BeginComputePassDesc)
                                 {
                                     .ResourceStates =
                                     {
                                         ibr_textureState(&renderOutput, ibr_TextureState_ReadWrite, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT),
                                         ibr_textureState(&inputTexture, ibr_TextureState_Read, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT),
                                         ibr_bufferState(&rasterizerParams, ibr_BufferState_Read, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT),
                                         ibr_bufferState(&gpuStack, ibr_BufferState_ReadWrite, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT),
                                         ibr_bufferState(&rasterBatches, ibr_BufferState_Read, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT),
                                     },
                                     .PassName = "Rasterizer"
                                 });

            ib_ShaderInput rasterizerInput = ibr_resourcesToShaderInput(graph, (ibr_ResourceToShaderInputDesc)
                                                                        {
                                                                            .Layout = &RasterizerCompute.InlineShaderInputLayouts[0],
                                                                            .ShaderInputs = ib_staticArrayRange(RasterizerInputs),
                                                                            .Resources = ib_staticArrayRange((ibr_Resource*[])
                                                                                                             {
                                                                                                                 [Rasterizer_Params] = &rasterizerParams,
                                                                                                                 [Rasterizer_Input] = &inputTexture,
                                                                                                                 [Rasterizer_Output] = &renderOutput,
                                                                                                             })
                                                                        });

            ib_bindShaderInputToCompute(commands, &RasterizerCompute, &rasterizerInput);
            vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, RasterizerCompute.VulkanPipeline);
            vkCmdDispatch(commands, rasterTileCount.x, rasterTileCount.y, 1u);
            ibr_endComputePass(graph, commands);
        }

        ibr_beginTransferPass(graph, commands, (ibr_BeginTransferPassDesc)
                             {
                                 .ResourceStates =
                                 {
                                    ibr_textureState(&renderOutput, ibr_TextureState_TransferSrc, VK_PIPELINE_STAGE_TRANSFER_BIT),
                                    ibr_textureState(&swapchainResource, ibr_TextureState_TransferDst, VK_PIPELINE_STAGE_TRANSFER_BIT)
                                 },
                                 .PassName = "CopyToSwapchain"
                             });

        vkCmdCopyImage2(commands, &(VkCopyImageInfo2)
                        {
                            .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2,
                            .srcImage = renderOutput.Texture->Image,
                            .srcImageLayout = renderOutput.TextureLayout,
                            .dstImage = swapchainResource.Texture->Image,
                            .dstImageLayout = swapchainResource.TextureLayout,
                            .regionCount = 1,
                            .pRegions = &(VkImageCopy2)
                            {
                                .sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2,
                                .srcSubresource = (VkImageSubresourceLayers)
                                {
                                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                    .layerCount = 1u,
                                },
                                .dstSubresource = (VkImageSubresourceLayers)
                                {
                                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                    .layerCount = 1u,
                                },
                                .extent = renderOutput.Texture->Extent
                            }
                        });

        ibr_endTransferPass(graph, commands);

        imgui_endFrame();

        ibr_barriers(graph, commands, (ibr_BarriersDesc)
        {
            ibr_textureState(&swapchainResource, ibr_TextureState_RenderTarget, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT)
        });

        imgui_render(commands, graph->ScreenExtent, swapchainResource.Texture);

        ibr_barriers(graph, commands, (ibr_BarriersDesc)
                     {
                         ibr_textureToPresentState(&swapchainResource)
                     });

        ib_vkCheck(vkEndCommandBuffer(commands));

        ibr_submitCommandBuffers(graph, (ibr_SubmitCommandBufferDesc)
                                 {
                                     .Queue = ib_Queue_Graphics,
                                     .CommandBuffers = commands,
                                     .WaitSemaphores = (ibr_SemaphoreSubmit){ graph->SwapchainAcquireSemaphore },
                                     .SignalSemaphores = (ibr_SemaphoreSubmit){ graph->FrameSemaphore },
                                     .SubmitFence = graph->FrameFence
                                 });

        ibr_present((ibr_PresentDesc) { &Surface, graph });
        ibr_endUpload(graph);
        ibr_endFrame(graph);
    }
}

void events(sapp_event const* event)
{
    if (event->type == SAPP_EVENTTYPE_RESIZED)
    {
        ib_rebuildSurface(&Core, &Surface);
    }
    else if (event->type == SAPP_EVENTTYPE_KEY_DOWN && event->key_code == SAPP_KEYCODE_R)
    {
        vkDeviceWaitIdle(Core.LogicalDevice);
        loadShaders();
    }
    else if (event->type == SAPP_EVENTTYPE_KEY_DOWN && event->key_code == SAPP_KEYCODE_SPACE)
    {
        PauseTime = !PauseTime;
    }
}

sapp_desc sokol_main(int argc, char* argv[]) {
    return (sapp_desc) {
        .init_cb = &init,
        .frame_cb = &update,
        .cleanup_cb = &kill,
        .event_cb = &events,
        .win32.console_attach = true
    };
}
