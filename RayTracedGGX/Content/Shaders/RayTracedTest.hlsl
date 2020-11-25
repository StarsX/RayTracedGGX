//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "BRDFModels.hlsli"
#include "RayTracedGGX.hlsli"

#define ROUGHNESS 0.0

RayPayload computeLighting(uint instanceIdx, float3 N, float3 V, float3 pos, uint recursionDepth = 0)
{
	// Trace a reflection ray.
	const float a = ROUGHNESS * ROUGHNESS;
	const float3 H = computeDirectionGGX(a, N, 0.0);
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
		const float vis = Vis_Schlick(ROUGHNESS, NoV, NoL);

		// BRDF
		// Microfacet specular = D * F * G / (4 * NoL * NoV) = D * F * Vis
		const float NoH = saturate(dot(N, H));
		// pdf = D * NoH / (4 * VoH)
		//payload.Color *= NoL * F * vis * (4.0 * VoH / NoH);
		// pdf = D * NoH
		payload.Color *= saturate(F * NoL * vis / NoH);
		//payload.Color *= EnvBRDFApprox(specColors[InstanceIndex()], ROUGHNESS, NoV);

		//const float3 color = float3(1.0 - attr.barycentrics.x - attr.barycentrics.y, attr.barycentrics.xy);
	}
	else payload.Color = 0.0;

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
