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

static void saveConfig()
{
    FILE* config = fopen("config.txt", "w");
    fprintf(config, "%s\n", InputFilePath);
    fprintf(config, "%s\n", BackendScriptPath);
    fprintf(config, "%s\n", CompilationParams);
    fclose(config);
}

static void getStringNoNewline(char* array, int arrayCount, FILE* file)
{
    fgets(array, arrayCount, file);
    size_t strLen = strlen(array);
    // strip newline retained by fgets.
    if (strLen > 0 && array[strLen - 1] == '\n')
    {
        array[strLen - 1] = 0;
    }
}

static void loadConfig()
{
    FILE* config = fopen("config.txt", "r");
    if (config != NULL)
    {
        getStringNoNewline(InputFilePath, ib_arrayCount(InputFilePath), config);
        getStringNoNewline(BackendScriptPath, ib_arrayCount(BackendScriptPath), config);
        getStringNoNewline(CompilationParams, ib_arrayCount(CompilationParams), config);
        fclose(config);
    }
}

static bool readWholeFile(char const* path, char** output, size_t* outputSize)
{
    FILE *statsFile = fopen(path, "rb");
    if (statsFile != NULL)
    {
        fseek(statsFile, 0, SEEK_END);
        long fileSize = ftell(statsFile);
        fseek(statsFile, 0, SEEK_SET); /* same as rewind(f); */

        *output = realloc(*output, fileSize + 1);
        *outputSize = fileSize + 1;

        fread(*output, fileSize, 1, statsFile);
        (*output)[fileSize] = 0;

        fclose(statsFile);
        return true;
    }

    return false;
}

static ib_Core Core;
static ibr_RenderGraphPool GraphPool;
static ib_Surface Surface;

static char* DisassemblyStats = NULL;
static size_t DisassemblyStatsSize = 0;

static char* Disassembly = NULL;
static size_t DisassemblySize = 0;

static char const* outputFileName = "temp/compilation_output.txt";
static char const* statsFileName = "temp/stats.txt";

static char* SourceFileData = NULL;
static size_t SourceFileSize = 0;

static bool RecompileOnFileChange = false;
static bool FileModified = false;

static void init(void)
{
    if (!readWholeFile(statsFileName, &DisassemblyStats, &DisassemblyStatsSize))
    {
        DisassemblyStats = calloc(1, 1);
        DisassemblyStatsSize = 1;
    }

    if (!readWholeFile(outputFileName, &Disassembly, &DisassemblySize))
    {
        Disassembly = calloc(1, 1);
        DisassemblySize = 1;
    }

    SourceFileData = calloc(1, 1);
    SourceFileSize = 1;

    CreateDirectoryA("temp", NULL);
    LogOutputHandle = fopen("temp/log.txt", "w");
    loadConfig();

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
                                       .SRGB = false
                                   });

    imgui_init(&Core, Surface.Format.format);
}

static void kill(void)
{
    free(DisassemblyStats);
    free(Disassembly);
    free(SourceFileData);

    saveConfig();
    fclose(LogOutputHandle);

    vkDeviceWaitIdle(Core.LogicalDevice);

    imgui_kill();
    ib_freeSurface(&Core, &Surface);
    ibr_freeRenderGraphPool(&Core, &GraphPool);
    ib_killCore(&Core);
}

static bool ShouldCompile = false;

static int sourceFileResize(ImGuiInputTextCallbackData* data)
{
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
    {
        SourceFileData = realloc(SourceFileData, data->BufSize);
        SourceFileSize = data->BufSize;
        data->Buf = SourceFileData;
    }
    return 0;
}

static void update(void)
{
    imgui_beginFrame();

    ImGuiIO* io = igGetIO_Nil();
    igSetNextWindowSize(io->DisplaySize, ImGuiCond_None);
    igSetNextWindowPos((ImVec2_c){0}, ImGuiCond_None, (ImVec2_c){0});
    if (igBegin("Main Window", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize))
    {
        static FILETIME lastFileTime = { 0 };

        bool fileTimeChanged = false;

        WIN32_FILE_ATTRIBUTE_DATA fileAttributes;
        if (GetFileAttributesExA(InputFilePath, GetFileExInfoStandard, &fileAttributes) == TRUE)
        {
            fileTimeChanged = memcmp(&fileAttributes.ftLastWriteTime, &lastFileTime, sizeof(FILETIME)) != 0;
            lastFileTime = fileAttributes.ftLastWriteTime;
        }

        ShouldCompile |= (RecompileOnFileChange && fileTimeChanged);

        igBeginTable("Table0", 2, ImGuiTableFlags_Resizable, (ImVec2_c) { 0 }, 0.0f);
        {
            igTableNextColumn();

            {
                igBeginTable("ParamTable", 2, ImGuiTableFlags_None, (ImVec2_c) { 0.0f, 0.0f }, 0.0f);
                {
                    igTableSetupColumn("Inputs", ImGuiTableColumnFlags_WidthStretch, 0.0f, 0);
                    igTableSetupColumn("Labels", ImGuiTableColumnFlags_WidthFixed, 0.0f, 0);

                    igTableNextColumn();
                    igInputTextEx("##Source File", NULL, InputFilePath, ib_arrayCount(InputFilePath), (ImVec2_c) { -1.0f, 0.0f }, ImGuiInputTextFlags_None, NULL, NULL);
                    igTableNextColumn();
                    igText("Source File");

                    igTableNextColumn();
                    igInputTextEx("##Backend Script", NULL, BackendScriptPath, ib_arrayCount(BackendScriptPath), (ImVec2_c) { -1.0f, 0.0f }, ImGuiInputTextFlags_None, NULL, NULL);
                    igTableNextColumn();
                    igText("Backend Script");

                    igTableNextColumn();
                    igInputTextEx("##Params", NULL, CompilationParams, ib_arrayCount(CompilationParams), (ImVec2_c) { -1.0f, 0.0f }, ImGuiInputTextFlags_None, NULL, NULL);
                    igTableNextColumn();
                    igText("Params");
                }
                igEndTable();

                ShouldCompile |= igButton("Compile", (ImVec2_c) { 0 });

                if (ShouldCompile)
                {
                    char systemCommand[1024];
                    snprintf(systemCommand, ib_arrayCount(systemCommand), "py \"%s\" -i \"%s\" -s \"%s\" -o=\"%s\" %s",
                             BackendScriptPath, InputFilePath, statsFileName, outputFileName, CompilationParams);

                    win32RunProcess(systemCommand);

                    readWholeFile(statsFileName, &DisassemblyStats, &DisassemblyStatsSize);
                    readWholeFile(outputFileName, &Disassembly, &DisassemblySize);

                    ShouldCompile = false;
                }

                igSameLine(0.0f, -1.0f);
                igCheckbox("Recompile On File Change", &RecompileOnFileChange);
            }

            igTableNextColumn();

            {
                igText("Stats");
                igInputTextMultiline("##Stats", DisassemblyStats, DisassemblyStatsSize, (ImVec2_c) { -1.0f, 0.0f }, ImGuiInputTextFlags_ReadOnly, (ImGuiInputTextCallback) { 0 }, NULL);
            }
        }
        igEndTable();

        igBeginTable("Table1", 2, ImGuiTableFlags_BordersOuter | ImGuiTableFlags_Resizable | ImGuiTableFlags_Hideable, (ImVec2_c) { 0.0f, -1.0f }, 0.0f);
        {
            igTableSetupColumn(FileModified ? "Source*" : "Source", ImGuiTableColumnFlags_WidthStretch, 0.0f, 0);
            igTableSetupColumn("Disassembly", ImGuiTableColumnFlags_WidthStretch, 0.0f, 0);
            igTableHeadersRow();

            igTableNextColumn();

            {
                static char viewedPath[256];

                if (memcmp(viewedPath, InputFilePath, ib_arrayCount(InputFilePath)) != 0 || fileTimeChanged)
                {
                    memcpy(viewedPath, InputFilePath, ib_arrayCount(InputFilePath));
                    readWholeFile(viewedPath, &SourceFileData, &SourceFileSize);
                }

                FileModified |= igInputTextMultiline("##SourceFile", SourceFileData, SourceFileSize, (ImVec2_c) { -1.0f, -1.0f }, ImGuiInputTextFlags_CallbackResize, &sourceFileResize, NULL);
            }

            igTableNextColumn();

            {
                igInputTextMultiline("##DisassemblyFile", Disassembly, DisassemblySize, (ImVec2_c) { -1.0f, -1.0f }, ImGuiInputTextFlags_ReadOnly, (ImGuiInputTextCallback) { 0 }, NULL);
            }
        }
        igEndTable();
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

static void events(sapp_event const* event)
{
    if (event->type == SAPP_EVENTTYPE_RESIZED)
    {
        ib_rebuildSurface(&Core, &Surface);
    }
    else if (event->type == SAPP_EVENTTYPE_KEY_DOWN)
    {
        if (event->key_code == SAPP_KEYCODE_C && event->modifiers == (SAPP_MODIFIER_SHIFT | SAPP_MODIFIER_CTRL))
        {
            ShouldCompile = true;
        }
        else if (event->key_code == SAPP_KEYCODE_S && event->modifiers == SAPP_MODIFIER_CTRL)
        {
            FILE* sourceFile = fopen(InputFilePath, "wb");
            if (sourceFile != NULL)
            {
                fprintf(sourceFile, "%s", SourceFileData);
                fclose(sourceFile);

                if (RecompileOnFileChange)
                {
                    ShouldCompile = true;
                }

                FileModified = false;
            }
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
        .window_title = "Disassembly Playground"
    };
}
