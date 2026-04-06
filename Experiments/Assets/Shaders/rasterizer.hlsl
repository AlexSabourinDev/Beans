struct RasterizerParams
{
	uint2 OutputDimensions;
};

[[vk::binding(0)]] cbuffer RasterizerConstants
{
	RasterizerParams Params;
};

[[vk::binding(1)]] RWTexture2D<float4> Output;

[numthreads(8,8,1)]
void CS(uint2 aDispatchThreadId : SV_DispatchThreadId)
{
	Output[aDispatchThreadId] = float4(1.0f, 1.0f, 0.0f, 1.0f);
}