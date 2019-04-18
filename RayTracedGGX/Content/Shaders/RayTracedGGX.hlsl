//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

#include "BRDFModels.hlsli"
#include "RayTracedGGX.hlsli"

#define ROUGHNESS	0.25

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
