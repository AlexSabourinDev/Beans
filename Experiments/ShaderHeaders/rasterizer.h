// Generated header file for Rasterizer.
#include <iceberg/ib_core.h>
#include <stdint.h>

typedef enum RasterizerInputEnum
{
	Rasterizer_Params = 0,
	Rasterizer_Input = 1,
	Rasterizer_Samplers = 2,
	Rasterizer_Output = 3,
} RasterizerInputEnum;

static ib_ShaderInputDesc const RasterizerInputs[] = 
{
	{ .Shaders = VK_SHADER_STAGE_COMPUTE_BIT, .Type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .UseImmutableSamplers = false },
	{ .Shaders = VK_SHADER_STAGE_COMPUTE_BIT, .Type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .UseImmutableSamplers = false },
	{ .Shaders = VK_SHADER_STAGE_COMPUTE_BIT, .Type = VK_DESCRIPTOR_TYPE_SAMPLER, .UseImmutableSamplers = true },
	{ .Shaders = VK_SHADER_STAGE_COMPUTE_BIT, .Type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .UseImmutableSamplers = false },
};

typedef struct RasterizerParams
{
	float OutputDimensions[2];
	uint64_t MeshAddress;
	uint64_t StackAddress;
	uint64_t RasterTileAddress;
	uint32_t IndexCount;
	uint32_t VertexCount;
} RasterizerParams;

const struct { uint32_t x, y, z; } RasterizerThreadGroupSize = { 8, 8, 1 };
