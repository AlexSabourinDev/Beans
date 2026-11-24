#include <LPP_API_x64_CPP.h>

#define SOKOL_IMPL
#define SOKOL_NOAPI
#include <sokol/sokol_app.h>
#include <sokol/sokol_time.h>

#include <iceberg/ib_core.h>
#include <iceberg/ib_rendergraph.h>

#include <cranberries/cranberry_math.h>

#include "ShaderHeaders/clear_color.h"
#include "ShaderHeaders/trace_voxels.h"

LppDefaultAgent LivePPAgent;
static bool initLivePP()
{
    LivePPAgent = LppCreateDefaultAgent(NULL, L"../../LivePP");
    bool isValidAgent = LppIsValidDefaultAgent(&LivePPAgent);
    assert(isValidAgent);

    if (isValidAgent)
    {
        LivePPAgent.EnableModule(LppGetCurrentModulePath(),  LPP_MODULES_OPTION_ALL_IMPORT_MODULES, NULL, NULL);
    }

    return isValidAgent;
}

static void killLivePP()
{
    LppDestroyDefaultAgent(&LivePPAgent);
}

// Transient allocator begin

static iba_StackAllocator StackAllocator = { 0 };
static void initStackAllocator()
{
    // Use this stack allocator during init for any temporary memory.
    // Might use frame 0's stack allocator instead in the future.
    size_t const transientStackPageSize = 1024u * 1024u;
    iba_initCpuStackAllocator((iba_CpuStackAllocatorDesc)
                           {
                               .PageSize = transientStackPageSize
                           },
                           &StackAllocator);
}

static void killStackAllocator()
{
    iba_killStackAllocator(&StackAllocator);
}

static void* transientAlloc(size_t size, size_t alignment)
{
    iba_StackAllocation allocation = iba_stackAlloc(&StackAllocator, (iba_StackAllocationRequest){ size, alignment });
    return iba_cpuStackAllocToMemory(allocation);
}

// Transient allocator end

ib_Core Core;
ib_Surface Surface;

ibr_DefaultResources DefaultResources;
ibr_RenderGraph Graphs[ib_FramebufferCount];

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

// Global buffer memory pool begin

static size_t const GlobalBufferMemorySize = 1024u * 1024u * 32u; // 32 MB of global buffer memory.
static ib_Buffer GlobalBufferMemory;
static iba_TlsfAllocator GlobalBufferMemoryAllocator;

static void initGlobalBufferMemory(ib_Core* core)
{
     GlobalBufferMemory = ib_allocBuffer(core, (ib_BufferDesc)
                   {
                       .Usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                       .Size = GlobalBufferMemorySize,
                       .RequiredMemoryFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                       .DebugName = "Global Buffer",
                       .AllocationType = iba_GpuAllocationType_Device,
                   });
    iba_initTlsfAllocator(&GlobalBufferMemoryAllocator);
    iba_tlsfAddRoot(&GlobalBufferMemoryAllocator, 0u, GlobalBufferMemorySize);
}

static void killGlobalBufferMemory(ib_Core* core)
{
    iba_killTlsfAllocator(&GlobalBufferMemoryAllocator);
    ib_freeBuffer(core, &GlobalBufferMemory);
}

// Global buffer memory pool end

// Mesh API Begin

typedef struct
{
    uint16_t* Indices;
    c16i3* Positions;
    c16i2* Normals;
    cv2* UVs;
    uint32_t IndexCount;
    uint32_t VertexCount;
} MeshDesc;

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

// Mesh API End

ib_ComputePipeline ClearColorShader;
ib_ComputePipeline TraceVoxelsShader;

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

static void loadShaders(void)
{
    void* clearColorSpv;
    size_t clearColorSpvSize;
    readWholeFile("CompiledAssets/Shaders/clear_color.spv", &clearColorSpv, &clearColorSpvSize);
    ClearColorShader = ib_allocComputePipeline(&Core, (ib_ComputePipelineDesc)
                            {
                                .ShaderDesc = (ib_ShaderDesc)
                                {
                                    .EntryPoint = "CS",
                                    .Code = clearColorSpv,
                                    .CodeSize = clearColorSpvSize,
                                    .Stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                },
                                .ShaderInputs =
                                {
                                    { ib_staticArrayRange(sg_ClearColorInputs) }
                                }
                            });

    void* traceVoxelsSpv;
    size_t traceVoxelsSpvSize;
    readWholeFile("CompiledAssets/Shaders/trace_voxels.spv", &traceVoxelsSpv, &traceVoxelsSpvSize);
    TraceVoxelsShader = ib_allocComputePipeline(&Core, (ib_ComputePipelineDesc)
                            {
                                .ShaderDesc = (ib_ShaderDesc)
                                {
                                    .EntryPoint = "CS",
                                    .Code = traceVoxelsSpv,
                                    .CodeSize = traceVoxelsSpvSize,
                                    .Stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                },
                                .ShaderInputs =
                                {
                                    { ib_staticArrayRange(sg_TraceVoxelsInputs) }
                                }
                            });
}

static void init(void)
{
    stm_setup();

    initLivePP();
    initStackAllocator();

    ib_initCore((ib_CoreDesc)
                {
                    .Win32MainWindowHandle = sapp_win32_get_hwnd(),
                    .Win32MainInstanceHandle = GetModuleHandle(NULL)
                },
                &Core);
    initGlobalBufferMemory(&Core);
    loadShaders();

    ibr_initRenderGraphs(&Core, Graphs, ib_arrayCount(Graphs));
    Surface = ib_allocWin32Surface(&Core, (ib_SurfaceDesc)
                                   {
                                       .Win32WindowHandle = sapp_win32_get_hwnd(),
                                       .Win32InstanceHandle = GetModuleHandle(NULL),
                                       .UseVSync = true,
                                       .SRGB = true
                                   });

    

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
    vkDeviceWaitIdle(Core.LogicalDevice);

    ibr_killRenderGraphs(&Core, Graphs, ib_arrayCount(Graphs));

    killGlobalBufferMemory(&Core);

    ib_freeComputePipeline(&Core, &ClearColorShader);
    ib_freeComputePipeline(&Core, &TraceVoxelsShader);
    ib_freeSurface(&Core, &Surface);
    ib_killCore(&Core);

    killStackAllocator();
    killLivePP();
}

typedef struct
{
    cq Rotation;
    cv3 Position;
    float Scale;
} Transform;

Transform const identityTransform =
{
    .Rotation = { 0.0f, 0.0f, 0.0f, 1.0f },
    .Scale = 1.0f
};

typedef struct
{
    Transform Transform;
    float FOV;
    float NearPlane;
    float FarPlane;
} CameraData;

typedef struct
{
    CameraData Camera;
} SceneFrameData;

static void renderScene(SceneFrameData scene)
{
    ibr_RenderGraph* graph = beginRenderGraph(&Surface);
    if (graph != NULL)
    {
        VkCommandBuffer commands = ibr_allocTransientCommandBuffer(graph, ib_Queue_Graphics);
        ib_beginCommandBuffer(&Core, commands);
        ibr_beginUpload(graph, commands);

        ibr_Resource swapchainResource = ibr_allocPassResource(graph, commands,
                                                               ibr_textureResourceDesc(graph->SwapchainTexture, VK_IMAGE_LAYOUT_UNDEFINED));

        ibr_Resource renderOutput = ibr_allocPassResource(graph, commands,
            ibr_transientTextureResourceDesc(ibr_ScreenExtent, VK_FORMAT_R8G8B8A8_UNORM, ibr_TransientTextureFlag_StorageBit | ibr_TransientTextureFlag_TransferSrcBit, "Render Output"));

        // Clear color renderpass
        {
            ibr_Resource clearColorConstants = ibr_allocTransientBufferResource(graph,
                                                                                commands,
                                                                                &(sg_ClearColorConstants)
                                                                                {
                                                                                    .OutputDimensions = { graph->ScreenExtent.width, graph->ScreenExtent.height },
                                                                                    .Color = cv4_to_c32u((cv4) { 1.0f, 1.0f, 0.0f, 1.0f })
                                                                                },
                                                                                sizeof(sg_ClearColorConstants), ibr_TransientBufferFlag_Device, "ClearColor Constants");

            ibr_beginComputePass(graph, commands, (ibr_BeginComputePassDesc)
                                 {
                                     .ResourceStates =
                                     {
                                         ibr_textureState(&renderOutput, ibr_TextureState_Write, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT),
                                         ibr_bufferState(&clearColorConstants, ibr_BufferState_Read, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT),
                                     }
                                 });

            ib_ShaderInput shaderData = ibr_resourcesToShaderInput(graph, (ibr_ResourceToShaderInputDesc)
                                                                   {
                                                                       .ShaderInputs = sg_ClearColorInputs,
                                                                       .Layout = &ClearColorShader.InlineShaderInputLayouts[0],
                                                                       .Resources = ib_staticArrayRange((ibr_Resource*[])
                                                                                                        {
                                                                                                            [ClearColor_Output] = &renderOutput,
                                                                                                            [ClearColor_Constants] = &clearColorConstants
                                                                                                        })
                                                                   });

            ib_bindShaderInputToCompute(commands, &ClearColorShader, &shaderData);
            vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, ClearColorShader.VulkanPipeline);
            vkCmdDispatch(commands, cu_div_ceil(graph->ScreenExtent.width, sg_ClearColorThreadGroupSize.x), cu_div_ceil(graph->ScreenExtent.height, sg_ClearColorThreadGroupSize.y), 1u);

            ibr_endComputePass(graph, commands);
        }

        // Trace voxels
        {
            float aspectRatio = (float)graph->ScreenExtent.width / (float)graph->ScreenExtent.height;
            cv4 projectionTerms = perspective_projection_terms(scene.Camera.FOV, scene.Camera.NearPlane, scene.Camera.FarPlane, aspectRatio);

            sg_VoxelTraceConstants constants;
            cq_store(constants.ViewToWorld.Rotation, scene.Camera.Transform.Rotation);
            cv3_store(constants.ViewToWorld.Position, scene.Camera.Transform.Position);
            constants.ViewToWorld.Scale = scene.Camera.Transform.Scale;

            cv4 invProjectionTerms = (cv4) { 1.0f / projectionTerms.x, -1.0f / projectionTerms.y, projectionTerms.z, projectionTerms.w };
            cv4_store(constants.InvProjection, invProjectionTerms);
            constants.InvOutputDimensions[0] = 1.0f / (float)graph->ScreenExtent.width;
            constants.InvOutputDimensions[1] = 1.0f / (float)graph->ScreenExtent.height;
            ibr_Resource traceVoxelsConstants = ibr_allocTransientBufferResource(graph,
                                                                                commands,
                                                                                &constants,
                                                                                sizeof(constants), ibr_TransientBufferFlag_Device, "TraceVoxels Constants");

            ibr_beginComputePass(graph, commands, (ibr_BeginComputePassDesc)
                                 {
                                     .ResourceStates =
                                     {
                                         ibr_textureState(&renderOutput, ibr_TextureState_Write, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT),
                                         ibr_bufferState(&traceVoxelsConstants, ibr_BufferState_Read, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT),
                                     }
                                 });

            ib_ShaderInput shaderData = ibr_resourcesToShaderInput(graph, (ibr_ResourceToShaderInputDesc)
                                                                   {
                                                                       .ShaderInputs = sg_TraceVoxelsInputs,
                                                                       .Layout = &TraceVoxelsShader.InlineShaderInputLayouts[0],
                                                                       .Resources = ib_staticArrayRange((ibr_Resource*[])
                                                                                                        {
                                                                                                            [TraceVoxels_Output] = &renderOutput,
                                                                                                            [TraceVoxels_Constants] = &traceVoxelsConstants
                                                                                                        })
                                                                   });

            ib_bindShaderInputToCompute(commands, &TraceVoxelsShader, &shaderData);
            vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, TraceVoxelsShader.VulkanPipeline);
            vkCmdDispatch(commands, cu_div_ceil(graph->ScreenExtent.width, sg_TraceVoxelsThreadGroupSize.x), cu_div_ceil(graph->ScreenExtent.height, sg_TraceVoxelsThreadGroupSize.y), 1u);

            ibr_endComputePass(graph, commands);
        }


        // Render output to swapchain
        {
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
        }

        ibr_barriers(graph, commands, (ibr_BarriersDesc)
                     {
                         ibr_textureToPresentState(&swapchainResource)
                     });

        ib_vkCheck(vkEndCommandBuffer(commands));

        ibr_submitCommandBuffers(graph, (ibr_SubmitCommandBufferDesc)
                                 {
                                     .Queue = ib_Queue_Graphics,
                                     .CommandBuffers = commands,
                                     .WaitSemaphores = (ibr_SemaphoreSubmit) { graph->SwapchainAcquireSemaphore },
                                     .SignalSemaphores = (ibr_SemaphoreSubmit) { graph->PresentSemaphore },
                                     .SubmitFence = graph->FrameFence
                                 });

        ibr_present((ibr_PresentDesc) { &Surface, graph });
        ibr_endUpload(graph);
        ibr_endFrame(graph);
    }
}

const Transform SceneCameraTransform =
{
    .Scale = 1.0f,
};

static void update(void)
{
    SceneFrameData sceneFrameData = (SceneFrameData){0};
    sceneFrameData.Camera.FOV = tanf(1.22173f * 0.5f);
    sceneFrameData.Camera.NearPlane = 0.1f;
    sceneFrameData.Camera.FarPlane = 1000.0f;
    sceneFrameData.Camera.Transform = SceneCameraTransform;
    sceneFrameData.Camera.Transform.Rotation = cq_axis_angle((cv3) { 0.0f, 1.0f, 0.0f }, (float)stm_sec(stm_now()) * 0.5f);
    renderScene(sceneFrameData);
}

static void events(sapp_event const* event)
{
    if (event->type == SAPP_EVENTTYPE_RESIZED)
    {
        ib_rebuildSurface(&Core, &Surface);
    }
    else if (event->type == SAPP_EVENTTYPE_KEY_DOWN)
    {
        if (event->key_code == SAPP_KEYCODE_R)
        {
            vkDeviceWaitIdle(Core.LogicalDevice);

            char CurrentWorkingDirectory[256];
            GetCurrentDirectoryA(ib_arrayCount(CurrentWorkingDirectory), CurrentWorkingDirectory);
            char shaderCompilation[1024];
            snprintf(shaderCompilation, ib_arrayCount(shaderCompilation), "py %s/../Python/compile_voxel_game_assets.py -only_shaders=True\n", CurrentWorkingDirectory);
            printf("Running: %s\n", shaderCompilation);
            system(shaderCompilation);

            loadShaders();
        }
    }
}

sapp_desc sokol_main(int argc, char* argv[]) {
    return (sapp_desc) {
        .init_cb = &init,
        .frame_cb = &update,
        .cleanup_cb = &kill,
        .event_cb = &events,
        .win32.console_attach = true,
        .window_title = "Voxel Game"
    };
}
