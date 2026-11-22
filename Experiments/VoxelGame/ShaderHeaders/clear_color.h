// Generated header file for ClearColor.
#include <iceberg/ib_core.h>
#include <stdint.h>

typedef enum ClearColorInputEnum
{
	ClearColor_Output = 0,
	ClearColor_Constants = 1,
} ClearColorInputEnum;

static ib_ShaderInputDesc const ClearColorInputs[] = 
{
	{ .Shaders = VK_SHADER_STAGE_COMPUTE_BIT, .Type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .UseImmutableSamplers = false },
	{ .Shaders = VK_SHADER_STAGE_COMPUTE_BIT, .Type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .UseImmutableSamplers = false },
};

typedef struct ClearColorConstants
{
	uint32_t OutputDimensions[2];
	uint32_t Color;
} ClearColorConstants;

const struct { uint32_t x, y, z; } ClearColorThreadGroupSize = { 8, 8, 1 };
