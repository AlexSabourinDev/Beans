
[[vk::binding(0)]] RWTexture2D<float4> Output;

struct BackgroundConstants
{
    uint2 OutputDimensions;
};
[[vk::binding(1)]] ConstantBuffer<BackgroundConstants> Constants;

[numthreads(8, 8, 1)]
void CS(uint2 aDispatchThreadId : SV_DispatchThreadID)
{
    if (any(aDispatchThreadId >= Constants.OutputDimensions))
    {
        return;
    }
    Output[aDispatchThreadId] = float4(1.0f, 0.0f, 1.0f, 1.0f);
}
