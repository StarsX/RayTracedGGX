//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "BRDFModels.hlsli"
#include "Material.hlsli"

#define PRIMITIVE_BITS 24
#define MAX_RECURSION_DEPTH	1

typedef RaytracingAccelerationStructure RaytracingAS;
typedef BuiltInTriangleIntersectionAttributes TriAttributes;

enum HitGroup
{
	HIT_GROUP_REFLECTION,
	HIT_GROUP_DIFFUSE,

	NUM_HIT_GROUP
};

//--------------------------------------------------------------------------------------
// Structs
//--------------------------------------------------------------------------------------
struct Vertex
{
	float3	Pos;
	float3	Nrm;
};

struct Attrib
{
	float3	Pos;
	float3	Nrm;
	float2	UV;
};

struct RayPayload
{
	float3	Color;
	uint	RecursionDepth;
};

struct GlobalConstants
{
	float4x4 WorldViewProjs[NUM_MESH];
	float4x4 WorldViewProjsPrev[NUM_MESH];
	float4x3 Worlds[NUM_MESH];
	float3x3 WorldITs[NUM_MESH];
	uint FrameIndex;
};

struct RayGenConstants
{
	matrix	ProjToWorld;
	float3	EyePt;
	float2	ProjBias;
};

//--------------------------------------------------------------------------------------
// Constant buffers
//--------------------------------------------------------------------------------------
ConstantBuffer<GlobalConstants> g_cb		: register (b1);
ConstantBuffer<RayGenConstants> l_rayGen	: register (b2);

//--------------------------------------------------------------------------------------
// Texture and buffers
//--------------------------------------------------------------------------------------
RWTexture2D<float3>			g_rwRenderTargets[NUM_HIT_GROUP] : register (u0);
RWTexture2D<float4>			g_rwNormal		: register (u2);
RWTexture2D<float2>			g_rwRoughMetal	: register (u3);
RWTexture2D<float2>			g_rwVelocity	: register (u4);
RaytracingAS				g_scene			: register (t0);
Texture2D<uint>				g_txVisiblity	: register (t1);
TextureCube<float3>			g_txEnv			: register (t2);

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

	return f1 - f0;
}

float3 dFdy(float3 f)
{
	const uint laneIdx = WaveGetLaneIndex();
	const uint2 lanePos = { laneIdx % g_waveXSize, laneIdx / g_waveXSize };
	const uint quadPosY = lanePos.y >> 1;
	const float3 f0 = WaveReadLaneAt(f, (quadPosY << 1) * g_waveXSize + lanePos.x);
	const float3 f1 = WaveReadLaneAt(f, ((quadPosY << 1) + 1) * g_waveXSize + lanePos.x);

	return f1 - f0;
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

float3 computeLocalDirectionCos(float2 xi)
{
	const float phi = 2.0 * PI * xi.x;

	// Only near the specular direction according to the roughness for importance sampling
	const float cosTheta = sqrt(xi.y);
	//const float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
	const float sinTheta = sqrt(1.0 - xi.y);

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

// Compute local direction first and transform it to world space
float3 computeDirectionCos(float3 normal, float2 xi)
{
	//const float3 localDir = float3(0.0.xx, 1.0);
#if 1
	const float3 localDir = computeLocalDirectionCos(xi);

	return normalize(normal + localDir);
#else
	const float3 localDir = computeLocalDirectionCos(xi);
	const float3x3 tanSpace = computeLocalToWorld(normal);

	return tanSpace[0] * localDir.x + tanSpace[1] * localDir.y + tanSpace[2] * localDir.z;
#endif
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
RayPayload traceRadianceRay(RayDesc ray, uint currentRayRecursionDepth, uint hitGroup, float3 ddx = 0.0, float3 ddy = 0.0)
{
	RayPayload payload;

	if (currentRayRecursionDepth >= MAX_RECURSION_DEPTH)
		payload.Color = environment(ray.Direction, ddx, ddy);
	else
	{
		payload.Color = 0.0;
		payload.RecursionDepth = currentRayRecursionDepth;
		TraceRay(g_scene, RAY_FLAG_NONE, ~0, 0, 1, 0, ray, payload);
	}

	return payload;
}

//--------------------------------------------------------------------------------------
// Calculate barycentrics
// http://filmicworlds.com/blog/visibility-buffer-rendering-with-material-graphs/
//--------------------------------------------------------------------------------------
float2 calcBarycentrics(float4 p[3], float2 ndc)
{
	const float3 invW = 1.0 / float3(p[0].w, p[1].w, p[2].w);

	const float2 ndc0 = p[0].xy * invW.x;
	const float2 ndc1 = p[1].xy * invW.y;
	const float2 ndc2 = p[2].xy * invW.z;

	const float invDet = 1.0 / determinant(float2x2(ndc2 - ndc1, ndc0 - ndc1));
	const float3 dPdx = float3(ndc1.y - ndc2.y, ndc2.y - ndc0.y, ndc0.y - ndc1.y) * invDet;
	const float3 dPdy = float3(ndc2.x - ndc1.x, ndc0.x - ndc2.x, ndc1.x - ndc0.x) * invDet;

	const float2 deltaVec = ndc - ndc0;
	const float interpInvW = (invW.x + deltaVec.x * dot(invW, dPdx) + deltaVec.y * dot(invW, dPdy));
	const float interpW = 1.0 / interpInvW;

	float2 barycentrics;
	barycentrics.x = interpW * (deltaVec.x * dPdx.y * invW.y + deltaVec.y * dPdy.y * invW.y);
	barycentrics.y = interpW * (deltaVec.x * dPdx.z * invW.z + deltaVec.y * dPdy.z * invW.z);

	return barycentrics;
}

//--------------------------------------------------------------------------------------
// Get triangle vertices
//--------------------------------------------------------------------------------------
void getVertices(out Vertex vertices[3], uint meshIdx, uint primIdx)
{
	const uint baseIdx = primIdx * 3;
	const uint3 indices =
	{
		g_indexBuffers[NonUniformResourceIndex(meshIdx)][baseIdx],
		g_indexBuffers[NonUniformResourceIndex(meshIdx)][baseIdx + 1],
		g_indexBuffers[NonUniformResourceIndex(meshIdx)][baseIdx + 2]
	};

	// Retrieve corresponding vertex normals for the triangle vertices.
	[unroll]
	for (uint i = 0; i < 3; ++i)
		vertices[i] = g_vertexBuffers[NonUniformResourceIndex(meshIdx)][indices[i]];
}

//--------------------------------------------------------------------------------------
// Interpolate attributes
//--------------------------------------------------------------------------------------
Attrib interpAttrib(Vertex vertices[3], float2 barycentrics)
{
	const float3 baryWeights = float3(1.0 - (barycentrics.x + barycentrics.y), barycentrics.xy);

	Attrib attrib;
	attrib.Pos =
		baryWeights[0] * vertices[0].Pos +
		baryWeights[1] * vertices[1].Pos +
		baryWeights[2] * vertices[2].Pos;

	attrib.Nrm =
		baryWeights[0] * vertices[0].Nrm +
		baryWeights[1] * vertices[1].Nrm +
		baryWeights[2] * vertices[2].Nrm;

	attrib.UV = getUV(attrib.Nrm, attrib.Pos, float3(1.0, 0.2, 1.0));
	/*attrib.UV =
		baryWeights[0] * vertices[0].UV +
		baryWeights[1] * vertices[1].UV +
		baryWeights[2] * vertices[2].UV;*/

	return attrib;
}

//--------------------------------------------------------------------------------------
// Generate a ray in world space for a primary-surface pixel corresponding to an index
// from the dispatched 2D grid.
//--------------------------------------------------------------------------------------
bool getPrimarySurface(uint2 index, uint2 dim, out float3 N, out float3 V, out float3 P,
	out float4 color, out float2 rghMtl, out float2 velocity)
{
	uint visibility = g_txVisiblity[index];
	float2 screenPos = (index + 0.5) / dim * 2.0 - 1.0;
	screenPos.y = -screenPos.y; // Invert Y for Y-up-style NDC.

	if (visibility > 0)
	{
		// Decode visibility
		--visibility;
		const uint instanceIdx = visibility >> PRIMITIVE_BITS;
		const uint primitiveIdx = (visibility & ((1 << PRIMITIVE_BITS) - 1));

		// Fetch vertices
		Vertex vertices[3];
		getVertices(vertices, instanceIdx, primitiveIdx);

		// Calculate barycentrics
		float4 p[3];
		[unroll]
		for (uint i = 0; i < 3; ++i)
			p[i] = mul(float4(vertices[i].Pos, 1.0), g_cb.WorldViewProjs[instanceIdx]);
		screenPos -= l_rayGen.ProjBias;
		const float2 barycentrics = calcBarycentrics(p, screenPos);

		// Interpolate triangle sample attributes
		Attrib attrib = interpAttrib(vertices, barycentrics);
		color = getBaseColor(instanceIdx, attrib.UV);
		rghMtl = getRoughMetal(instanceIdx, attrib.UV);

		// Calculate velocity (motion vector)
		const float4 pos = float4(attrib.Pos, 1.0);
		const float4 hPosPrev = mul(pos, g_cb.WorldViewProjsPrev[instanceIdx]);
		velocity = (screenPos - hPosPrev.xy / hPosPrev.w) * float2(0.5, -0.5);

		P = mul(pos, g_cb.Worlds[instanceIdx]);
		N = normalize(mul(attrib.Nrm, g_cb.WorldITs[instanceIdx]));
		V = normalize(l_rayGen.EyePt - P);

		return true;
	}
	else
	{
		// Unproject the pixel coordinate into a ray.
		const float4 world = mul(float4(screenPos, 0.0, 1.0), l_rayGen.ProjToWorld);
		velocity = 0.0;

		P = world.xyz / world.w;
		N = 0.0;
		V = normalize(l_rayGen.EyePt - P);

		return false;
	}
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
	return float2(i / float(num), Hammersley(i) / float(0x10000));
}

// Morton order generator
uint MortonCode(uint x)
{
	//x &= 0x0000ffff;
	x = (x ^ (x << 8)) & 0x00ff00ff;
	x = (x ^ (x << 4)) & 0x0f0f0f0f;
	x = (x ^ (x << 2)) & 0x33333333;
	x = (x ^ (x << 1)) & 0x55555555;

	return x;
}

uint MortonIndex(uint2 pos)
{
	// Interleaved combination
	return MortonCode(pos.x) | (MortonCode(pos.y) << 1);
}

uint RNG(uint seed)
{
	// Condensed version of pcg_output_rxs_m_xs_32_32
	seed = seed * 747796405 + 1;
	seed = ((seed >> ((seed >> 28) + 4)) ^ seed) * 277803737;
	seed = (seed >> 22) ^ seed;

	return seed;
}

float2 RNG(uint i, uint num)
{
	return float2(i / float(num), (RNG(i) & 0xffff) / float(0x10000));
}

float2 getSampleParam(uint2 index, uint2 dim, uint numSamples = 256)
{
	uint s = index.y * dim.x + index.x;
	//uint s = MortonIndex(index);

	s = RNG(s);
	s += g_cb.FrameIndex;
	s = RNG(s);
	s %= numSamples;

	return RNG(s, numSamples);
	//return Hammersley(s, numSamples);
}

RayPayload computeLighting(bool hit, float2 rghMtl, float3 N, float3 V, float3 P, float4 color, uint hitGroup, float t = 0.0, uint recursionDepth = 0)
{
	RayDesc ray;
	ray.Origin = P;
	ray.TMin = ray.TMax = 0.0;

	float3 H;
	float NoL;

	if (hit)
	{
		const float2 xi = getSampleParam(DispatchRaysIndex().xy, DispatchRaysDimensions().xy);

		if (hitGroup == HIT_GROUP_REFLECTION)
		{
			// Trace a reflection ray.
			const float a = rghMtl.x * rghMtl.x;
			H = recursionDepth < MAX_RECURSION_DEPTH ? computeDirectionGGX(a, N, xi) : N;

			ray.Direction = reflect(-V, H);
			NoL = dot(N, ray.Direction);
			if (NoL > 0.0)
			{
				// Set TMin to an offset to avoid aliasing artifacts along contact areas.
				ray.TMin = 1e-5;
				ray.TMax = 10000.0;
			}
		}
		else
		{
			// Trace a diffuse ray.
			ray.Direction = computeDirectionCos(N, xi);
			ray.TMin = 1e-5;
			ray.TMax = 10000.0;
			NoL = rghMtl.y < 1.0 ? 1.0 : 0.0;
		}
	}
	else ray.Direction = -V;

	const float3 dLdx = dFdx(ray.Direction);
	const float3 dLdy = dFdy(ray.Direction);
	RayPayload payload = traceRadianceRay(ray, recursionDepth, hitGroup, dLdx, dLdy);

	if (!hit && hitGroup == HIT_GROUP_REFLECTION) return payload;
	else if (NoL <= 0.0 || (!hit && hitGroup == HIT_GROUP_DIFFUSE)) return (RayPayload)0;

	if (hitGroup == HIT_GROUP_REFLECTION)
	{
		// Calculate fresnel
		const float3 f0 = lerp(0.04, color.xyz, rghMtl.y);
		const float VoH = saturate(dot(V, H));
		const float3 F = F_Schlick(f0, VoH);

		// Visibility factor
		const float NoV = saturate(dot(N, V));
		const float vis = Vis_Smith(rghMtl.x, NoV, NoL);

		// BRDF
		// Microfacet specular = D * F * G / (4 * NoL * NoV) = D * F * Vis
		//const float NoH = saturate(dot(N, H));
		const float NoH = saturate(dot(N, H));
		// pdf = D * NoH / (4 * VoH)
		payload.Color *= NoL * F * vis * (4.0 * VoH / NoH);
		// pdf = D * NoH
		//payload.Color *= F * NoL * vis / NoH;
	}
	else
	{
		// BRDF
		const float3 albedo = color.xyz;
		//const float NoL = dot(N, ray.Direction);
		//const float3 brdf = albedo / PI;
		//const float pdf = NoL / PI;
		//payload.Color *= brdf * NoL / pdf;
		payload.Color *= albedo * (1.0 - 0.04);
	}

	payload.Color *= recursionDepth > 0 ? exp(-0.15 * t) : 1.0;

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
	float3 N, V, P;
	float4 color;
	float2 rghMtl, velocity;
	const bool hit = getPrimarySurface(index, dim, N, V, P, color, rghMtl, velocity);

	g_rwNormal[index] = float4(N * 0.5 + 0.5, hit ? 1.0 : 0.0);
	if (hit) g_rwRoughMetal[index] = rghMtl;
	g_rwVelocity[index] = velocity;

	RayPayload payload = computeLighting(hit, rghMtl, N, V, P, color, HIT_GROUP_REFLECTION);
	g_rwRenderTargets[HIT_GROUP_REFLECTION][index] = payload.Color;		// Write the raytraced color to the output texture.

	if (rghMtl.y < 1.0)
	{
		payload = computeLighting(hit, rghMtl, N, V, P, color, HIT_GROUP_DIFFUSE);
		g_rwRenderTargets[HIT_GROUP_DIFFUSE][index] = payload.Color;	// Write the raytraced color to the output texture.
	}
}

//--------------------------------------------------------------------------------------
// Ray closest hits
//--------------------------------------------------------------------------------------
[shader("closesthit")]
void closestHitReflection(inout RayPayload payload, TriAttributes attr)
{
	Vertex vertices[3];
	getVertices(vertices, InstanceIndex(), PrimitiveIndex());
	Attrib attrib = interpAttrib(vertices, attr.barycentrics);

	const uint instanceIdx = InstanceIndex();
	const float3 N = normalize(mul(attrib.Nrm, g_cb.WorldITs[instanceIdx]));
	const float3 V = -WorldRayDirection();
	const float3 P = hitWorldPosition();

	const float2 rghMtl = getRoughMetal(instanceIdx, attrib.UV);
	const uint hitGroup = rghMtl.y > 0.5 ? HIT_GROUP_REFLECTION : HIT_GROUP_DIFFUSE;
	const float4 color = getBaseColor(instanceIdx, attrib.UV);

	// Trace a reflection ray.
	payload = computeLighting(true, rghMtl, N, V, P, color, hitGroup, RayTCurrent(), 1);
}

[shader("closesthit")]
void closestHitDiffuse(inout RayPayload payload, TriAttributes attr)
{
	Vertex vertices[3];
	getVertices(vertices, InstanceIndex(), PrimitiveIndex());
	Attrib attrib = interpAttrib(vertices, attr.barycentrics);

	const uint instanceIdx = InstanceIndex();
	const float3 N = normalize(mul(attrib.Nrm, g_cb.WorldITs[instanceIdx]));
	const float3 V = -WorldRayDirection();
	const float3 P = hitWorldPosition();

	const float2 rghMtl = getRoughMetal(instanceIdx, attrib.UV);
	const uint hitGroup = rghMtl.y > 0.5 ? HIT_GROUP_REFLECTION : HIT_GROUP_DIFFUSE;
	float4 color = getBaseColor(instanceIdx, attrib.UV);
	color.xyz *= hitGroup ? 1.0 - rghMtl.y : 1.0;

	// Trace a diffuse ray.
	payload = computeLighting(true, rghMtl, N, V, P, color, hitGroup, RayTCurrent(), 1);
}

//--------------------------------------------------------------------------------------
// Ray miss
//--------------------------------------------------------------------------------------
[shader("miss")]
void missMain(inout RayPayload payload)
{
	payload.Color = environment(WorldRayDirection());
	//payload.Color = environment(WorldRayDirection() + WorldRayOrigin() * 0.01);
}
