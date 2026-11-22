#include <LPP_API_x64_CPP.h>

#define SOKOL_IMPL
#define SOKOL_NOAPI
#include <sokol/sokol_app.h>

#include <iceberg/ib_core.h>
#include <iceberg/ib_rendergraph.h>

#include <cranberries/cranberry_math.h>

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

static void init()
{
    initLivePP();
    initStackAllocator();

    ib_initCore((ib_CoreDesc)
                {
                    .Win32MainWindowHandle = sapp_win32_get_hwnd(),
                    .Win32MainInstanceHandle = GetModuleHandle(NULL)
                },
                &Core);
    initGlobalBufferMemory(&Core);


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

static void kill()
{
    vkDeviceWaitIdle(Core.LogicalDevice);

    ibr_killRenderGraphs(&Core, Graphs, ib_arrayCount(Graphs));

    killGlobalBufferMemory(&Core);

    ib_freeSurface(&Core, &Surface);
    ib_killCore(&Core);

    killStackAllocator();
    killLivePP();
}

static void update()
{
    ibr_RenderGraph* graph = beginRenderGraph(&Surface);
    if (graph != NULL)
    {
        VkCommandBuffer commands = ibr_allocTransientCommandBuffer(graph, ib_Queue_Graphics);
        ib_beginCommandBuffer(&Core, commands);
        ibr_beginUpload(graph, commands);

        ibr_Resource swapchainResource = ibr_allocPassResource(graph, commands,
                                                               ibr_textureResourceDesc(graph->SwapchainTexture, VK_IMAGE_LAYOUT_UNDEFINED));

        ibr_beginGraphicsPass(graph, commands, (ibr_BeginGraphicsPassDesc)
                              {
                                  .RenderTargets = (ibr_RenderTargetState)
                                  {
                                      .Resource = &swapchainResource,
                                      .LoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                      .StoreOp = VK_ATTACHMENT_STORE_OP_STORE,
                                      .ClearValue = { .color = { 1.0f, 0.0f, 1.0f, 1.0f } }
                                  }
                              });

        ibr_endGraphicsPass(graph, commands);

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

sapp_desc sokol_main(int argc, char* argv[]) {
    return (sapp_desc) {
        .init_cb = &init,
        .frame_cb = &update,
        .cleanup_cb = &kill,
        .win32.console_attach = true,
        .window_title = "Voxel Game"
    };
}
