//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#define MAX_RECURSION_DEPTH	1

typedef RaytracingAccelerationStructure RaytracingAS;
typedef BuiltInTriangleIntersectionAttributes TriAttributes;

//--------------------------------------------------------------------------------------
// Structs
//--------------------------------------------------------------------------------------
struct Vertex
{
	float3	Pos;
	float3	Nrm;
};

struct RayPayload
{
	float3	Color;
	uint	RecursionDepth;
};

struct GlobalConstants
{
	float4x3 Normal;
	uint	FrameIndex;
};

struct RayGenConstants
{
	matrix	ProjToWorld;
	float3	EyePt;
};

//--------------------------------------------------------------------------------------
// Constant buffers
//--------------------------------------------------------------------------------------
ConstantBuffer<GlobalConstants> g_cb : register(b0);
ConstantBuffer<RayGenConstants> l_cbRayGen : register (b1);

//--------------------------------------------------------------------------------------
// Texture and buffers
//--------------------------------------------------------------------------------------
RWTexture2D<float4>			RenderTarget	: register (u0);
RaytracingAS				g_scene			: register (t0);
Texture2D					g_normal		: register (t1);
Texture2D<float>			g_roughness		: register (t2);
Texture2D<float>			g_depth			: register (t3);
TextureCube<float3>			g_txEnv			: register (t4);

// IA buffers
Buffer<uint>				g_indexBuffers[]	: register (t0, space1);
StructuredBuffer<Vertex>	g_vertexBuffers[]	: register (t0, space2);

//--------------------------------------------------------------------------------------
// Samplers
//--------------------------------------------------------------------------------------
SamplerState g_sampler : register (s0);

static const uint g_waveXSize = 8;

float3 dFdx(float3 f)
{
	const uint laneIdx = WaveGetLaneIndex();
	const uint2 lanePos = { laneIdx % g_waveXSize, laneIdx / g_waveXSize };
	const uint quadPosX = lanePos.x >> 1;
	const float3 f0 = WaveReadLaneAt(f, lanePos.y * g_waveXSize + (quadPosX << 1));
	const float3 f1 = WaveReadLaneAt(f, lanePos.y * g_waveXSize + (quadPosX << 1) + 1);

	const float3 ddx = f1 - f0;

	return abs(ddx) > 0.5 ? 0.0 : ddx;
}

float3 dFdy(float3 f)
{
	const uint laneIdx = WaveGetLaneIndex();
	const uint2 lanePos = { laneIdx % g_waveXSize, laneIdx / g_waveXSize };
	const uint quadPosY = lanePos.y >> 1;
	const float3 f0 = WaveReadLaneAt(f, (quadPosY << 1) * g_waveXSize + lanePos.x);
	const float3 f1 = WaveReadLaneAt(f, ((quadPosY << 1) + 1) * g_waveXSize + lanePos.x);

	const float3 ddy = f1 - f0;

	return abs(ddy) > 0.5 ? 0.0 : ddy;
}

//--------------------------------------------------------------------------------------
// Compute direction in local space
//--------------------------------------------------------------------------------------
float3 computeLocalDirectionGGX(float a, float2 xi)
{
	const float phi = 2.0 * PI * xi.x;

	// Only near the specular direction according to the roughness for importance sampling
	const float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
	const float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

	return float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

//--------------------------------------------------------------------------------------
// Transform local to world space
//--------------------------------------------------------------------------------------
float3x3 computeLocalToWorld(float3 normal)
{
	// Using right-hand coord
	const float3 up = abs(normal.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0.xx);
	const float3 xAxis = normalize(cross(up, normal));
	const float3 yAxis = cross(normal, xAxis);
	const float3 zAxis = normal;

	return float3x3(xAxis, yAxis, zAxis);
}

// Compute local direction first and transform it to world space
float3 computeDirectionGGX(float a, float3 normal, float2 xi)
{
	const float3 localDir = computeLocalDirectionGGX(a, xi);
	const float3x3 tanSpace = computeLocalToWorld(normal);

	return tanSpace[0] * localDir.x + tanSpace[1] * localDir.y + tanSpace[2] * localDir.z;
}

//--------------------------------------------------------------------------------------
// Retrieve hit world position.
//--------------------------------------------------------------------------------------
float3 environment(float3 dir, float3 ddx = 0.0, float3 ddy = 0.0)
{
#if 1
	return ((abs(ddx) + abs(ddy) > 0.0 ? g_txEnv.SampleGrad(g_sampler, dir, ddx, ddy) :
		g_txEnv.SampleLevel(g_sampler, dir, 0.0))) * 1.5;
#else
	const float3 sunDir = normalize(float3(-1.0, 1.0, -1.0));
	const float sumAmt = saturate(dot(dir, sunDir));

	const float a = dot(dir, float3(0.0, 1.0, 0.0)) * 0.5 + 0.5;
	const float3 color = lerp(float3(0.0, 0.16, 0.64), 1.0, a);

	return color * 3.0 + (sumAmt > 0.9995 ? 7.0 : 0.0);
#endif
}

// Trace a radiance ray into the scene and returns a shaded color.
RayPayload traceRadianceRay(RayDesc ray, uint currentRayRecursionDepth, float3 ddx = 0.0, float3 ddy = 0.0)
{
	RayPayload payload;

	if (currentRayRecursionDepth >= MAX_RECURSION_DEPTH)
		payload.Color = environment(ray.Direction, ddx, ddy);// *0.5;
	else
	{
		// Set TMin to a zero value to avoid aliasing artifacts along contact areas.
		// Note: make sure to enable face culling so as to avoid surface face fighting.
		ray.TMin = 0.0;
		ray.TMax = 10000.0;
		payload.Color = 0.0.xxx;
		payload.RecursionDepth = currentRayRecursionDepth;
		TraceRay(g_scene, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, ~0, 0, 1, 0, ray, payload);
	}

	return payload;
}

//--------------------------------------------------------------------------------------
// Generate a ray in world space for a primary-surface pixel corresponding to an index
// from the dispatched 2D grid.
//--------------------------------------------------------------------------------------
uint getPrimarySurface(uint2 index, uint2 dim, out float3 N, out float3 V, out float3 pos)
{
	const float4 norm = g_normal[index];
	const float depth = g_depth[index];

	float2 screenPos = (index + 0.5) / dim * 2.0 - 1.0;
	screenPos.y = -screenPos.y; // Invert Y for Y-up-style NDC.

	if (norm.w > 0.0)
	{
		// Unproject the pixel coordinate into a ray.
		const float4 world = mul(float4(screenPos, depth, 1.0), l_cbRayGen.ProjToWorld);

		pos = world.xyz / world.w;
		N = normalize(norm.xyz - 0.5);
		V = normalize(l_cbRayGen.EyePt - pos);

		return norm.w * 2.0 - 1.0;
	}
	else
	{
		// Unproject the pixel coordinate into a ray.
		const float4 world = mul(float4(screenPos, 0.0, 1.0), l_cbRayGen.ProjToWorld);

		pos = world.xyz / world.w;
		V = normalize(l_cbRayGen.EyePt - pos);

		return 0xffffffff;
	}
}

//--------------------------------------------------------------------------------------
// Get IA-style inputs
//--------------------------------------------------------------------------------------
Vertex getInput(float2 barycentrics)
{
	const uint meshIdx = InstanceIndex();
	const uint baseIdx = PrimitiveIndex() * 3;
	const uint3 indices =
	{
		g_indexBuffers[NonUniformResourceIndex(meshIdx)][baseIdx],
		g_indexBuffers[NonUniformResourceIndex(meshIdx)][baseIdx + 1],
		g_indexBuffers[NonUniformResourceIndex(meshIdx)][baseIdx + 2]
	};

	// Retrieve corresponding vertex normals for the triangle vertices.
	Vertex vertices[3] =
	{
		g_vertexBuffers[NonUniformResourceIndex(meshIdx)][indices[0]],
		g_vertexBuffers[NonUniformResourceIndex(meshIdx)][indices[1]],
		g_vertexBuffers[NonUniformResourceIndex(meshIdx)][indices[2]]
	};

	Vertex input;
	input.Pos = vertices[0].Pos +
		barycentrics.x * (vertices[1].Pos - vertices[0].Pos) +
		barycentrics.y * (vertices[2].Pos - vertices[0].Pos);

	input.Nrm = vertices[0].Nrm +
		barycentrics.x * (vertices[1].Nrm - vertices[0].Nrm) +
		barycentrics.y * (vertices[2].Nrm - vertices[0].Nrm);

	return input;
}

//--------------------------------------------------------------------------------------
// Retrieve hit world position.
//--------------------------------------------------------------------------------------
float3 hitWorldPosition()
{
	return WorldRayOrigin() + RayTCurrent() * WorldRayDirection();
}

//--------------------------------------------------------------------------------------
// Ray miss
//--------------------------------------------------------------------------------------
[shader("miss")]
void missMain(inout RayPayload payload)
{
	payload.Color = environment(WorldRayDirection());
}
