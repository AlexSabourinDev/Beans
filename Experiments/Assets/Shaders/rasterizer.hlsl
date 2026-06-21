#include "ib_sampler.hsh"
#include "ib_encodings.hsh"
#include "ib_math.hsh"

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

float2 loadNormalOct(uint vertexIndex)
{
    uint indexOffset = Params.IndexCount * sizeof(uint16_t);
    uint positionOffset = Params.VertexCount * sizeof(int16_t3);
    uint readOffset = indexOffset + positionOffset;
    uint16_t2 packedOct = vk::RawBufferLoad<uint16_t2>(Params.MeshAddress + readOffset + vertexIndex * sizeof(uint16_t2));
    return toFloatingPoint((float2)packedOct, 15.0f);
}

float2 loadUV(uint vertexIndex)
{
    uint indexOffset = Params.IndexCount * sizeof(uint16_t);
    uint positionOffset = Params.VertexCount * sizeof(int16_t3);
    uint normalOffset = Params.VertexCount * sizeof(int16_t2);
    uint readOffset = indexOffset + positionOffset + normalOffset;

    uint32_t alignementMask = sizeof(float) - 1;
    readOffset = (readOffset + alignementMask) & ~alignementMask;

    return vk::RawBufferLoad<float2>(Params.MeshAddress + readOffset + vertexIndex * sizeof(float2));
}

template<int C>
vector<float, C> interpolate(vector<float, C> a0, vector<float, C> a1, vector<float, C> a2, float3 barycentrics)
{
    vector<float, C> result = 0.0f;
    result += a0 * barycentrics.x;
    result += a1 * barycentrics.y;
    result += a2 * barycentrics.z;
    return result;
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

struct NDCTri
{
    float4 Verts[3];
    uint16_t3 Indices;
};

static uint const ThreadGroupXY = 8u;
static uint const ThreadGroupSize = ThreadGroupXY * ThreadGroupXY;
static uint const RasterizationQueueSize = ThreadGroupSize;
groupshared NDCTri RasterizationQueue[RasterizationQueueSize];
groupshared uint RasterizationQueueCount;

[numthreads(ThreadGroupXY,ThreadGroupXY,1)]
void CS(uint2 dispatchThreadId : SV_DispatchThreadId, uint groupIndex : SV_GroupIndex)
{
    float pixDepth = 1.0f;
    uint16_t3 pixTri = 0;
    float3 barycentrics = 0.0f;
    [loop]
    for (uint batchIndex = 0u; batchIndex < Params.IndexCount; batchIndex += ThreadGroupSize)
    {
        // Clear our queue count before doing another batch.
        if (groupIndex == 0)
        {
            RasterizationQueueCount = 0u;
        }
        GroupMemoryBarrierWithGroupSync();

        // Transform `ThreadGroupSize` triangles
        uint triangleIndex = batchIndex + groupIndex * 3;
        if (triangleIndex < Params.IndexCount)
        {
            uint16_t3 tri = loadTriangle(triangleIndex);
            float4 triPixelVert[3];
            for (uint v = 0; v < 3; v++)
            {
                float3 localPos = loadVertexPos(tri[v]);
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
                uint writeIndex;
                InterlockedAdd(RasterizationQueueCount, 1u, writeIndex);
                RasterizationQueue[writeIndex].Indices = tri;
                for (uint w = 0; w < 3; w++)
                {
                    RasterizationQueue[writeIndex].Verts[w] = triPixelVert[w];
                }
            }
        }
        GroupMemoryBarrierWithGroupSync();

        float2 pixCoord = (float2)dispatchThreadId + 0.5f;
        // Rasterize front facing triangles
        for (uint i = 0; i < RasterizationQueueCount; i++)
        {
            NDCTri tri = RasterizationQueue[i];

            float e0 = ccwEdgeFunction(pixCoord, tri.Verts[1].xy, tri.Verts[0].xy);
            float e1 = ccwEdgeFunction(pixCoord, tri.Verts[2].xy, tri.Verts[1].xy);
            float e2 = ccwEdgeFunction(pixCoord, tri.Verts[0].xy, tri.Verts[2].xy);

            // https://fgiesen.wordpress.com/2013/02/08/triangle-rasterization-in-practice/
            // TODO: Can we apply a bias here like suggested in the link above?
            // How do we simplify this expression?
            bool topLeft0 = (e0 == 0.0f) && isCCWTopLeftEdge(tri.Verts[1].xy, tri.Verts[0].xy);
            bool topLeft1 = (e1 == 0.0f) && isCCWTopLeftEdge(tri.Verts[2].xy, tri.Verts[1].xy);
            bool topLeft2 = (e2 == 0.0f) && isCCWTopLeftEdge(tri.Verts[0].xy, tri.Verts[2].xy);

            if ((e0 > 0.0f || topLeft0)
                && (e1 > 0.0f || topLeft1)
                && (e2 > 0.0f || topLeft2))
            {
                float totalArea = e0 + e1 + e2;
                float3 screenBarycentrics = { e1 / totalArea, e2 / totalArea, e0 / totalArea };
                float sampleDepth = dot(float3(tri.Verts[0].z, tri.Verts[1].z, tri.Verts[2].z), screenBarycentrics);

                if (sampleDepth < pixDepth && sampleDepth >= 0.0f)
                {
                    pixTri = tri.Indices;

                    float3 perVertexRcpW = rcp(float3(tri.Verts[0].w, tri.Verts[1].w, tri.Verts[2].w));

                    // Interpolate 1/W along our screenspace triangle then recover W.
                    float interpolatedW = rcp(dot(perVertexRcpW, screenBarycentrics));

                    // When we want to transform our coordinate into perspective correct interpolation
                    // Apply our per vertex 1/W to get Attribute0/W0
                    // Then apply our screenspace barycentric
                    // Attribute0/W0*Barycentric0
                    // And finally apply our interpolated W
                    // Attribute0/W0*Barycentric0*InterpolatedW
                    //
                    // We just bake these operations into a worldBarycentrics variable to apply to our attributes later down the line.
                    barycentrics = perVertexRcpW * screenBarycentrics * interpolatedW;
                    pixDepth = sampleDepth;
                }
            }
        }

        // Make sure our batch is complete before moving onto the next one.
        // We're going to clear our triangle queue on the next iteration
        // And we want to make sure we're not clearing it while some pixels are still reading the value.
        // TODO: Could skip this barrier if we're on the last iteration.
        GroupMemoryBarrierWithGroupSync();
    }

    if (any(dispatchThreadId < Params.OutputDimensions))
    {
        float4 output = 0.0f;
        if (any(pixTri != 0))
        {
            float2 uv0 = loadUV(pixTri[0]);
            float2 uv1 = loadUV(pixTri[1]);
            float2 uv2 = loadUV(pixTri[2]);

            float2 uv = interpolate(uv0, uv1, uv2, barycentrics);

            float2 oct0 = loadNormalOct(pixTri[0]);
            float2 oct1 = loadNormalOct(pixTri[1]);
            float2 oct2 = loadNormalOct(pixTri[2]);
            float3 normal = fromSquareOctahedral(interpolate(oct0, oct1, oct2, barycentrics));

            float3 albedo = Input.SampleLevel(Samplers[ib_Sampler_NearestRepeat], uv * 10.0f, 0.0f).rgb;
            float3 light = normalize(float3(1.0f, 1.0f, 0.0f));
            output.rgb += mad(dot(light, normal), 0.5f, 0.5f) * albedo / Pi;
        }

        Output[dispatchThreadId] = output;
    }
}