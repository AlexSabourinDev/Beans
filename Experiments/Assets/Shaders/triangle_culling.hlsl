#include "mesh_bindless.hsh"
#include "rasterizer.hsh"
#include "ib_math.hsh"

// #define DEBUG_ALLOCATIONS

struct CullingParams
{
    float4x4 ProjectionFromWorld;
    float2 OutputDimensions;
    uint64_t MeshAddress;
    uint64_t StackAddress;
    uint64_t RasterTileAddress;
    uint2 TileCount;
    float2 InvTileDims;
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

uint stackAlloc(uint64_t stackAddress, uint size, uint alignment)
{
    vk::BufferPointer<uint> stack = vk::BufferPointer<uint>(stackAddress);
    uint stackOffset;
    InterlockedAdd(stack.Get(), size + alignment - 1, stackOffset);

    uint allocOffset = stackOffset + sizeof(uint);

    uint alignmentMask = (alignment - 1);
    allocOffset = (allocOffset + alignmentMask) & (~alignmentMask);
    return allocOffset;
}

#ifdef DEBUG_ALLOCATIONS
groupshared uint DebugOverAllocationSize;
#endif // DEBUG_ALLOCATIONS

void writeToRasterTile(uint2 tile, uint triIndex)
{
    uint linearTileIndex = tile.y * Params.TileCount.x + tile.x;
    vk::BufferPointer<uint> rasterBatchHeadOffsetPtr = vk::BufferPointer<uint>(Params.RasterTileAddress + linearTileIndex * sizeof(uint));

    [loop]
    while (true)
    {
        uint currentRasterBatchHeadOffset = rasterBatchHeadOffsetPtr.Get();
        [branch]
        if (currentRasterBatchHeadOffset != 0)
        {
            vk::BufferPointer<RasterBatch> rasterBatchHeadPtr = vk::BufferPointer<RasterBatch>(Params.StackAddress + currentRasterBatchHeadOffset);

            uint writeIndex;
            InterlockedAdd(rasterBatchHeadPtr.Get().TriangleCount, 1u, writeIndex);

            [branch]
            if (writeIndex < RasterBatchSize)
            {
                rasterBatchHeadPtr.Get().TriangleIndices[writeIndex] = triIndex;
                break;
            }
        }

        // Scalarization loop to avoid allocating too much in our stack allocator. Yay!
        [loop]
        while (true)
        {
            uint scalarTileIndex = WaveReadLaneFirst(linearTileIndex);
            [branch]
            if (linearTileIndex == scalarTileIndex)
            {
                // Only allocate a batch through a single lane.
                [branch]
                if (WaveIsFirstLane())
                {
                    // TODO: Measure excessive stack allocations.
                    uint nextBatchOffset = stackAlloc(Params.StackAddress, sizeof(RasterBatch), sizeof(uint));
                    vk::BufferPointer<RasterBatch> nextBatch = vk::BufferPointer<RasterBatch>(Params.StackAddress + nextBatchOffset);

                    nextBatch.Get().Next = currentRasterBatchHeadOffset;
                    nextBatch.Get().TriangleCount = 0u;

                    uint originalValue;
                    InterlockedCompareExchange(rasterBatchHeadOffsetPtr.Get(), currentRasterBatchHeadOffset, nextBatchOffset, originalValue);

#ifdef DEBUG_ALLOCATIONS
                    if (originalValue != currentRasterBatchHeadOffset)
                    {
                        DebugOverAllocationSize += sizeof(RasterBatch);
                    }
#endif // DEBUG_ALLOCATIONS
                }
                break;
            }
        }
    }
}

static uint const ThreadGroupX = 64u;

groupshared NDCTri PostTransformCache[ThreadGroupX];
groupshared uint PostTransformCacheCount;
groupshared uint StackWriteOffset;

[numthreads(ThreadGroupX, 1, 1)]
void CS(uint dispatchThreadId : SV_DispatchThreadId, uint groupThreadIndex : SV_GroupIndex)
{
    if (groupThreadIndex == 0)
    {
        PostTransformCacheCount = 0u;
#ifdef DEBUG_ALLOCATIONS
        DebugOverAllocationSize = 0u;
#endif // DEBUG_ALLOCATIONS
    }
    GroupMemoryBarrierWithGroupSync();

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
            NDCTri ndcTri = (NDCTri)0;
            for (uint i = 0; i < 3; i++)
            {
                ndcTri.Verts[i] = triPixelVert[i];
            }
            ndcTri.TriangleIndex = triIndex;

            uint activeWaveCount = WaveActiveCountBits(true);
            uint waveWriteIndex;
            if (WaveIsFirstLane())
            {
                InterlockedAdd(PostTransformCacheCount, activeWaveCount, waveWriteIndex);
            }
            waveWriteIndex = WaveReadLaneFirst(waveWriteIndex);
            uint laneWriteIndex = waveWriteIndex + WavePrefixCountBits(true);

            PostTransformCache[laneWriteIndex] = ndcTri;
        }
    }

    GroupMemoryBarrierWithGroupSync();
    if (PostTransformCacheCount > 0)
    {
        if (groupThreadIndex == 0)
        {
            StackWriteOffset = stackAlloc(Params.StackAddress, PostTransformCacheCount * sizeof(NDCTri), sizeof(float4));
        }
        GroupMemoryBarrierWithGroupSync();

        for (uint i = WaveGetLaneIndex(); i < PostTransformCacheCount; i += WaveGetLaneCount())
        {
            NDCTri ndcTri = PostTransformCache[i];
            uint stackOffset = StackWriteOffset + i * sizeof(NDCTri);
            vk::RawBufferStore(Params.StackAddress + stackOffset, ndcTri);

            // TODO: We can probably figure out the covering tiles in a more efficient way :thinking:
            float2 triAABBMin = float2(FltMax, FltMax);
            float2 triAABBMax = float2(-FltMax, -FltMax);
            for (uint i = 0; i < 3; i++)
            {
                triAABBMin = min(triAABBMin, ndcTri.Verts[i].xy);
                triAABBMax = max(triAABBMax, ndcTri.Verts[i].xy);
            }

            float2 tileMin = floor(triAABBMin * Params.InvTileDims);
            float2 tileMax = ceil(triAABBMax * Params.InvTileDims);

            for (float y = tileMin.y; y <= tileMax.y; y += 1.0f)
            {
                for (float x = tileMin.x; x <= tileMax.x; x += 1.0f)
                {
                    writeToRasterTile(uint2((uint)x, (uint)y), stackOffset);
                }
            }
            
        }
    }

#ifdef DEBUG_ALLOCATIONS
    if (groupThreadIndex == 0 && DebugOverAllocationSize > 0)
    {
        printf("Group over allocated: %u.", DebugOverAllocationSize);
    }
#endif // DEBUG_ALLOCATIONS
}