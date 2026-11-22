// Generated header file for ComputePlayground.
#include <iceberg/ib_core.h>
#include <stdint.h>

typedef enum ComputePlaygroundInputEnum
{
	ComputePlayground_Constants = 0,
} ComputePlaygroundInputEnum;

static ib_ShaderInputDesc const ComputePlaygroundInputs[] = 
{
	{ .Shaders = VK_SHADER_STAGE_COMPUTE_BIT, .Type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .UseImmutableSamplers = false },
};

typedef struct Data
{
	float Padding[4];
	float Padding0[3];
	float Padding1;
	float Padding2[4];
	float _0;
	uint32_t _1;
	uint32_t _2;
	float _3[2];
	float _4[2];
	float _5[2];
	float _6[2];
	float _7[2];
	float Padding3[4];
} Data;

const struct { uint32_t x, y, z; } ComputePlaygroundThreadGroupSize = { 32, 1, 1 };
