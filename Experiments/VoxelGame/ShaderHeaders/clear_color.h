// Generated header file for ClearColor.
#include <iceberg/ib_core.h>
#include <stdint.h>

typedef enum sg_ClearColorInputEnum
{
	ClearColor_Output = 0,
	ClearColor_Constants = 1,
} sg_ClearColorInputEnum;

static ib_ShaderInputDesc const sg_ClearColorInputs[] = 
{
	{ .Shaders = VK_SHADER_STAGE_COMPUTE_BIT, .Type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .UseImmutableSamplers = false },
	{ .Shaders = VK_SHADER_STAGE_COMPUTE_BIT, .Type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .UseImmutableSamplers = false },
};

typedef struct sg_ClearColorConstants
{
	uint32_t OutputDimensions[2];
	uint32_t Color;
} sg_ClearColorConstants;

const struct { uint32_t x, y, z; } sg_ClearColorThreadGroupSize = { 8, 8, 1 };
