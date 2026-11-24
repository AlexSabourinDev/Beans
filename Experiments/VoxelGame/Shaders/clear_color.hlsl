#include "ib_encodings.hsh"

[[vk::binding(0)]] RWTexture2D<float4> Output;

struct ClearColorConstants
{
    uint2 OutputDimensions;
    uint Color;
};
[[vk::binding(1)]] ConstantBuffer<ClearColorConstants> Constants;

[numthreads(8, 8, 1)]
void CS(uint2 dispatchThreadID : SV_DispatchThreadID)
{
    if (any(dispatchThreadID >= Constants.OutputDimensions))
    {
        return;
    }
    Output[dispatchThreadID] = unpackRGBA(Constants.Color);
}
