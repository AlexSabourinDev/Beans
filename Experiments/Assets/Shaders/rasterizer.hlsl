#include "ib_Sampler.hsh"

struct RasterizerParams
{
    uint2 OutputDimensions;
    uint64_t MeshAddress;
    uint IndexCount;
    uint VertexCount;
};

[[vk::binding(0)]] ConstantBuffer<RasterizerParams> Params;
[[vk::binding(1)]] Texture2D<float4> Input;
[[vk::binding(2)]] SamplerState Samplers[ib_Sampler_Count];
[[vk::binding(3)]] RWTexture2D<float4> Output;

uint16_t3 loadTriangle(uint index)
{
    return vk::RawBufferLoad<uint16_t3>(Params.MeshAddress + index * sizeof(uint16_t3));
}

template<int C>
vector<float, C> toFloatingPoint(vector<float, C> fixedPoint, float fractionalBits)
{
    return fixedPoint / exp2(fractionalBits);
}

float3 loadVertexPos(uint vertexIndex)
{
    uint indexOffset = Params.IndexCount * sizeof(uint16_t);
    uint16_t3 packedPos = vk::RawBufferLoad<uint16_t3>(Params.MeshAddress + indexOffset + vertexIndex * sizeof(uint16_t3));

    return toFloatingPoint((float3)packedPos, 7.0f);
}

// 16.8 Fixed Point
struct i16f8
{
    float Value;
};

i16f8 toFixedPoint(float value)
{
    float const fractionBits = 8.0f;
    float const integerBits = 16.0f;

    // Based on https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#FLOATtoFIXED
    // Don't bother handling values outside range. Just return 0.
    // TODO: Revisit later, we probably want to either assert or clip.
    if (!isfinite(value))
    {
        return (i16f8)0;
    }

    float scale = exp2(fractionBits);

    float minFixed = -exp2(integerBits - 1.0f);
    float maxFixed = exp2(integerBits - 1.0f) - exp2(-fractionBits);

    float fixedPoint;
    if (value >= maxFixed)
    {
        fixedPoint = maxFixed * scale;
    }
    else if (value <= minFixed)
    {
        fixedPoint = minFixed * scale;
    }
    else
    {
        fixedPoint = value * scale;
    }

    i16f8 result;
    result.Value = round(fixedPoint);
    return result;
}

[numthreads(8,8,1)]
void CS(uint2 aDispatchThreadId : SV_DispatchThreadId)
{
    if (any(aDispatchThreadId >= Params.OutputDimensions))
    {
        return;
    }

    float4 output = 0.0f;
    [loop]
    for (uint i = 0; i < Params.IndexCount; i+=3)
    {
        uint16_t3 tri = loadTriangle(i);
        for (uint v = 0; v < 3; v++)
        {
            float3 p = loadVertexPos(tri[v]);
            output.xyz += p;
        }
    }

    float2 uv = ((float2)aDispatchThreadId + 0.5) / (float2)Params.OutputDimensions;
    Output[aDispatchThreadId] = Input.SampleLevel(Samplers[ib_Sampler_NearestRepeat], uv * 32, 0.0f);
}