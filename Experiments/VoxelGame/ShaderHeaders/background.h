// Generated header file for Background.
#include <iceberg/ib_core.h>
#include <stdint.h>

typedef enum BackgroundInputEnum
{
	Background_Output = 0,
	Background_Constants = 1,
} BackgroundInputEnum;

static ib_ShaderInputDesc const BackgroundInputs[] = 
{
	{ .Shaders = VK_SHADER_STAGE_COMPUTE_BIT, .Type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .UseImmutableSamplers = false },
	{ .Shaders = VK_SHADER_STAGE_COMPUTE_BIT, .Type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .UseImmutableSamplers = false },
};

typedef struct BackgroundConstants
{
	uint32_t OutputDimensions[2];
} BackgroundConstants;

const struct { uint32_t x, y, z; } BackgroundThreadGroupSize = { 8, 8, 1 };
