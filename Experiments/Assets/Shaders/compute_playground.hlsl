struct Data
{
	float4 Floats;
	float16_t2 Half0;
	float16_t2 Half1;
};

[[vk::binding(0)]] ConstantBuffer<Data> Constants;

[numthreads(1,1,1)]
void CS()
{
	printf("Floats: %f, %f, %f, %f", Constants.Floats.x, Constants.Floats.y, Constants.Floats.z, Constants.Floats.w);
	printf("Halfs: %f, %f, %f, %f", Constants.Half0.x, Constants.Half0.y, Constants.Half1.x, Constants.Half1.y);
}