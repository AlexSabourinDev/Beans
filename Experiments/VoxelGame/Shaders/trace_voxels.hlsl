#include <ib_math.hsh>
#include <ib_encodings.hsh>

struct Transform
{
    float4 Rotation;
    float3 Position;
    float Scale;
};

float4 quatConjugate(float4 q)
{
    return float4(-q.x, -q.y, -q.z, q.w);
}

float4 quatInverse(float4 q)
{
    return quatConjugate(q);
}

// https://fgiesen.wordpress.com/2019/02/09/rotating-a-single-vector-using-a-quaternion/
float3 quatRotate(float4 rotator, float3 p)
{
    float3 t = rotator.xyz * 2.0f;
    t = cross(t, p);

    float3 res = mad(t, rotator.w, p);
    return res + cross(rotator.xyz, t);
}

float3 transformPoint(Transform transform, float3 p)
{
    return quatRotate(transform.Rotation, p * transform.Scale) + transform.Position;
}

float3 invTransformPoint(Transform transform, float3 p)
{
    return quatRotate(quatInverse(transform.Rotation), p - transform.Position) / transform.Scale;
}

float3 invProjection(float4 invProjectionConstants, float3 uvw)
{
    // Projection is:
    // u = projectionConstants.x * x / z
    // v = projectionConstants.y * y / z
    // w = projectionConstants.z + projectionConstants.w / z
    // Solve for z:
    // w - projectionConstants.z = projectionConstants.w / z
    // 1 / (w - projectionConstants.z) = z / projectionConstants.w
    // projectionConstants.w / (w - projectionConstants.z) = z
    float3 pos;
    pos.z = invProjectionConstants.w / (uvw.z - invProjectionConstants.z);
    pos.x = (uvw.x * pos.z) * invProjectionConstants.x;
    pos.y = (uvw.y * pos.z) * invProjectionConstants.y;

    return pos;
}

struct VoxelTraceConstants
{
    Transform ViewToWorld;
    float4 InvProjection;
    float2 InvOutputDimensions;
};

// https://www.researchgate.net/publication/233899848_Efficient_implementation_of_the_3D-DDA_ray_traversal_algorithm_on_GPU_and_its_application_in_radiation_dose_calculation
float dda(float3 rayDir, float3 invRayDir, inout float3 voxel, inout float3 rayTracking)
{
    float minDist = min(min(rayTracking.x, rayTracking.y), rayTracking.z);

    if(rayTracking.x == minDist)
    {
        rayTracking.x += abs(invRayDir.x);
        voxel.x += rayDir.x > 0.0 ? 1.0 : -1.0;
    }
    else if(rayTracking.y == minDist)
    {
        rayTracking.y += abs(invRayDir.y);
        voxel.y += rayDir.y > 0.0 ? 1.0 : -1.0;
    }
    else if (rayTracking.z == minDist)
    {
        rayTracking.z += abs(invRayDir.z);
        voxel.z += rayDir.z > 0.0 ? 1.0 : -1.0;
    }

    return minDist;
}


[[vk::binding(0)]] RWTexture2D<float4> Output;
[[vk::binding(1)]] ConstantBuffer<VoxelTraceConstants> Constants;
[[vk::binding(2)]] StructuredBuffer<uint> VoxelPageBitmasks;
[[vk::binding(3)]] StructuredBuffer<uint> VoxelPageBrickIndirection;
[[vk::binding(4)]] StructuredBuffer<uint2> VoxelBrickBitmasks;

bool isBrickValid(uint3 brickId)
{
    uint brickIndex = packZCurve3D_256(brickId);
    uint arrIndex = brickIndex >> 5u;
    uint bitIndex = brickIndex & 31u;
    return (VoxelPageBitmasks[arrIndex] & (1u << bitIndex)) != 0;
}

struct VoxelHit
{
    float3 VoxelID;
    float HitT;
};

VoxelHit traceBricks(float3 origin, float3 viewDir, float brickWidth, float volumeWidth)
{
    // Traversal is much simpler in unit space where one voxel is one unit wide.
    // TODO: Also handle our page's origin
    float3 rescaledOrigin = origin / brickWidth;

    float3 voxelEdge = select(viewDir > 0.0f, 1.0f, 0.0f);
    float3 distanceToEdge = voxelEdge - frac(rescaledOrigin);
    float3 rayTracking = select(viewDir == 0.0f, FltMax, distanceToEdge / viewDir);
    float3 localBrick = floor(rescaledOrigin);

    VoxelHit hit = (VoxelHit)0;
    hit.HitT = -1.0f;

    while (true)
    {
        float rayT = dda(viewDir, 1.0f / viewDir, localBrick, rayTracking);
        if (any(localBrick < 0.0f) || any(localBrick >= volumeWidth))
        {
            break;
        }

        uint3 intBrickId = (uint3)localBrick;
        bool hitBrick = isBrickValid(intBrickId);
        if (hitBrick)
        {
            hit.VoxelID = localBrick;
            // Make sure to rescale our hit to world scale
            hit.HitT = rayT * brickWidth;
            break;
        }
    }

    return hit;
}

[numthreads(8, 8, 1)]
void CS(uint2 dispatchThreadID : SV_DispatchThreadID)
{
    float2 uv = ((float2)dispatchThreadID + 0.5f) * Constants.InvOutputDimensions;
    if (any(uv >= 1.0f))
    {
        return;
    }

    float3 farPosView = invProjection(Constants.InvProjection, float3(mad(uv, 2.0f, -1.0f), 1.0f));
    float3 farPosWorld = transformPoint(Constants.ViewToWorld, farPosView);
    float3 viewDir = normalize(farPosWorld);

    float3 origin = Constants.ViewToWorld.Position;

    // TODO: Change 64 to page width
    // TODO: Add max step count?
    float brickWidth = 2.0f;
    VoxelHit brickHit = traceBricks(origin, viewDir, brickWidth, 64.0f);
    if (brickHit.HitT >= 0.0f)
    {
        float3 voxelOrigin = (brickHit.VoxelID + 0.5f) * brickWidth;

        float3 hitPos = origin + brickHit.HitT * viewDir;
        float3 voxelToHit = hitPos - voxelOrigin;

        float maxComponent = max(abs(voxelToHit.x), max(abs(voxelToHit.y), abs(voxelToHit.z)));
        float3 normal;
        if (abs(voxelToHit.x) == maxComponent)
        {
            normal = float3(sign(voxelToHit.x), 0.0f, 0.0f);
        }
        else if (abs(voxelToHit.y) == maxComponent)
        {
            normal = float3(0.0f, sign(voxelToHit.y), 0.0f);
        }
        else if (abs(voxelToHit.z) == maxComponent)
        {
            normal = float3(0.0f, 0.0f, sign(voxelToHit.z));
        }

        Output[dispatchThreadID] = float4(normal, 1.0f);
        return;
    }

    Output[dispatchThreadID] = float4(viewDir, 1.0f);
}
