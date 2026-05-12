struct Data
{
	float4 Padding[8];
	float3 Padding0;
	float Padding1;
	float4 Padding2;
	float _0;
	uint _1;
	uint _2;
	float16_t2 _3;
	float16_t2 _4;
	float16_t2 _5;
	float16_t2 _6;
	float16_t2 _7;
	float4 Padding3;
};

[[vk::binding(0)]] ConstantBuffer<Data> Constants;

[numthreads(32,1,1)]
void CS(uint3 dispatchId : SV_DispatchThreadId)
{
	if (all(dispatchId == 0))
	{
		float2 v0 = Constants._3;
		float2 v1 = Constants._4;
		printf("Halfs: %v2f", v0 + v1);
	}
}