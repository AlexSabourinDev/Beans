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

#include <math.h>

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

// Voxel Page API

uint32_t spreadThirdBits_256(uint32_t bits)
{
    // Spread bits recursively by doing:
    // 012 345 67x xxx xxx xxx xxx xxx
    // 012 3xx xxx xxx 456 7xx xxx xxx
    // 01x xxx 23x xxx 45x xxx 67x xxx
    // 0xx 1xx 2xx 3xx 4xx 5xx 6xx 7xx

    // we can do this with shifts like:
    // x = ((x << 8) | x) & 0x00F00F
    // x = ((x << 4) | x) & 003030303
    // x = ((x << 2) | x) & 011111111

    // (Interestingly, octal makkes easier masks here)

    // But actually, we can use a multiply instead of a shift and or:
    bits = (bits * 0x0101) & 0x00F00F;
    bits = (bits * 0x11) & 003030303;
    bits = (bits * 0x5) & 011111111;

    return bits;
}

uint32_t spreadThirdBits_16(uint32_t bits)
{
    // Spread bits recursively by doing:
    // 012 3xx xxx xxx
    // 01x xxx 23x xxx
    // 0xx 1xx 2xx 3xx

    // we can do this with shifts like:
    // x = ((x << 4) | x) & 003030303
    // x = ((x << 2) | x) & 011111111

    // (Interestingly, octal makkes easier masks here)

    // But actually, we can use a multiply instead of a shift and or:
    bits = (bits * 0x11) & 00303;
    bits = (bits * 0x5) & 01111;
    return bits;
}

uint32_t zCurve3D_256(cu3 indices)
{
    return spreadThirdBits_256(indices.x)
        | (spreadThirdBits_256(indices.y) << 1u)
        | (spreadThirdBits_256(indices.z) << 2u);
}

uint32_t zCurve3D_16(cu3 indices)
{
    return spreadThirdBits_16(indices.x)
        | (spreadThirdBits_16(indices.y) << 1u)
        | (spreadThirdBits_16(indices.z) << 2u);
}

typedef struct Bitset
{
    uint64_t* Data;
    uint32_t Size;
} Bitset;

const uint32_t bitset_IndexToBitShift = 6u;
const uint32_t bitset_IndexBitMask = 63u;

void bitset_init(Bitset* bitset)
{
    memset(bitset->Data, 0, sizeof(uint64_t) * bitset->Size);
}

uint32_t bitset_alloc(Bitset* bitset)
{
    for (uint32_t i = 0; i < bitset->Size; i++)
    {
        uint64_t freeBits = ~bitset->Data[i];
        if (freeBits != 0u)
        {
            uint32_t allocIndex = ib_firstBitLowU64(freeBits);
            bitset->Data[i] = bitset->Data[i] | (1ull << allocIndex);
            return allocIndex + (i << bitset_IndexToBitShift);
        }
    }
    return UINT_MAX;
}

void bitset_set(Bitset* bitset, uint32_t index)
{
    uint32_t arrayIndex = index >> bitset_IndexToBitShift;
    uint32_t bitIndex = index & bitset_IndexBitMask;
    bitset->Data[arrayIndex] = bitset->Data[arrayIndex] | (1ull << bitIndex);
}

bool bitset_isSet(Bitset* bitset, uint32_t index)
{
    uint32_t arrayIndex = index >> bitset_IndexToBitShift;
    uint32_t bitIndex = index & bitset_IndexBitMask;
    return (bitset->Data[arrayIndex] & (1ull << bitIndex)) != 0u;
}

void bitset_free(Bitset* bitset, uint32_t freeIndex)
{
    uint32_t arrayIndex = freeIndex >> bitset_IndexToBitShift;
    uint32_t bitIndex = freeIndex & bitset_IndexBitMask;
    bitset->Data[arrayIndex] = bitset->Data[arrayIndex] & (~(1ull << bitIndex));
}

#define vox_BrickWidth 8u
#define vox_BrickWidthBitShift 3u
#define vox_BrickWidthBitMask 7u
#define vox_PageWidth 64u
static uint32_t const vox_PageSize = vox_PageWidth * vox_PageWidth * vox_PageWidth;

typedef struct vox_Page
{
    Bitset AllocatedBricks;
    uint32_t* BrickIndices;
} vox_Page;

typedef struct vox_Brick
{
    uint64_t Voxels;
} vox_Brick;

static vox_Brick* BrickPool;
static Bitset BrickPoolAllocator;
static vox_Page TempPage;

static void vox_init()
{
    BrickPool = (vox_Brick*)malloc(sizeof(vox_Brick) * vox_PageSize);
    BrickPoolAllocator = (Bitset)
    {
        .Data = (uint64_t*)malloc(sizeof(uint64_t) * (vox_PageSize >> bitset_IndexToBitShift)),
        .Size = vox_PageSize >> bitset_IndexToBitShift
    };
    bitset_init(&BrickPoolAllocator);

    TempPage = (vox_Page)
    {
        .AllocatedBricks = (Bitset)
        {
            .Data = (uint64_t*)malloc(sizeof(uint64_t) * (vox_PageSize >> bitset_IndexToBitShift)),
            .Size = vox_PageSize >> bitset_IndexToBitShift
        },
        .BrickIndices = (uint32_t*)malloc(sizeof(uint32_t) * vox_PageSize)
    };
    bitset_init(&TempPage.AllocatedBricks);
}

static void vox_kill()
{
    free(BrickPool);
    free(BrickPoolAllocator.Data);
    free(TempPage.AllocatedBricks.Data);
    free(TempPage.BrickIndices);
}

static void vox_sinWaveHeightField(vox_Page* page, float voxelWorldWidth, float sinFrequency, float sinAmplitude, float heightBase)
{
    uint32_t pageWidthInVoxels = vox_BrickWidth * vox_PageWidth;
    for (uint32_t x = 0; x < pageWidthInVoxels; x++)
    {
        for (uint32_t y = 0; y < pageWidthInVoxels; y++)
        {
            for (uint32_t z = 0; z < pageWidthInVoxels; z++)
            {
                cv3 voxelCenter =
                {
                    ((float)x + 0.5f) * voxelWorldWidth,
                    ((float)y + 0.5f) * voxelWorldWidth,
                    ((float)z + 0.5f) * voxelWorldWidth,
                };

                float waveHeight = sinf(voxelCenter.x * sinFrequency) * sinAmplitude + heightBase;

                if (voxelCenter.y <= waveHeight)
                {
                    cu3 brickId = { x >> vox_BrickWidthBitShift, y >> vox_BrickWidthBitShift, z >> vox_BrickWidthBitShift };
                    uint32_t brickIndex = zCurve3D_256(brickId);
                    if (!bitset_isSet(&page->AllocatedBricks, brickIndex))
                    {
                        page->BrickIndices[brickIndex] = bitset_alloc(&BrickPoolAllocator);
                        bitset_set(&page->AllocatedBricks, brickIndex);
                    }

                    vox_Brick* brick = &BrickPool[page->BrickIndices[brickIndex]];

                    cu3 localVoxelId = { x & vox_BrickWidthBitMask, y & vox_BrickWidthBitMask, z & vox_BrickWidthBitMask };
                    uint32_t localVoxelIndex = zCurve3D_16(localVoxelId);
                    brick->Voxels = brick->Voxels | (1ull << localVoxelIndex);
                }
            }
        }
    }
}

// Global buffer memory pool end

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

    vox_init();
    vox_sinWaveHeightField(&TempPage, 0.125f, 0.75f, 1.5f, 4.0f);
}

static void kill(void)
{
    vox_kill();

    vkDeviceWaitIdle(Core.LogicalDevice);

    ibr_killDefaultResources(&Core, &DefaultResources);
    ibr_killRenderGraphs(&Core, Graphs, ib_arrayCount(Graphs));

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

Transform const IdentityTransform =
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
                                                                                sizeof(sg_ClearColorConstants), ibr_TransientBufferFlag_UniformBufferBit | ibr_TransientBufferFlag_Device, "ClearColor Constants");

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
                                                                       .ShaderInputs = ib_staticArrayRange(sg_ClearColorInputs),
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
                                                                                sizeof(constants), ibr_TransientBufferFlag_UniformBufferBit | ibr_TransientBufferFlag_Device, "TraceVoxels Constants");

            ibr_Resource tracePageBitmask = ibr_allocTransientBufferResource(graph,
                                                                                commands,
                                                                                TempPage.AllocatedBricks.Data,
                                                                                sizeof(uint64_t) * TempPage.AllocatedBricks.Size, ibr_TransientBufferFlag_StorageBufferBit | ibr_TransientBufferFlag_Device, "PageBitmask");



            ibr_beginComputePass(graph, commands, (ibr_BeginComputePassDesc)
                                 {
                                     .ResourceStates =
                                     {
                                         ibr_textureState(&renderOutput, ibr_TextureState_Write, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT),
                                         ibr_bufferState(&traceVoxelsConstants, ibr_BufferState_Read, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT),
                                         ibr_bufferState(&tracePageBitmask, ibr_BufferState_Read, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT),
                                     }
                                 });

            ib_ShaderInput shaderData = ibr_resourcesToShaderInput(graph, (ibr_ResourceToShaderInputDesc)
                                                                   {
                                                                       .ShaderInputs = ib_staticArrayRange(sg_TraceVoxelsInputs),
                                                                       .Layout = &TraceVoxelsShader.InlineShaderInputLayouts[0],
                                                                       .Resources = ib_staticArrayRange((ibr_Resource*[])
                                                                                                        {
                                                                                                            [TraceVoxels_Output] = &renderOutput,
                                                                                                            [TraceVoxels_Constants] = &traceVoxelsConstants,
                                                                                                            [TraceVoxels_VoxelPageBitmasks] = &tracePageBitmask,
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
    .Position = { 32.0f, 16.0f, 16.0f },
    .Rotation = { 0.0f, 0.0f, 0.0f, 1.0f },
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
