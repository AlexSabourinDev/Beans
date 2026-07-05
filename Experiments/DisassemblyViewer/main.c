#define SOKOL_IMPL
#define SOKOL_NOAPI
#include <sokol/sokol_app.h>

#include <iceberg/ib_core.h>
#include <iceberg/ib_rendergraph.h>

#include "../Shared/ib_imgui.h"

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

typedef struct
{
    bool Open;
    FILETIME LastFileTime;
    char* SourceFileData;
    size_t SourceFileSize;
    char* Disassembly;
    size_t DisassemblySize;
    char InputFilePath[256];
    char BackendScriptPath[256];
    char CompilationParams[256];
    char* DisassemblyStats;
    size_t DisassemblyStatsSize;
    bool ShouldCompile;
} CompilationContext;

#define MaxCompilationContexts 8
static CompilationContext Contexts[MaxCompilationContexts];
static uint32_t ActiveContexts = 0;
static uint32_t CurrentContext = 0;

static void saveConfig()
{
    FILE* config = fopen("config.txt", "w");
    fprintf(config, "%u\n", ActiveContexts);
    for (uint32_t i = 0; i < ActiveContexts; i++)
    {
        fprintf(config, "%s\n", Contexts[i].InputFilePath);
        fprintf(config, "%s\n", Contexts[i].BackendScriptPath);
        fprintf(config, "%s\n", Contexts[i].CompilationParams);
    }
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
        fscanf(config, "%u\n", &ActiveContexts);
        for (uint32_t i = 0; i < ActiveContexts; i++)
        {
            getStringNoNewline(Contexts[i].InputFilePath, ib_arrayCount(Contexts[i].InputFilePath), config);
            getStringNoNewline(Contexts[i].BackendScriptPath, ib_arrayCount(Contexts[i].BackendScriptPath), config);
            getStringNoNewline(Contexts[i].CompilationParams, ib_arrayCount(Contexts[i].CompilationParams), config);
        }
        fclose(config);
    }
}

static void readWholeFileFromHandle(FILE* file, char** output, size_t* outputSize)
{
    fseek(file, 0, SEEK_END);
    long fileSize = ftell(file);
    fseek(file, 0, SEEK_SET); /* same as rewind(f); */

    *output = realloc(*output, fileSize + 1);
    *outputSize = fileSize + 1;

    fread(*output, fileSize, 1, file);
    (*output)[fileSize] = 0;
}

static bool readWholeFile(char const* path, char** output, size_t* outputSize)
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

static ib_Core Core;
static ibr_RenderGraph Graphs[ib_FramebufferCount];
static ib_Surface Surface;

static char const* OutputFileName = "temp/compilation_output.txt";
static char const* StatsFileName = "temp/stats.txt";
static char const* LogFileName = "temp/log.txt";

static bool RecompileOnFileChange = false;
static bool FileModified = false;

static void init(void)
{
    for (uint32_t i = 0; i < MaxCompilationContexts; i++)
    {
        Contexts[i].DisassemblyStats = calloc(1, 1);
        Contexts[i].DisassemblyStatsSize = 1;
        Contexts[i].Disassembly = calloc(1, 1);
        Contexts[i].DisassemblySize = 1;
        Contexts[i].SourceFileData = calloc(1, 1);
        Contexts[i].SourceFileSize = 1;
    }

    CreateDirectoryA("temp", NULL);
    LogOutputHandle = fopen(LogFileName, "w+");
    loadConfig();

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
                                       .SRGB = false
                                   });

    imgui_init(&Core, Surface.Format.format);
}

static void kill(void)
{
    for (uint32_t i = 0; i < MaxCompilationContexts; i++)
    {
        free(Contexts[i].DisassemblyStats);
        free(Contexts[i].Disassembly);
        free(Contexts[i].SourceFileData);
    }

    saveConfig();
    fclose(LogOutputHandle);

    vkDeviceWaitIdle(Core.LogicalDevice);

    imgui_kill();
    ib_freeSurface(&Core, &Surface);
    ibr_killRenderGraphs(&Core, Graphs, ib_arrayCount(Graphs));
    ib_killCore(&Core);
}

static int sourceFileResize(ImGuiInputTextCallbackData* data)
{
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
    {
        Contexts[CurrentContext].SourceFileData = realloc(Contexts[CurrentContext].SourceFileData, data->BufSize);
        Contexts[CurrentContext].SourceFileSize = data->BufSize;
        data->Buf = Contexts[CurrentContext].SourceFileData;
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
        if (igBeginTabBar("CompilationTabs", ImGuiTabBarFlags_None))
        {
            for (uint32_t i = 0; i < ActiveContexts; i++)
            {
                char contextName[] = "Context 0";
                // Bit of a hack, 9th character is 0, make it match our active context count.
                contextName[8] += i;
                if (igBeginTabItem(contextName, NULL, ImGuiTabItemFlags_None))
                {
                    CurrentContext = i;

                    bool fileTimeChanged = false;

                    CompilationContext* ctx = &Contexts[i];

                    WIN32_FILE_ATTRIBUTE_DATA fileAttributes;
                    if (GetFileAttributesExA(ctx->InputFilePath, GetFileExInfoStandard, &fileAttributes) == TRUE)
                    {
                        fileTimeChanged = memcmp(&fileAttributes.ftLastWriteTime, &ctx->LastFileTime, sizeof(FILETIME)) != 0;
                        ctx->LastFileTime = fileAttributes.ftLastWriteTime;
                    }

                    ctx->ShouldCompile |= (RecompileOnFileChange && fileTimeChanged);

                    igBeginTable("Table0", 2, ImGuiTableFlags_Resizable, (ImVec2_c) { 0 }, 0.0f);
                    {
                        igTableNextColumn();

                        {
                            igBeginTable("ParamTable", 2, ImGuiTableFlags_None, (ImVec2_c) { 0.0f, 0.0f }, 0.0f);
                            {
                                igTableSetupColumn("Inputs", ImGuiTableColumnFlags_WidthStretch, 0.0f, 0);
                                igTableSetupColumn("Labels", ImGuiTableColumnFlags_WidthFixed, 0.0f, 0);

                                igTableNextColumn();
                                igInputTextEx("##Source File", NULL, ctx->InputFilePath, ib_arrayCount(ctx->InputFilePath), (ImVec2_c) { -1.0f, 0.0f }, ImGuiInputTextFlags_None, NULL, NULL);
                                igTableNextColumn();
                                igText("Source File");

                                igTableNextColumn();
                                igInputTextEx("##Backend Script", NULL, ctx->BackendScriptPath, ib_arrayCount(ctx->BackendScriptPath), (ImVec2_c) { -1.0f, 0.0f }, ImGuiInputTextFlags_None, NULL, NULL);
                                igTableNextColumn();
                                igText("Backend Script");

                                igTableNextColumn();
                                igInputTextEx("##Params", NULL, ctx->CompilationParams, ib_arrayCount(ctx->CompilationParams), (ImVec2_c) { -1.0f, 0.0f }, ImGuiInputTextFlags_None, NULL, NULL);
                                igTableNextColumn();
                                igText("Params");
                            }
                            igEndTable();

                            ctx->ShouldCompile |= igButton("Compile", (ImVec2_c) { 0 });

                            if (ctx->ShouldCompile)
                            {
                                char systemCommand[1024];
                                snprintf(systemCommand, ib_arrayCount(systemCommand), "py \"%s\" -i \"%s\" -s \"%s\" -o=\"%s\" %s",
                                         ctx->BackendScriptPath, ctx->InputFilePath, StatsFileName, OutputFileName, ctx->CompilationParams);

                                win32RunProcess(systemCommand);

                                readWholeFile(StatsFileName, &ctx->DisassemblyStats, &ctx->DisassemblyStatsSize);
                                readWholeFile(OutputFileName, &ctx->Disassembly, &ctx->DisassemblySize);

                                char* logFile = NULL;
                                size_t logFileSize = 0;
                                readWholeFileFromHandle(LogOutputHandle, &logFile, &logFileSize);
                                printf("%s\n", logFile);
                                free(logFile);

                                ctx->ShouldCompile = false;
                            }

                            igSameLine(0.0f, -1.0f);
                            igCheckbox("Recompile On File Change", &RecompileOnFileChange);
                        }

                        igTableNextColumn();

                        {
                            igText("Stats");
                            igInputTextMultiline("##Stats", ctx->DisassemblyStats, ctx->DisassemblyStatsSize, (ImVec2_c) { -1.0f, 0.0f }, ImGuiInputTextFlags_ReadOnly, (ImGuiInputTextCallback) { 0 }, NULL);
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

                            if (memcmp(viewedPath, ctx->InputFilePath, ib_arrayCount(ctx->InputFilePath)) != 0 || fileTimeChanged)
                            {
                                memcpy(viewedPath, ctx->InputFilePath, ib_arrayCount(ctx->InputFilePath));
                                readWholeFile(viewedPath, &ctx->SourceFileData, &ctx->SourceFileSize);
                            }

                            FileModified |= igInputTextMultiline("##SourceFile", ctx->SourceFileData, ctx->SourceFileSize, (ImVec2_c) { -1.0f, -1.0f }, ImGuiInputTextFlags_CallbackResize, &sourceFileResize, NULL);
                        }

                        igTableNextColumn();

                        {
                            igInputTextMultiline("##DisassemblyFile", ctx->Disassembly, ctx->DisassemblySize, (ImVec2_c) { -1.0f, -1.0f }, ImGuiInputTextFlags_ReadOnly, (ImGuiInputTextCallback) { 0 }, NULL);
                        }
                    }
                    igEndTable();
                    igEndTabItem();
                }
            }

            if (igTabItemButton("+", ImGuiTabItemFlags_Button))
            {
                ActiveContexts = ActiveContexts < MaxCompilationContexts ? (ActiveContexts+1) : MaxCompilationContexts;
            }
            igEndTabBar();
        }
    }
    igEnd();

    static uint32_t ActiveFrame = 0;
    ibr_RenderGraph* graph = &Graphs[ActiveFrame]; 
    bool frameBegun = ibr_beginFrame(graph, (ibr_BeginFrameDesc)
                                            {
                                                .FrameIndex = ActiveFrame,
                                                .Surface = &Surface
                                            });
    if (frameBegun)
    {
        VkCommandBuffer commands = ibr_allocTransientCommandBuffer(graph, ib_Queue_Graphics);
        ib_beginCommandBuffer(&Core, commands);
        ibr_beginUpload(graph, commands);

        ibr_Resource swapchainResource;
        ibr_allocPassResources(graph, commands, (ibr_AllocPassResourcesDesc)
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
        ibr_endUpload(graph);
        ibr_endFrame(graph);
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
            Contexts[CurrentContext].ShouldCompile = true;
        }
        else if (event->key_code == SAPP_KEYCODE_S && event->modifiers == SAPP_MODIFIER_CTRL)
        {
            FILE* sourceFile = fopen(Contexts[CurrentContext].InputFilePath, "wb");
            if (sourceFile != NULL)
            {
                fprintf(sourceFile, "%s", Contexts[CurrentContext].SourceFileData);
                fclose(sourceFile);

                if (RecompileOnFileChange)
                {
                    Contexts[CurrentContext].ShouldCompile = true;
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
