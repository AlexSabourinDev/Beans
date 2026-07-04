#include "ib_sampler.hsh"
#include "ib_encodings.hsh"
#include "ib_math.hsh"
#include "ib_vulkan.hsh"
#include "rasterizer.hsh"
#include "mesh_bindless.hsh"

// References:
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#15.4%20Clipping

struct RasterizerParams
{
    float2 OutputDimensions;
    uint64_t MeshAddress;
    uint IndexCount;
    uint VertexCount;
};

[[vk::binding(0)]] ConstantBuffer<RasterizerParams> Params;
[[vk::binding(1)]] Texture2D<float4> Input;
[[vk::binding(2)]] SamplerState Samplers[ib_Sampler_Count];
[[vk::binding(3)]] RWTexture2D<float4> Output;
[[vk::binding(4)]] ByteAddressBuffer PostTransformCache;

template<int C>
vector<float, C> interpolate(vector<float, C> a0, vector<float, C> a1, vector<float, C> a2, float3 barycentrics)
{
    vector<float, C> result = 0.0f;
    result += a0 * barycentrics.x;
    result += a1 * barycentrics.y;
    result += a2 * barycentrics.z;
    return result;
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

static uint const ThreadGroupXY = 8u;

[numthreads(ThreadGroupXY,ThreadGroupXY,1)]
void CS(uint2 dispatchThreadId : SV_DispatchThreadId, uint groupIndex : SV_GroupIndex, uint2 groupID : SV_GroupID)
{
    // TODO: Batch triangles per tile instead of rasterizing every triangle.

    if (sizeof(NDCTri) != sizeof(float) * 13)
    {
        printf("Expecting NDCTri to be 13 dwords. Actual size is %u", sizeof(NDCTri));
    }

    float pixDepth = 1.0f;
    uint triangleIndex = 0xFFFFFFFF;
    float3 barycentrics = 0.0f;

    float2 pixCoord = (float2)dispatchThreadId + 0.5f;

    uint triangleCount = PostTransformCache.Load(0u);
    for (uint triIndex = 0u; triIndex < triangleCount; triIndex++)
    {
        uint writeOffset = sizeof(uint);
        NDCTri tri = PostTransformCache.Load<NDCTri>(writeOffset + triIndex * sizeof(NDCTri));

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
                triangleIndex = tri.TriangleIndex;
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

    if (any(dispatchThreadId < Params.OutputDimensions))
    {
        float4 output = 0.0f;
        if (triangleIndex != 0xFFFFFFFF)
        {
            uint16_t3 pixTri = loadTriangle(Params.MeshAddress, triangleIndex);

            float2 uv0 = loadUV(Params.MeshAddress, Params.IndexCount, Params.VertexCount, pixTri[0]);
            float2 uv1 = loadUV(Params.MeshAddress, Params.IndexCount, Params.VertexCount, pixTri[1]);
            float2 uv2 = loadUV(Params.MeshAddress, Params.IndexCount, Params.VertexCount, pixTri[2]);

            float2 uv = interpolate(uv0, uv1, uv2, barycentrics);

            float2 oct0 = loadNormalOct(Params.MeshAddress, Params.IndexCount, Params.VertexCount, pixTri[0]);
            float2 oct1 = loadNormalOct(Params.MeshAddress, Params.IndexCount, Params.VertexCount, pixTri[1]);
            float2 oct2 = loadNormalOct(Params.MeshAddress, Params.IndexCount, Params.VertexCount, pixTri[2]);
            float3 normal = fromSquareOctahedral(interpolate(oct0, oct1, oct2, barycentrics));

            float3 albedo = Input.SampleLevel(Samplers[ib_Sampler_NearestRepeat], uv * 10.0f, 0.0f).rgb;
            float3 light = normalize(float3(1.0f, 1.0f, 0.0f));
            output.rgb += mad(dot(light, normal), 0.5f, 0.5f) * albedo / Pi;
        }

        Output[dispatchThreadId] = output;
    }
}