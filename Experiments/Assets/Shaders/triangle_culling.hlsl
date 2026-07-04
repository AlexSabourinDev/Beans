#include "mesh_bindless.hsh"
#include "rasterizer.hsh"

struct CullingParams
{
    float4x4 ProjectionFromWorld;
    float2 OutputDimensions;
    uint64_t MeshAddress;
    uint IndexCount;
    uint VertexCount;
};

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

[[vk::binding(0)]] ConstantBuffer<CullingParams> Params;
[[vk::binding(1)]] RWByteAddressBuffer PostTransformCache;

static uint const ThreadGroupX = 64u;

[numthreads(ThreadGroupX, 1, 1)]
void CS(uint dispatchThreadId : SV_DispatchThreadId, uint groupIndex : SV_GroupIndex)
{
    uint triIndex = dispatchThreadId;
    if (triIndex * 3 < Params.IndexCount)
    {
        uint16_t3 tri = loadTriangle(Params.MeshAddress, triIndex);
        float4 triPixelVert[3];
        for (uint v = 0; v < 3; v++)
        {
            float3 localPos = loadVertexPos(Params.MeshAddress, Params.IndexCount, tri[v]);
            // No world matrix right now
            float4 clipPos = mul(Params.ProjectionFromWorld, float4(localPos, 1.0f));
            float3 ndcPos = clipPos.xyz / clipPos.w;
            float2 vertexUV = mad(ndcPos.xy, 0.5f, 0.5f);
            triPixelVert[v] = float4(roundToFixedPoint(vertexUV * Params.OutputDimensions), ndcPos.z, clipPos.w);
        }

        float2 v1ToV0 = triPixelVert[1].xy - triPixelVert[0].xy;
        float2 v2ToV0 = triPixelVert[2].xy - triPixelVert[0].xy;
        bool frontfacing = (v1ToV0.x * v2ToV0.y)-(v1ToV0.y * v2ToV0.x) > 0.0f;
        if (frontfacing)
        {
            // TODO: Use groupshared as primary backing,
            // Then flush to global memory.
            // TODO: PostTransformCache needs worst case memory. Can we make it dynamic?
            // Or maybe compute the number of transformed vertices?
            // Or maybe shrink it once we know how large it should be?
            uint writeIndex;
            PostTransformCache.InterlockedAdd(0u, 1u, writeIndex);

            NDCTri ndcTri = (NDCTri)0;
            for (uint i = 0; i < 3; i++)
            {
                ndcTri.Verts[i] = triPixelVert[i];
            }
            ndcTri.TriangleIndex = triIndex;

            uint writeOffset = sizeof(uint);
            PostTransformCache.Store<NDCTri>(writeOffset + writeIndex * sizeof(NDCTri), ndcTri);
        }
    }
}