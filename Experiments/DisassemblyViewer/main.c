#define SOKOL_IMPL
#define SOKOL_NOAPI
#include <sokol/sokol_app.h>

#include <iceberg/ib_core.h>
#include <iceberg/ib_rendergraph.h>

#include "ib_imgui.h"

#include <stdlib.h>
#include <stdio.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <io.h>

static FILE* LogOutputHandle;

static void win32RunProcess(char* commandLine)
{
    fprintf(LogOutputHandle, "%s\n", commandLine);
    fflush(LogOutputHandle);

    HANDLE outputLog = (HANDLE)_get_osfhandle(_fileno(LogOutputHandle));

    PROCESS_INFORMATION processInformation = { 0 };
    STARTUPINFOA startupInfo = { 0 };
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.hStdOutput = outputLog;
    startupInfo.hStdError = outputLog;
    startupInfo.dwFlags |= STARTF_USESTDHANDLES;

    // The target process will be created in session of the
    // provider host process. If the provider was hosted in
    // wmiprvse.exe process, the target process will be launched
    // in session 0 and UI is invisible to the logged on user,
    // but the process can be found through task manager.
    BOOL creationResult = CreateProcessA(
        NULL,                   // Command line + module name
        commandLine,        // Command line
        NULL,                   // Process handle not inheritable
        NULL,                   // Thread handle not inheritable
        TRUE,                  // Set handle inheritance to FALSE
        NORMAL_PRIORITY_CLASS | CREATE_NO_WINDOW,  // creation flags
        NULL,                   // Use parent's environment block
        NULL,                   // Use parent's starting directory 
        &startupInfo,           // Pointer to STARTUPINFO structure
        &processInformation);   // Pointer to PROCESS_INFORMATION structure

    if (creationResult == TRUE)
    {
        WaitForSingleObject(processInformation.hProcess, INFINITE);
        CloseHandle(processInformation.hProcess);
    }
    else
    {
        fprintf(LogOutputHandle, "Failed to run compilation command.");
    }
}

static char InputFilePath[256];
static char BackendScriptPath[256];
static char CompilationParams[256];

static void SaveConfig()
{
    FILE* config = fopen("config.txt", "w");
    fprintf(config, "%s\n", InputFilePath);
    fprintf(config, "%s\n", BackendScriptPath);
    fprintf(config, "%s\n", CompilationParams);
    fclose(config);
}

void GetStringNoNewline(char* array, int arrayCount, FILE* file)
{
    fgets(array, arrayCount, file);
    size_t strLen = strlen(array);
    // strip newline retained by fgets.
    if (strLen > 0 && array[strLen - 1] == '\n')
    {
        array[strLen - 1] = 0;
    }
}

static void LoadConfig()
{
    FILE* config = fopen("config.txt", "r");
    if (config != NULL)
    {
        GetStringNoNewline(InputFilePath, ib_arrayCount(InputFilePath), config);
        GetStringNoNewline(BackendScriptPath, ib_arrayCount(BackendScriptPath), config);
        GetStringNoNewline(CompilationParams, ib_arrayCount(CompilationParams), config);
        fclose(config);
    }
}

static ib_Core Core;
static ibr_RenderGraphPool GraphPool;
static ib_Surface Surface;

static char* DisassemblyStats = NULL;
static size_t DisassemblyStatsSize = 0;

static void init(void)
{
    DisassemblyStats = calloc(1, 1);
    DisassemblyStatsSize = 1;

    LogOutputHandle = fopen("temp/log.txt", "w");
    LoadConfig();

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

    imgui_init(&Core, Surface.Format.format);
}

static void kill(void)
{
    free(DisassemblyStats);

    SaveConfig();
    fclose(LogOutputHandle);

    vkDeviceWaitIdle(Core.LogicalDevice);

    imgui_kill();
    ib_freeSurface(&Core, &Surface);
    ibr_freeRenderGraphPool(&Core, &GraphPool);
    ib_killCore(&Core);
}

static void update(void)
{
    imgui_beginFrame();

    ImGuiIO* io = igGetIO_Nil();
    igSetNextWindowSize(io->DisplaySize, ImGuiCond_None);
    igSetNextWindowPos((ImVec2_c){0}, ImGuiCond_None, (ImVec2_c){0});
    if (igBegin("Main Window", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize))
    {
        igInputText("Source File", InputFilePath, ib_arrayCount(InputFilePath), ImGuiInputTextFlags_None, NULL, NULL);
        igInputText("Backend Script", BackendScriptPath, ib_arrayCount(BackendScriptPath), ImGuiInputTextFlags_None, NULL, NULL);
        igInputText("Params", CompilationParams, ib_arrayCount(CompilationParams), ImGuiInputTextFlags_None, NULL, NULL);
        if (igButton("Compile", (ImVec2_c){0}))
        {
            char const* outputFileName = "temp/compilation_output.txt";

            char systemCommand[1024];
            snprintf(systemCommand, ib_arrayCount(systemCommand), "py \"%s\" -i \"%s\" -o \"%s\" %s",
                     BackendScriptPath, InputFilePath, outputFileName, CompilationParams);

            win32RunProcess(systemCommand);

            FILE *statsFile = fopen("temp/stats.txt", "rb");
            if (statsFile != NULL)
            {
                fseek(statsFile, 0, SEEK_END);
                long fileSize = ftell(statsFile);
                fseek(statsFile, 0, SEEK_SET); /* same as rewind(f); */

                DisassemblyStats = realloc(DisassemblyStats, fileSize + 1);
                DisassemblyStatsSize = fileSize + 1;

                fread(DisassemblyStats, fileSize, 1, statsFile);
                DisassemblyStats[fileSize] = 0;

                fclose(statsFile);
            }
        }

        igInputTextMultiline("##Stats", DisassemblyStats, DisassemblyStatsSize, (ImVec2_c) { 0 }, ImGuiInputTextFlags_ReadOnly, (ImGuiInputTextCallback) { 0 }, NULL);
    }
    igEnd();

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

        imgui_render(commands, graph->ScreenExtent, graph->SwapchainTexture);

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

    imgui_endFrame();
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
