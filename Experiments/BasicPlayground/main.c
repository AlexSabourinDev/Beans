#define SOKOL_IMPL
#define SOKOL_NOAPI
#include <sokol/sokol_app.h>

#include <iceberg/ib_core.h>
#include <iceberg/ib_rendergraph.h>

static ib_Core Core;
static ibr_RenderGraphPool GraphPool;
static ib_Surface Surface;

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
}

static void kill(void)
{
    vkDeviceWaitIdle(Core.LogicalDevice);
    ib_freeSurface(&Core, &Surface);
    ibr_freeRenderGraphPool(&Core, &GraphPool);
    ib_killCore(&Core);
}

static void update(void)
{
    static uint32_t activeFrame = 0;
    ibr_RenderGraph* graph = ibr_beginFrame(&GraphPool, (ibr_BeginFrameDesc)
                   {
                       .FrameIndex = activeFrame,
                       .Surface = &Surface
                   });
    if (graph != NULL)
    {
        VkCommandBuffer commands = ibr_allocTransientCommandBuffer(graph, ib_Queue_Graphics);
        ib_beginCommandBuffer(&Core, commands);

        ibr_Resource swapchainResource = ibr_allocPassResource(graph, (ibr_ResourceDesc)
                              {
                                  .Type = ibr_ResourceType_Texture,
                                  .Texture = graph->SwapchainTexture,
                              });

        ibr_beginGraphicsPass(graph, commands, (ibr_BeginGraphicsPassDesc)
                              {
                                  .RenderTargets = ib_singleValueRange((ibr_RenderTargetState)
                                                                       {
                                                                           .Resource = &swapchainResource,
                                                                           .LoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                                                           .StoreOp = VK_ATTACHMENT_STORE_OP_STORE,
                                                                           .ClearValue = { .color = {1.0f, 0.0f, 1.0f, 1.0f} }
                                                                       }),
                              });
        ibr_endGraphicsPass(graph, commands);
        ibr_barriers(graph, commands, (ibr_BarriersDesc)
                     {
                         .ResourceStates = ib_staticArrayRange((ibr_ResourceState[])
                                                               {
                                                                   ibr_textureToPresentState(&swapchainResource)
                                                               })
                     });

        ib_vkCheck(vkEndCommandBuffer(commands));
        VkSubmitInfo submitInfo =
        {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &commands,
            .pSignalSemaphores = &graph->FrameSemaphore,
            .signalSemaphoreCount = 1,
            .pWaitSemaphores = &Surface.Framebuffers[activeFrame].AcquireSemaphore,
            .waitSemaphoreCount = 1,
            .pWaitDstStageMask = (VkPipelineStageFlags[]) { VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT }
        };
        ib_vkCheck(vkQueueSubmit(Core.Queues[ib_Queue_Graphics].Queue, 1, &submitInfo, graph->FrameFence));

        ib_SurfaceState surfaceState = ib_presentSurface(&Core, (ib_PresentSurfaceDesc)
                      {
                          .Surface = &Surface,
                          .SwapchainTextureIndex = graph->SwapchainTextureIndex,
                          .WaitSemaphore = graph->FrameSemaphore
                      });
        ibr_endFrame(&GraphPool, graph);
    }

    activeFrame = (activeFrame + 1) % ib_FramebufferCount;
}

sapp_desc sokol_main(int argc, char* argv[]) {
    return (sapp_desc) {
        .init_cb = &init,
        .frame_cb = &update,
        .cleanup_cb = &kill,
        .win32.console_attach = true
    };
}
