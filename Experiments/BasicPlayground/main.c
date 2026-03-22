#define SOKOL_IMPL
#define SOKOL_NOAPI
#include <sokol/sokol_app.h>

#include <cranberries/cranberry_math.h>

#include <iceberg/ib_core.h>
#include <iceberg/ib_rendergraph.h>

#include <stdint.h>

#define alignof __alignof

static ib_Core Core;
static ibr_RenderGraphPool GraphPool;
static ib_Surface Surface;

typedef struct
{
    uint16_t* Indices;
    cv3* Positions;
    cv2* Normals;
    uint32_t IndexCount;
    uint32_t PositionCount;
    uint32_t NormalCount;
} MeshDesc;

static void* transientAlloc(iba_StackAllocator* allocator, size_t size, size_t alignment)
{
    iba_StackAllocation allocation = iba_stackAlloc(allocator, (iba_StackAllocationRequest){ size, alignment });
    return iba_cpuStackAllocToMemory(allocation);
}

static size_t const GlobalBufferMemorySize = 1024u * 1024u * 32u; // 32 MB of global buffer memory.
static ib_Buffer GlobalBufferMemory;
static iba_TlsfAllocator GlobalBufferMemoryAllocator;

static MeshDesc createSphereMesh(uint32_t latitudeSegments, uint32_t longitudeSegments, float radius, iba_StackAllocator* allocator)
{
    MeshDesc mesh = {0};
    uint32_t segmentQuadCount = latitudeSegments * longitudeSegments;
    uint32_t segmentIndexCount = segmentQuadCount * 6;
    uint32_t segmentVertexCount = segmentQuadCount * 4;

    mesh.Indices = (uint16_t*)transientAlloc(allocator, segmentIndexCount * sizeof(uint16_t), alignof(uint16_t));
    mesh.Positions = (cv3*)transientAlloc(allocator, segmentVertexCount * sizeof(cv3), alignof(cv3));
    mesh.Normals = (cv2*)transientAlloc(allocator, segmentVertexCount * sizeof(cv2), alignof(cv2));
    mesh.IndexCount = segmentIndexCount;
    mesh.PositionCount = segmentVertexCount;
    mesh.NormalCount = segmentVertexCount;

    uint32_t indexWriter = 0;
    uint32_t vertexWriter = 0;

    float deltaTheta = cran_pi * cf_rcp((float)longitudeSegments);
    float deltaPhi = cran_tao * cf_rcp((float)latitudeSegments);
    for (float theta = 0.0f; theta < cran_pi; theta += deltaTheta)
    {
        for (float phi = 0; phi < cran_tao; phi += deltaPhi)
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
                mesh.Positions[vertexWriter + i] = cv3_mulf(pos[i], radius);
                mesh.Normals[vertexWriter + i] = cv3_to_octahedral(pos[i]);
            }

            vertexWriter += 4;
            indexWriter += 6;
        }
    }
    ib_assert(vertexWriter == segmentVertexCount);
    ib_assert(indexWriter == segmentIndexCount);

    return mesh;
}

typedef struct
{
    iba_TlsfAllocation Alloc;
    uint32_t IndexOffset;
    uint32_t PositionOffset;
    uint32_t NormalOffset;
} Mesh;

static Mesh allocMesh(MeshDesc desc)
{
    Mesh mesh;

    uint32_t indexSize = sizeof(uint16_t) * desc.IndexCount;
    uint32_t positionSize = sizeof(cv3) * desc.PositionCount;
    uint32_t normalSize = sizeof(cv2) * desc.NormalCount;
    uint32_t allocationSize = indexSize + positionSize + normalSize;
    mesh.Alloc = iba_tlsfAlloc(&GlobalBufferMemoryAllocator, allocationSize, alignof(float));
    mesh.IndexOffset = (uint32_t)mesh.Alloc.Offset;
    memcpy(GlobalBufferMemory.Allocation.CPUMemory + mesh.IndexOffset, desc.Indices, indexSize);
    mesh.PositionOffset = (uint32_t)(mesh.Alloc.Offset + indexSize);
    memcpy(GlobalBufferMemory.Allocation.CPUMemory + mesh.PositionOffset, desc.Positions, positionSize);
    mesh.NormalOffset = (uint32_t)(mesh.Alloc.Offset + indexSize + positionSize);
    memcpy(GlobalBufferMemory.Allocation.CPUMemory + mesh.NormalOffset, desc.Normals, normalSize);

    return mesh;
}

static void freeMesh(Mesh* mesh)
{
    iba_tlsfFree(&GlobalBufferMemoryAllocator, mesh->Alloc.Block);
}

Mesh SphereMesh;
static void init(void)
{
    ib_initCore((ib_CoreDesc)
                {
                    .Win32MainWindowHandle = sapp_win32_get_hwnd(),
                    .Win32MainInstanceHandle = GetModuleHandle(NULL)
                },
                &Core);

    GraphPool = ibr_allocRenderGraphPool(&Core);
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
    iba_StackAllocator initStackAllocator = { 0 };
    iba_initCpuStackAllocator((iba_CpuStackAllocatorDesc)
                           {
                               .PageSize = transientStackPageSize
                           },
                           &initStackAllocator);

    MeshDesc sphereMeshDesc = createSphereMesh(32, 32, 1.0f, &initStackAllocator);
    SphereMesh = allocMesh(sphereMeshDesc);
    iba_killStackAllocator(&initStackAllocator);
}

static void kill(void)
{
    vkDeviceWaitIdle(Core.LogicalDevice);
    freeMesh(&SphereMesh);
    
    iba_killTlsfAllocator(&GlobalBufferMemoryAllocator);
    ib_freeBuffer(&Core, &GlobalBufferMemory);
    ib_freeSurface(&Core, &Surface);
    ibr_freeRenderGraphPool(&Core, &GraphPool);
    ib_killCore(&Core);
}

static void update(void)
{
    static uint32_t ActiveFrame = 0;
    ibr_RenderGraph* graph = ibr_beginFrame(&GraphPool, (ibr_BeginFrameDesc)
                   {
                       .FrameIndex = ActiveFrame,
                       .Surface = &Surface
                   });
    if (graph != NULL)
    {
        VkCommandBuffer commands = ibr_allocTransientCommandBuffer(graph, ib_Queue_Graphics);
        ib_beginCommandBuffer(&Core, commands);

        ibr_Resource swapchainResource; 
        ibr_allocPassResources(graph, (ibr_AllocPassResourcesDesc)
                               {
                                   .ResourceBindings = (ibr_AllocResourceBinding)
                                   {
                                       .OutResource = &swapchainResource,
                                       .Desc =
                                       {
                                           .Type = ibr_ResourceType_Texture,
                                           .Texture = graph->SwapchainTexture,
                                       }
                                   }
                               });

        ibr_beginGraphicsPass(graph, commands, (ibr_BeginGraphicsPassDesc)
                              {
                                  .RenderTargets = (ibr_RenderTargetState)
                                  {
                                      .Resource = &swapchainResource,
                                      .LoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                      .StoreOp = VK_ATTACHMENT_STORE_OP_STORE,
                                      .ClearValue = { .color = { 0.0f, 0.0f, 0.0f, 1.0f } }
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
                                     .WaitSemaphores = graph->SwapchainAcquireSemaphore,
                                     .SignalSemaphores = graph->FrameSemaphore,
                                     .SubmitFence = graph->FrameFence
                                 });

        ibr_present((ibr_PresentDesc) { &Surface, graph });
        ibr_endFrame(&GraphPool, graph);
    }

    ActiveFrame = (ActiveFrame + 1) % ib_FramebufferCount;
}

void events(sapp_event const* event)
{
    if (event->type == SAPP_EVENTTYPE_RESIZED)
    {
        ib_rebuildSurface(&Core, &Surface);
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
