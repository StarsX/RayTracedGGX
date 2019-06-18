//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

#include "BRDFModels.hlsli"
#include "RayTracedGGX.hlsli"

#define ROUGHNESS	0.25

//--------------------------------------------------------------------------------------
// Ray generation
//--------------------------------------------------------------------------------------
[shader("raygeneration")]
void raygenMain()
{
	// Trace the ray.
	RayDesc ray;

	// Fallback layer has no depth
	uint2 dim = DispatchRaysDimensions().xy;
	dim.y >>= 1;

	uint3 index = DispatchRaysIndex();
	index.yz = uint2(index.y >> 1, index.y & 1);

	// Generate a ray for a camera pixel corresponding to an index from the dispatched 2D grid.
	generateCameraRay(index, dim, ray.Origin, ray.Direction);

	RayPayload payload = traceRadianceRay(ray, 0);

	// Write the raytraced color to the output texture.
	const float a = payload.RecursionDepth > 0 ? 1.0 : 0.0;
	RenderTarget[index] = float4(payload.Color, a);
}

// Quasirandom low-discrepancy sequences
float2 Hammersley(uint i, uint num)
{
	uint bits = i;
	bits = (bits << 16) | (bits >> 16);
	bits = ((bits & 0x55555555) << 1) | ((bits & 0xAAAAAAAA) >> 1);
	bits = ((bits & 0x33333333) << 2) | ((bits & 0xCCCCCCCC) >> 2);
	bits = ((bits & 0x0F0F0F0F) << 4) | ((bits & 0xF0F0F0F0) >> 4);
	bits = ((bits & 0x00FF00FF) << 8) | ((bits & 0xFF00FF00) >> 8);

	return float2(i / float(num), bits / float(0xffffffff));
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

float2 GetHammersley(uint2 index)
{
	const uint n = 256;
	uint i = MortonIndex(index.xy % 16);
	//uint i = (index.y % 16) * 16 + (index.x % 16);
	i = (i + l_cbHitGroup.FrameIndex) % n;

	return Hammersley(i, n);
}

//--------------------------------------------------------------------------------------
// Ray closest hit
//--------------------------------------------------------------------------------------
[shader("closesthit")]
void closestHitMain(inout RayPayload payload, TriAttributes attr)
{
	Vertex input = getInput(attr.barycentrics);

	uint3 index = DispatchRaysIndex();
	index.yz = uint2(index.y >> 1, index.y & 1);

	// Trace a reflection ray.
	RayDesc ray;
	const bool isCentroidSample = index.z;
	const float2 xi = isCentroidSample ? 0.0 : GetHammersley(index.xy);
	const float a = ROUGHNESS * ROUGHNESS;
	const float3 N = normalize(InstanceIndex() ? mul(input.Nrm, (float3x3)l_cbHitGroup.Normal) : input.Nrm);
	const float3 H = computeDirectionGGX(a, N, xi);
	ray.Origin = hitWorldPosition();
	ray.Direction = reflect(WorldRayDirection(), H);
	float3 radiance = traceRadianceRay(ray, ++payload.RecursionDepth).Color;

	const float NoL = saturate(dot(N, ray.Direction));

	if (NoL > 0.0)
	{
		// Calculate fresnel
		const float3 specColors[] =
		{
			float3(0.95, 0.93, 0.88),	// Silver
			float3(1.00, 0.71, 0.29)	// Gold
		};
		const float3 V = -WorldRayDirection();
		const float VoH = saturate(dot(V, H));
		const float3 F = F_Schlick(specColors[InstanceIndex()], VoH);

		// Visibility factor
		const float NoV = saturate(dot(N, V));
		const float vis = Vis_Schlick(ROUGHNESS, NoV, NoL);

		// BRDF
		// Microfacet specular = D * F * G / (4 * NoL * NoV) = D * F * Vis
		const float NoH = saturate(dot(N, H));
		// pdf = D * NoH / (4 * VoH)
		//radiance *= NoL * F * vis * (4.0 * VoH / NoH);
		// pdf = D * NoH
		const float3 specular = F * NoL * vis / NoH;
		radiance *= isCentroidSample ? saturate(specular) : specular;
	}

	//const float3 color = float3(1.0 - attr.barycentrics.x - attr.barycentrics.y, attr.barycentrics.xy);

	payload.Color = radiance;
}
