#include "ib_Sampler.hsh"

// References:
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#15.4%20Clipping

struct RasterizerParams
{
    float2 OutputDimensions;
    uint64_t MeshAddress;
    uint IndexCount;
    uint VertexCount;
    float4x4 ProjectionFromWorld;
};

[[vk::binding(0)]] ConstantBuffer<RasterizerParams> Params;
[[vk::binding(1)]] Texture2D<float4> Input;
[[vk::binding(2)]] SamplerState Samplers[ib_Sampler_Count];
[[vk::binding(3)]] RWTexture2D<float4> Output;

uint16_t3 loadTriangle(uint index)
{
    return vk::RawBufferLoad<uint16_t3>(Params.MeshAddress + index * sizeof(uint16_t));
}

template<int C>
vector<float, C> toFloatingPoint(vector<float, C> fixedPoint, float fractionalBits)
{
    return fixedPoint / exp2(fractionalBits);
}

float3 loadVertexPos(uint vertexIndex)
{
    uint indexOffset = Params.IndexCount * sizeof(uint16_t);
    int16_t3 packedPos = vk::RawBufferLoad<int16_t3>(Params.MeshAddress + indexOffset + vertexIndex * sizeof(uint16_t3));

    return toFloatingPoint((float3)packedPos, 7.0f);
}

float roundToFixedPoint(float value)
{
    float const fractionBits = 8.0f;
    float const integerBits = 16.0f;

    // Based on https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#FLOATtoFIXED
    // Don't bother handling values outside range. Just return 0.
    // TODO: Revisit later, we probably want to either assert or clip.
    if (!isfinite(value))
    {
        return 0.0f;
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

    // Is there a better way to round to a fixed decimal place?
    return round(fixedPoint) / scale;
}

float2 roundToFixedPoint(float2 value)
{
    for (uint i = 0; i < 2; i++)
    {
        value[i] = roundToFixedPoint(value[i]);
    }
    return value;
}

// https://www.cs.drexel.edu/~deb39/Classes/Papers/comp175-06-pineda.pdf
// https://www.scratchapixel.com/lessons/3d-basic-rendering/rasterization-practical-implementation/rasterization-stage.html
// Essentially a cross product that tells us the direction
float ccwEdgeFunction(float2 p, float2 v1, float2 v0)
{
    return (v1.x - v0.x) * (p.y - v0.y)-(v1.y - v0.y) * (p.x - v0.x);
}

// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#15.4%20Clipping
// Clearly explained here: https://fgiesen.wordpress.com/2013/02/08/triangle-rasterization-in-practice/
bool isCCWTopLeftEdge(float2 v1, float2 v0)
{
    float2 e = v1 - v0;
    bool horizontalEdge = (e.y == 0.0f);
    bool leftTraveling = e.x < 0.0f;

    // Anything going down is a left edge due to CCW winding.
    bool leftEdge = e.y < 0.0f;

    return (horizontalEdge && leftTraveling) || leftEdge;
}

[numthreads(8,8,1)]
void CS(uint2 aDispatchThreadId : SV_DispatchThreadId)
{
    if (any(aDispatchThreadId >= Params.OutputDimensions))
    {
        return;
    }

    float2 pixCoord = (float2)aDispatchThreadId + 0.5f;

    float4 output = 0.0f;
    [loop]
    for (uint i = 0; i < Params.IndexCount; i+=3)
    {
        uint16_t3 tri = loadTriangle(i);

        float2 triPixelCoord[3];
        for (uint v = 0; v < 3; v++)
        {
            float3 localPos = loadVertexPos(tri[v]);
            // No world matrix right now
            float4 clipPos = mul(Params.ProjectionFromWorld, float4(localPos, 1.0f));
            float3 ndcPos = clipPos.xyz / clipPos.w;
            float2 vertexUV = mad(ndcPos.xy, 0.5f, 0.5f);
            triPixelCoord[v] = roundToFixedPoint(vertexUV * Params.OutputDimensions);
        }

        if (all(aDispatchThreadId == 0))
        {
            //printf("%v2f, %v2f, %v2f", triPixelCoord[0], triPixelCoord[1], triPixelCoord[2]);
        }

        float e0 = ccwEdgeFunction(pixCoord, triPixelCoord[1], triPixelCoord[0]);
        float e1 = ccwEdgeFunction(pixCoord, triPixelCoord[2], triPixelCoord[1]);
        float e2 = ccwEdgeFunction(pixCoord, triPixelCoord[0], triPixelCoord[2]);

        // https://fgiesen.wordpress.com/2013/02/08/triangle-rasterization-in-practice/
        // TODO: Can we apply a bias here like suggested in the link above?
        bool topLeft0 = (e0 == 0.0f) && isCCWTopLeftEdge(triPixelCoord[1], triPixelCoord[0]);
        bool topLeft1 = (e1 == 0.0f) && isCCWTopLeftEdge(triPixelCoord[2], triPixelCoord[1]);
        bool topLeft2 = (e2 == 0.0f) && isCCWTopLeftEdge(triPixelCoord[0], triPixelCoord[2]);

        if ((e0 > 0.0f || topLeft0)
            && (e1 > 0.0f || topLeft1)
            && (e2 > 0.0f || topLeft2))
        {
            output += 0.5f;
        }
    }

    Output[aDispatchThreadId] = output;
}