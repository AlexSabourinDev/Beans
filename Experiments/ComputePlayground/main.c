#include <iceberg/ib_core.h>
#include <iceberg/ib_rendergraph.h>

#include <Windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <immintrin.h>

// Platform... stuff. I'd like to put this somewhere.
// Maybe something line cranberry_platform
static char CurrentWorkingDirectory[256];
static void readWholeFileFromHandle(FILE* file, void** output, size_t* outputSize)
{
    fseek(file, 0, SEEK_END);
    long fileSize = ftell(file);
    fseek(file, 0, SEEK_SET);

    *output = malloc(fileSize);
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

static void compileShader(char const* shader, char const *shaderOutput, char const* entryPoint, char const* shaderType)
{
    char shaderCompilation[1024];
    snprintf(shaderCompilation, ib_arrayCount(shaderCompilation),
        "py %s/../../Assets/compile_shaders.py -i %s/../../Assets/Shaders/%s -e %s -t %s -o %s/../../CompiledAssets/Shaders/%s \n",
        CurrentWorkingDirectory, CurrentWorkingDirectory, shader, entryPoint, shaderType, CurrentWorkingDirectory, shaderOutput);
    printf("Running: %s\n", shaderCompilation);
    system(shaderCompilation);
}

uint16_t f32tof16(float aV)
{
	__m128 V1 = _mm_set_ss(aV);
    __m128i V2 = _mm_cvtps_ph(V1, _MM_FROUND_TO_NEAREST_INT);
    return (uint16_t)(_mm_extract_epi16(V2, 0));
}

int main()
{
    GetCurrentDirectoryA(ib_arrayCount(CurrentWorkingDirectory), CurrentWorkingDirectory);
    compileShader("compute_playground.hlsl", "compute_playground.spv", "CS", "compute");

    ib_Core core;
    ibr_RenderGraph graphs[ib_FramebufferCount] = { 0 };

    ib_initCore((ib_CoreDesc){}, &core);
    ibr_initRenderGraphs(&core, graphs, ib_arrayCount(graphs));

    static ib_ShaderInputDesc const computeInputs[] =
    {
        { .Index = 0, .Shaders = VK_SHADER_STAGE_COMPUTE_BIT, .Type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER },
    };

    void* computeSpv;
    size_t computeSpvSize;
    readWholeFile("../../CompiledAssets/Shaders/compute_playground.spv", &computeSpv, &computeSpvSize);
    ib_ComputePipeline pipeline = ib_allocComputePipeline(&core, (ib_ComputePipelineDesc)
                            {
                                .ShaderDesc = (ib_ShaderDesc)
                                {
                                    .EntryPoint = "CS",
                                    .Code = computeSpv,
                                    .CodeSize = computeSpvSize,
                                    .Stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                },
                                .ShaderInputs =
                                {
                                    { ib_staticArrayRange(computeInputs) }
                                }
                            });
    free(computeSpv);

	uint32_t activeFrameIndex = 0;
	while(true)
	{
		ibr_RenderGraph* graph = &graphs[activeFrameIndex];
		ibr_beginFrame(graph, (ibr_BeginFrameDesc) { .FrameIndex = activeFrameIndex });

		VkCommandBuffer commands = ibr_allocTransientCommandBuffer(graph, ib_Queue_Graphics);
		ib_beginCommandBuffer(&core, commands);

		typedef struct
		{
			union
			{
				struct
				{
					uint16_t x, y;
				};
				uint32_t v;
			};
		} float16_t2;

		typedef struct
		{
			float Padding[8][4];
			float Padding0[3];
			float Padding1;
			float Padding2[4];
			float _0;
			uint32_t _1;
			uint32_t _2;
			float16_t2 _3;
			float16_t2 _4;
			float16_t2 _5;
			float16_t2 _6;
			float16_t2 _7;
			float Padding3[4];
		} Data;

		Data data = (Data)
		{
			0.432154f, 0.432154f, 0.432154f, 0.432154f,
			0.0f,
			._1 = rand(),
			._2 = rand(),
			._3 = { .x = f32tof16((float)rand() / (float)RAND_MAX), .y = f32tof16((float)rand() / (float)RAND_MAX) },
			._4 = { .x = 0x3D62, .y = 0xBB84 }
		};

		ibr_Resource resource = ibr_allocPassResource(graph, commands, (ibr_ResourceDesc)
													  {
														  .Type = ibr_ResourceType_Buffer,
														  .Flags = ibr_ResourceFlag_Transient,
														  .BufferDesc =
														  {
															  .Usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
															  .Size = sizeof(Data),
															  .RequiredMemoryFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
														  }
													  });

		ibr_writeResource(graph, commands, &resource, (ib_WriteData)
						  {
							  .Data = &data,
							  .Size = sizeof(data),
						  });

		ib_ShaderInput input = ibr_resourcesToShaderInput(graph, (ibr_ResourceToShaderInputDesc)
														  {
															  .Layout = &pipeline.InlineShaderInputLayouts[0],
															  .ShaderInputs = ib_staticArrayRange(computeInputs),
															  .Resources = ib_staticArrayRange(
																  (ibr_Resource*[])
																  {
																	  &resource
																  })
														  });
		vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE,
								pipeline.Layout,
								0u,
								1u,
								&input.DescriptorSet,
								0u,
								NULL);
		vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.VulkanPipeline);
	
		vkCmdDispatch(commands, 128u, 128u, 1u);

		vkEndCommandBuffer(commands);
		ibr_submitCommandBuffers(graph, (ibr_SubmitCommandBufferDesc)
								 {
									 .Queue = ib_Queue_Graphics,
									 .CommandBuffers = commands,
									 .SubmitFence = graph->FrameFence
								 });
		ibr_endFrame(graph);

		vkDeviceWaitIdle(core.LogicalDevice);
		Sleep(16);

		activeFrameIndex = (activeFrameIndex + 1) % 2;
	}

    ib_freeComputePipeline(&core, &pipeline);
    ibr_killRenderGraphs(&core, graphs, ib_arrayCount(graphs));
    ib_killCore(&core);

    return 0;
}