#define SOKOL_IMPL
#define SOKOL_NOAPI
#include <sokol/sokol_app.h>

#include <cranberries/cranberry_math.h>

#include <iceberg/ib_core.h>
#include <iceberg/ib_rendergraph.h>

#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

iba_StackAllocator StackAllocator = { 0 };

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
    uint32_t IndexCount;
    uint32_t PositionCount;
    uint32_t NormalCount;
} MeshDesc;

static size_t const GlobalBufferMemorySize = 1024u * 1024u * 32u; // 32 MB of global buffer memory.
static ib_Buffer GlobalBufferMemory;
static iba_TlsfAllocator GlobalBufferMemoryAllocator;

// Quantization: https://cbloomrants.blogspot.com/2020/09/topics-in-quantization-for-games.html
static float const PosFixedPointScale = 128.0f;
static c16i3 packPosition(cv3 pos)
{
    // Rescale such that position is fixed point 8.7
    // As a result, 1 is 1/256 and 256 is 1
    pos = cv3_mulf(pos, PosFixedPointScale);
    return cv3_to_c16i3(pos);
}

static float const NormalFixedPointScale = 32768.0f;
static c16i2 packNormal(cv3 normal)
{
    // Rescale such that position is fixed point 16.16
    // As a result, 1 is 1/256 and 256 is 1
    cv2 octahedral = cv3_to_octahedral(normal);
    octahedral = cv2_mulf(octahedral, NormalFixedPointScale);
    return cv2_to_c16i2(octahedral);
}

static MeshDesc createSphereMesh(uint32_t latitudeSegments, uint32_t longitudeSegments, float radius)
{
    MeshDesc mesh = {0};
    uint32_t segmentQuadCount = latitudeSegments * longitudeSegments;
    uint32_t segmentIndexCount = segmentQuadCount * 6;
    uint32_t segmentVertexCount = segmentQuadCount * 4;

    mesh.Indices = (uint16_t*)transientAlloc(segmentIndexCount * sizeof(uint16_t), alignof(uint16_t));
    mesh.Positions = (c16i3*)transientAlloc(segmentVertexCount * sizeof(c16i3), alignof(c16i3));
    mesh.Normals = (c16i2*)transientAlloc(segmentVertexCount * sizeof(c16i2), alignof(c16i2));
    mesh.IndexCount = segmentIndexCount;
    mesh.PositionCount = segmentVertexCount;
    mesh.NormalCount = segmentVertexCount;

    uint32_t indexWriter = 0;
    uint32_t vertexWriter = 0;

    float deltaTheta = cran_pi * cf_rcp((float)longitudeSegments);
    float deltaPhi = cran_tao * cf_rcp((float)latitudeSegments);
    float theta = 0.0f;
    for (uint32_t thetaI = 0; thetaI < longitudeSegments; theta += deltaTheta, thetaI++)
    {
        float phi = 0.0f;
        for (uint32_t phiI = 0; phiI < latitudeSegments; phi += deltaPhi, phiI++)
        {
            cv3 pos[] =
            {
                cv3_from_spherical(theta, phi, 1.0f),
                cv3_from_spherical(theta, phi + deltaPhi, 1.0f),
                cv3_from_spherical(theta + deltaTheta, phi, 1.0f),
                cv3_from_spherical(theta + deltaTheta, phi + deltaPhi, 1.0f),
            };

            uint16_t quadIndices[6] = { 0, 2, 3, 0, 3, 1 };
            for (uint32_t i = 0; i < 6; i++)
            {
                mesh.Indices[indexWriter + i] = quadIndices[i] + vertexWriter;
            }

            for (uint32_t i = 0; i < 4; i++)
            {
                mesh.Positions[vertexWriter + i] = packPosition(cv3_mulf(pos[i], radius));
                mesh.Normals[vertexWriter + i] = packNormal(pos[i]);
            }

            vertexWriter += 4;
            indexWriter += 6;
        }
    }
    ib_assert(vertexWriter == segmentVertexCount);
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
    mesh.IndexCount = indexCount;
    mesh.PositionCount = vertexCount;
    mesh.NormalCount = vertexCount;

    float halfWidth = width * 0.5f;
    cv3 pos[] =
    {
        (cv3) { -halfWidth, -halfWidth, 0.0f },
        (cv3) { halfWidth, -halfWidth, 0.0f },
        (cv3) { -halfWidth, halfWidth, 0.0f },
        (cv3) { halfWidth, halfWidth, 0.0f },
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
    }

    return mesh;
}

typedef struct
{
    iba_TlsfAllocation Alloc;
    uint32_t IndexOffset;
    uint32_t PositionOffset;
    uint32_t NormalOffset;
    uint32_t IndexCount;
    uint32_t VertexCount;
} Mesh;

static Mesh allocMesh(MeshDesc desc)
{
    Mesh mesh;

    ib_assert(desc.PositionCount == desc.NormalCount);

    uint32_t indexSize = sizeof(uint16_t) * desc.IndexCount;
    uint32_t positionSize = sizeof(c16i3) * desc.PositionCount;
    uint32_t normalSize = sizeof(c16i2) * desc.NormalCount;
    uint32_t allocationSize = indexSize + positionSize + normalSize;
    mesh.Alloc = iba_tlsfAlloc(&GlobalBufferMemoryAllocator, allocationSize, alignof(float));
    mesh.IndexOffset = (uint32_t)mesh.Alloc.Offset;
    memcpy(GlobalBufferMemory.Allocation.CPUMemory + mesh.IndexOffset, desc.Indices, indexSize);
    mesh.PositionOffset = (uint32_t)(mesh.Alloc.Offset + indexSize);
    memcpy(GlobalBufferMemory.Allocation.CPUMemory + mesh.PositionOffset, desc.Positions, positionSize);
    mesh.NormalOffset = (uint32_t)(mesh.Alloc.Offset + indexSize + positionSize);
    memcpy(GlobalBufferMemory.Allocation.CPUMemory + mesh.NormalOffset, desc.Normals, normalSize);

    mesh.IndexCount = desc.IndexCount;
    mesh.VertexCount = desc.PositionCount;

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
        "py %s/../../Assets/compile_shaders.py -i %s/../../Assets/Shaders/%s -e %s -t %s -o %s/../../CompiledAssets/Shaders/%s \n",
        CurrentWorkingDirectory, CurrentWorkingDirectory, shader, entryPoint, shaderType, CurrentWorkingDirectory, shaderOutput);
    printf("Running: %s\n", shaderCompilation);
    system(shaderCompilation);
}

typedef struct
{
    cv2 OutputDimensions;
    uint64_t MeshAddress;
    uint32_t IndexCount;
    uint32_t VertexCount;
    cm4 ProjectionFromWorld;
} RasterizerParams;

enum
{
    Rasterizer_Params = 0,
    Rasterizer_Input,
    Rasterizer_Samplers,
    Rasterizer_Output
};

ib_ShaderInputDesc const RasterizerInputs[] =
{
    [Rasterizer_Params] = { .Index = Rasterizer_Params, .Shaders = VK_SHADER_STAGE_COMPUTE_BIT, .Type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER },
    [Rasterizer_Input] = { .Index = Rasterizer_Input, .Shaders = VK_SHADER_STAGE_COMPUTE_BIT, .Type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE },
    [Rasterizer_Samplers] = { .Index = Rasterizer_Samplers, .Shaders = VK_SHADER_STAGE_COMPUTE_BIT, .Type = VK_DESCRIPTOR_TYPE_SAMPLER, .UseImmutableSamplers = true },
    [Rasterizer_Output] = { .Index = Rasterizer_Output, .Shaders = VK_SHADER_STAGE_COMPUTE_BIT, .Type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE }
};

ib_ComputePipeline RasterizerCompute;

Mesh SphereMesh;
Mesh PlaneMesh;

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
    compileShader("rasterizer.hlsl", "rasterizer.spv", "CS", "compute");

    void* rasterizerSpv;
    size_t rasterizerSpvSize;
    readWholeFile("../../CompiledAssets/Shaders/rasterizer.spv", &rasterizerSpv, &rasterizerSpvSize);
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
}

ibr_DefaultResources DefaultResources;
static void init(void)
{
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

    MeshDesc sphereMeshDesc = createSphereMesh(4, 4, 1.0f);
    SphereMesh = allocMesh(sphereMeshDesc);

    MeshDesc planeMeshDesc = createPlane(1.0f);
    PlaneMesh = allocMesh(planeMeshDesc);

    loadShaders();

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
    freeMesh(&SphereMesh);
    freeMesh(&PlaneMesh);
    ib_freeComputePipeline(&Core, &RasterizerCompute);
    ibr_killDefaultResources(&Core, &DefaultResources);

    iba_killTlsfAllocator(&GlobalBufferMemoryAllocator);
    ib_freeBuffer(&Core, &GlobalBufferMemory);
    ib_freeSurface(&Core, &Surface);
    ibr_killRenderGraphs(&Core, Graphs, ib_arrayCount(Graphs));
    ib_killCore(&Core);
}

static void update(void)
{
    ibr_RenderGraph* graph = beginRenderGraph(&Surface); 
    if (graph != NULL)
    {
        VkCommandBuffer commands = ibr_allocTransientCommandBuffer(graph, ib_Queue_Graphics);
        ib_beginCommandBuffer(&Core, commands);
        ibr_beginUpload(graph, commands);

        ibr_Resource swapchainResource;
        ibr_Resource rasterizerParams;
        ibr_Resource inputTexture;
        ibr_Resource renderOutput;
        ibr_allocPassResources(graph, commands, (ibr_AllocPassResourcesDesc)
                               {
                                   .ResourceBindings =
                                   {
                                       (ibr_AllocResourceBinding)
                                       {
                                           .OutResource = &swapchainResource,
                                           .Desc = ibr_textureResourceDesc(graph->SwapchainTexture, VK_IMAGE_LAYOUT_UNDEFINED)
                                       },
                                       (ibr_AllocResourceBinding)
                                       {
                                           .OutResource = &inputTexture,
                                           .Desc = ibr_textureResourceDesc(&DefaultResources.Textures[ibr_DefaultTexture_Checkerboard], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                                       },
                                       (ibr_AllocResourceBinding)
                                       {
                                           .OutResource = &rasterizerParams,
                                           .Desc = (ibr_ResourceDesc)
                                           {
                                               .Type = ibr_ResourceType_Buffer,
                                               .Flags = ibr_ResourceFlag_Transient,
                                               .BufferDesc =
                                               {
                                                   .Usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                   .Size = sizeof(RasterizerParams),
                                                   .RequiredMemoryFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                                   .DebugName = "RasterizerParams",
                                               }
                                           }
                                       },
                                       (ibr_AllocResourceBinding)
                                       {
                                           .OutResource = &renderOutput,
                                           .Desc =
                                           {
                                               .Type = ibr_ResourceType_Texture,
                                               .Flags = ibr_ResourceFlag_Transient,
                                               .TextureDesc = (ib_TextureDesc)
                                               {
                                                   .Usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                                   .Format = VK_FORMAT_R8G8B8A8_UNORM,
                                                   .Extent = (VkExtent3D){ graph->ScreenExtent.width, graph->ScreenExtent.height },
                                                   .Aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                                                   .DebugName = "Render Output",
                                               }
                                           }
                                       },
                                   }
                               });


        float aspectRatio = (float)graph->ScreenExtent.width / (float)graph->ScreenExtent.height;
        cm4 projection = cm4_perspective_projection(tanf(cran_pi / 4.0f), 0.1f, 1000.0f, aspectRatio);
        cm4 view = cm4_translate((cv3) { 0.0f, 0.0f, 2.0f });
        cm4 projectionFromWorld = cm4_mul(projection, view);
        ibr_writeResource(graph, commands, &rasterizerParams, (ibr_WriteData)
                          {
                              .Data = &(RasterizerParams)
                              {
                                  .OutputDimensions = (cv2) { (float)graph->ScreenExtent.width, (float)graph->ScreenExtent.height },
                                  .MeshAddress = GlobalBufferMemory.DeviceAddress + SphereMesh.IndexOffset,
                                  .IndexCount = SphereMesh.IndexCount,
                                  .VertexCount = SphereMesh.VertexCount,
                                  .ProjectionFromWorld = projectionFromWorld,
                              },
                              .Size = sizeof(RasterizerParams)
                          });

        ibr_beginComputePass(graph, commands, (ibr_BeginComputePassDesc)
                              {
                                  .ResourceStates =
                                  {
                                      ibr_textureState(&renderOutput, ibr_TextureState_ReadWrite, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT),
                                      ibr_textureState(&inputTexture, ibr_TextureState_Read, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT),
                                      ibr_bufferState(&rasterizerParams, ibr_BufferState_Read, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)
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
                                           [Rasterizer_Output] = &renderOutput
                                       })
                                   });

        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE,
                                RasterizerCompute.Layout,
                                0u,
                                1u,
                                &rasterizerInput.DescriptorSet,
                                0u,
                                NULL);
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, RasterizerCompute.VulkanPipeline);
        vkCmdDispatch(commands, cu_div_ceil(graph->ScreenExtent.width, 8u), cu_div_ceil(graph->ScreenExtent.height, 8u), 1u);
        ibr_endComputePass(graph, commands);

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

    // Reset at the end of the frame,
    // this allows init to use the stack allocator as well
    // if ever we multi-thread, this should happen at the very end of the frame
    iba_stackReset(&StackAllocator);
}

void events(sapp_event const* event)
{
    if (event->type == SAPP_EVENTTYPE_RESIZED)
    {
        ib_rebuildSurface(&Core, &Surface);
    }
    if (event->key_code == SAPP_KEYCODE_R)
    {
        vkDeviceWaitIdle(Core.LogicalDevice);
        loadShaders();
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
