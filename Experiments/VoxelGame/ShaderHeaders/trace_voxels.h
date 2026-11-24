// Generated header file for TraceVoxels.
#include <iceberg/ib_core.h>
#include <stdint.h>

typedef enum sg_TraceVoxelsInputEnum
{
	TraceVoxels_Output = 0,
	TraceVoxels_Constants = 1,
} sg_TraceVoxelsInputEnum;

static ib_ShaderInputDesc const sg_TraceVoxelsInputs[] = 
{
	{ .Shaders = VK_SHADER_STAGE_COMPUTE_BIT, .Type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .UseImmutableSamplers = false },
	{ .Shaders = VK_SHADER_STAGE_COMPUTE_BIT, .Type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .UseImmutableSamplers = false },
};

typedef struct sg_Transform
{
	float Rotation[4];
	float Position[3];
	float Scale;
} sg_Transform;

typedef struct sg_VoxelTraceConstants
{
	sg_Transform ViewToWorld;
	float InvProjection[4];
	float InvOutputDimensions[2];
} sg_VoxelTraceConstants;

const struct { uint32_t x, y, z; } sg_TraceVoxelsThreadGroupSize = { 8, 8, 1 };
