//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "BRDFModels.hlsli"

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
		g_txEnv.SampleLevel(g_sampler, dir, 0.0)));// *1.5;
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

// Quasirandom low-discrepancy sequences
uint Hammersley(uint i)
{
	uint bits = i;
	bits = (bits << 16) | (bits >> 16);
	bits = ((bits & 0x55555555) << 1) | ((bits & 0xAAAAAAAA) >> 1);
	bits = ((bits & 0x33333333) << 2) | ((bits & 0xCCCCCCCC) >> 2);
	bits = ((bits & 0x0F0F0F0F) << 4) | ((bits & 0xF0F0F0F0) >> 4);
	bits = ((bits & 0x00FF00FF) << 8) | ((bits & 0xFF00FF00) >> 8);

	return bits;
}

float2 Hammersley(uint i, uint num)
{
	return float2(i / float(num), Hammersley(i) / float(0xffffffff));
}

// Morton order generator
uint MortonCode(uint x)
{
	x = (x ^ (x << 2)) & 0x33333333;
	x = (x ^ (x << 1)) & 0x55555555;

	return x;
}

uint MortonIndex(uint2 pos)
{
	// Interleaved combination
	return MortonCode(pos.x) | (MortonCode(pos.y) << 1);
}

uint WangHash(uint seed)
{
	seed = (seed ^ 61) ^ (seed >> 16);
	seed *= 9;
	seed = seed ^ (seed >> 4);
	seed *= 0x27d4eb2d;
	seed = seed ^ (seed >> 15);
	
	return seed;
}

float2 getHammersley(uint2 index, uint2 dim)
{
	const uint n = 256;
	const uint x = index.y * dim.x + index.x;
	const uint y = index.x * dim.y + index.y;
	uint s = MortonIndex(uint2(x, y));
	s += g_cb.FrameIndex;

	[unroll]
	for (uint i = 0; i < 1; ++i) s = WangHash(s);
	s %= n;

	return Hammersley(s, n);
}

RayPayload computeLighting(uint instanceIdx, float roughness, float3 N, float3 V, float3 pos, uint recursionDepth = 0)
{
	// Trace a reflection ray.
	const float2 xi = getHammersley(DispatchRaysIndex().xy, DispatchRaysDimensions().xy);
	const float a = roughness * roughness;
	const float3 H = computeDirectionGGX(a, N, xi);
	const RayDesc ray = { pos, 0.0, reflect(-V, H), 10000.0 };
	const float3 dLdx = dFdx(ray.Direction);
	const float3 dLdy = dFdy(ray.Direction);
	RayPayload payload = traceRadianceRay(ray, recursionDepth, dLdx, dLdy);

	const float NoL = saturate(dot(N, ray.Direction));

	if (NoL > 0.0)
	{
		// Calculate fresnel
		const float3 specColors[] =
		{
			float3(0.95, 0.93, 0.88),	// Silver
			float3(1.00, 0.71, 0.29)	// Gold
		};
		const float VoH = saturate(dot(V, H));
		const float3 F = F_Schlick(specColors[instanceIdx], VoH);

		// Visibility factor
		const float NoV = saturate(dot(N, V));
		const float vis = Vis_Schlick(roughness, NoV, NoL);

		// BRDF
		// Microfacet specular = D * F * G / (4 * NoL * NoV) = D * F * Vis
		const float NoH = saturate(dot(N, H));
		// pdf = D * NoH / (4 * VoH)
		payload.Color *= NoL * F * vis * (4.0 * VoH / NoH);
		// pdf = D * NoH
		//payload.Color *= F * NoL * vis / NoH;
	}

	return payload;
}

//--------------------------------------------------------------------------------------
// Ray generation
//--------------------------------------------------------------------------------------
[shader("raygeneration")]
void raygenMain()
{
	const uint2 dim = DispatchRaysDimensions().xy;
	const uint2 index = DispatchRaysIndex().xy;

	// Generate a ray corresponding to an index from a primary surface.
	float3 N, V, pos;
	const uint instanceIdx = getPrimarySurface(index, dim, N, V, pos);

	RayPayload payload;
	if (instanceIdx != 0xffffffff)
	{
		const float roughness = g_roughness[index];
		const RayPayload payload = computeLighting(instanceIdx, roughness, N, V, pos);
		RenderTarget[index] = float4(payload.Color, 1.0); // Write the raytraced color to the output texture.
	}
	else RenderTarget[index] = float4(environment(-V), 0.0);
}

//--------------------------------------------------------------------------------------
// Ray closest hit
//--------------------------------------------------------------------------------------
[shader("closesthit")]
void closestHitMain(inout RayPayload payload, TriAttributes attr)
{
	Vertex input = getInput(attr.barycentrics);

	// Trace a reflection ray.
	const uint instanceIdx = InstanceIndex();
	const float3 N = normalize(instanceIdx ? mul(input.Nrm, (float3x3)g_cb.Normal) : input.Nrm);
	payload = computeLighting(instanceIdx, 0.0, N, -WorldRayDirection(), hitWorldPosition(), 1);
}

//--------------------------------------------------------------------------------------
// Ray miss
//--------------------------------------------------------------------------------------
[shader("miss")]
void missMain(inout RayPayload payload)
{
	payload.Color = environment(WorldRayDirection());
}