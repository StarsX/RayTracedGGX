//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "BRDFModels.hlsli"
#include "RayTracedGGX.hlsli"

static const float g_roughnesses[] = { 0.4, 0.2 };

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

uint Hash(uint seed)
{
	seed = (seed ^ 61) ^ (seed >> 16);
	seed *= 9;
	seed = seed ^ (seed >> 4);
	seed *= 0x27d4eb2d;
	seed = seed ^ (seed >> 15);
	
	return seed;
}

float2 GetHammersley(uint2 index, uint2 dim)
{
	const uint n = 256;
	const uint x = index.y * dim.x + index.x;
	const uint y = index.x * dim.y + index.y;
	uint s = MortonIndex(uint2(x, y));
	s += g_cb.FrameIndex;

	[unroll]
	for (uint i = 0; i < 1; ++i) s = Hash(s);
	s %= n;

	return Hammersley(s, n);
}

RayPayload computeLighting(uint instanceIdx, float3 N, float3 V, float3 pos, uint recursionDepth = 0)
{
	// Trace a reflection ray.
	const float2 xi = GetHammersley(DispatchRaysIndex().xy, DispatchRaysDimensions().xy);
	const float roughness = g_roughnesses[instanceIdx];
	const float a = roughness * roughness;
	const float3 H = computeDirectionGGX(a, N, xi);
	const RayDesc ray = { pos, 0.0, reflect(-V, H), 10000.0 };
	RayPayload payload = traceRadianceRay(ray, recursionDepth);

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
		//payload.Color *= NoL * F * vis * (4.0 * VoH / NoH);
		// pdf = D * NoH
		payload.Color *= F * NoL * vis / NoH;
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
		const RayPayload payload = computeLighting(instanceIdx, N, V, pos);
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
	const float3 N = normalize(InstanceIndex() ? mul(input.Nrm, (float3x3)g_cb.Normal) : input.Nrm);
	payload = computeLighting(InstanceIndex(), N, -WorldRayDirection(), hitWorldPosition(), 1);
}
