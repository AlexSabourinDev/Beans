#include "ib_Sampler.hsh"

struct RasterizerParams
{
	uint2 OutputDimensions;
};

[[vk::binding(0)]] ConstantBuffer<RasterizerParams> Params;
[[vk::binding(1)]] Texture2D<float4> Input;
[[vk::binding(2)]] SamplerState Samplers[ib_Sampler_Count];
[[vk::binding(3)]] RWTexture2D<float4> Output;

[numthreads(8,8,1)]
void CS(uint2 aDispatchThreadId : SV_DispatchThreadId)
{
    if (any(aDispatchThreadId >= Params.OutputDimensions))
    {
        return;
    }

    uint const blockSize = 32;
    uint2 const checkerboardTexel = (aDispatchThreadId / blockSize) % 2;
    float2 uv = ((float2)aDispatchThreadId + 0.5f) / (float2)Params.OutputDimensions * 8.0f;
    Output[aDispatchThreadId] = Input[checkerboardTexel];
}