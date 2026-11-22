// Generated header file for TriangleCulling.
#include <iceberg/ib_core.h>
#include <stdint.h>

typedef enum TriangleCullingInputEnum
{
	TriangleCulling_Params = 0,
} TriangleCullingInputEnum;

static ib_ShaderInputDesc const TriangleCullingInputs[] = 
{
	{ .Shaders = VK_SHADER_STAGE_COMPUTE_BIT, .Type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .UseImmutableSamplers = false },
};

typedef struct CullingParams
{
	float ProjectionFromWorld[4][4];
	float OutputDimensions[2];
	uint64_t MeshAddress;
	uint64_t StackAddress;
	uint64_t RasterTileAddress;
	uint32_t TileCount[2];
	float InvTileDims[2];
	uint32_t IndexCount;
	uint32_t VertexCount;
} CullingParams;

const struct { uint32_t x, y, z; } TriangleCullingThreadGroupSize = { 64, 1, 1 };
