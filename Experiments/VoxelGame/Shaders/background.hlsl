
[[vk::binding(0)]] RWTexture2D<float4> Output;

struct BackgroundConstants
{
    
};
[[vk::binding(1)]] ConstantBuffer<

[numthreads(8, 8, 1)]
void CS(uint2 aDispatchThreadId : SV_DispatchThreadID)
{
    Output[aDispatchThreadId] = 0.0f;
}
