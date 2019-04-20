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

//--------------------------------------------------------------------------------------
// Ray closest hit
//--------------------------------------------------------------------------------------
[shader("closesthit")]
void closestHitMain(inout RayPayload payload, TriAttributes attr)
{
	Vertex input = getInput(attr.barycentrics);

	// Trace a reflection ray.
	RayDesc ray;
	const bool isCentroidSample = DispatchRaysIndex().y & 1;
	const float a = ROUGHNESS * ROUGHNESS;
	const float3 N = normalize(InstanceIndex() ? mul(input.Nrm, (float3x3)l_cbHitGroup.Normal) : input.Nrm);
	const float3 H = computeDirectionGGX(a, N, isCentroidSample);
	ray.Origin = hitWorldPosition();
	ray.Direction = reflect(WorldRayDirection(), H);
	float3 radiance = traceRadianceRay(ray, ++payload.RecursionDepth).Color;

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
	const float NoL = saturate(dot(N, ray.Direction));
	const float vis = Vis_Schlick(ROUGHNESS, NoV, NoL);

	// BRDF
	// Microfacet specular = D * F * G / (4 * NoL * NoV) = D * F * Vis
	const float NoH = saturate(dot(N, H));
	// pdf = D * NoH / (4 * VoH)
	//radiance *= NoL * F * vis * (4.0 * VoH / NoH);
	// pdf = D * NoH
	const float lightAmt = max(NoL, 1e-3) * vis / NoH;
	radiance *= F * (isCentroidSample ? min(lightAmt, 1.0) : lightAmt);

	//const float3 color = float3(1.0 - attr.barycentrics.x - attr.barycentrics.y, attr.barycentrics.xy);

	payload.Color = radiance;
}
