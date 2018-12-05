//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

#include "BRDFModels.hlsli"

#define NUM_TEXTURES		8
#define MAX_RECURSION_DEPTH	3

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
	float4	Color;
	uint	RecursionDepth;
};

struct RayGenConstants
{
	matrix	ProjToWorld;
	float3	EyePt;
};

//--------------------------------------------------------------------------------------
// Constant buffers
//--------------------------------------------------------------------------------------
ConstantBuffer<RayGenConstants> l_rayGenCB : register(b0);

//--------------------------------------------------------------------------------------
// Texture and buffers
//--------------------------------------------------------------------------------------
RWTexture2D<float4> RenderTarget	: register(u0);
RaytracingAS		g_scene			: register(t0);

// IA buffers
Buffer<uint>				g_indexBuffers[]	: register(t0, space1);
StructuredBuffer<Vertex>	g_vertexBuffers[]	: register(t0, space2);

//Texture2D g_textures[] : register(t0, space3);

//--------------------------------------------------------------------------------------
// Samplers
//--------------------------------------------------------------------------------------
SamplerState g_sampler;

//--------------------------------------------------------------------------------------
// Retrieve hit world position.
//--------------------------------------------------------------------------------------
float4 environment(float3 dir)
{
	const float a = dot(dir, float3(0.0, 1.0, 0.0)) * 0.5 + 0.5;

	return lerp(float4(0.0, 0.16, 0.64, 0.0), 1.0, a);
}

// Trace a radiance ray into the scene and returns a shaded color.
float4 traceRadianceRay(RayDesc ray, uint currentRayRecursionDepth)
{
	RayPayload payload;

	if (currentRayRecursionDepth >= MAX_RECURSION_DEPTH)
		payload.Color = environment(ray.Direction) * 0.5;
	else
	{
		// Set TMin to a zero value to avoid aliasing artifacts along contact areas.
		// Note: make sure to enable face culling so as to avoid surface face fighting.
		ray.TMin = 0.0;
		ray.TMax = 10000.0;
		payload.Color = 0.0.xxxx;
		payload.RecursionDepth = currentRayRecursionDepth + 1;
		TraceRay(g_scene, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, ~0, 0, 1, 0, ray, payload);
	}

	return payload.Color;
}

//--------------------------------------------------------------------------------------
// Generate a ray in world space for a camera pixel corresponding to an index
// from the dispatched 2D grid.
//--------------------------------------------------------------------------------------
void generateCameraRay(uint2 index, out float3 origin, out float3 direction)
{
	const float2 xy = index + 0.5f; // center in the middle of the pixel.
	float2 screenPos = xy / DispatchRaysDimensions().xy * 2.0 - 1.0;

	// Invert Y for Y-up-style NDC.
	screenPos.y = -screenPos.y;

	// Unproject the pixel coordinate into a ray.
	float4 world = mul(float4(screenPos, 0.0, 1.0), l_rayGenCB.ProjToWorld);
	world.xyz /= world.w;

	origin = l_rayGenCB.EyePt;
	direction = normalize(world.xyz - origin);
}

//--------------------------------------------------------------------------------------
// Ray generation
//--------------------------------------------------------------------------------------
[shader("raygeneration")]
void raygenMain()
{
	// Trace the ray.
	RayDesc ray;

	// Generate a ray for a camera pixel corresponding to an index from the dispatched 2D grid.
	generateCameraRay(DispatchRaysIndex().xy, ray.Origin, ray.Direction);

	float4 color = traceRadianceRay(ray, 0);
	//color /= (color + 1.0);

	// Write the raytraced color to the output texture.
	RenderTarget[DispatchRaysIndex().xy] = sqrt(color);
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
		g_indexBuffers[meshIdx][baseIdx],
		g_indexBuffers[meshIdx][baseIdx + 1],
		g_indexBuffers[meshIdx][baseIdx + 2]
	};

	// Retrieve corresponding vertex normals for the triangle vertices.
	Vertex vertices[3] =
	{
		g_vertexBuffers[meshIdx][indices[0]],
		g_vertexBuffers[meshIdx][indices[1]],
		g_vertexBuffers[meshIdx][indices[2]]
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
// Ray closest hit
//--------------------------------------------------------------------------------------
[shader("closesthit")]
void closestHitMain(inout RayPayload payload, TriAttributes attr)
{
	Vertex input = getInput(attr.barycentrics);

	// Trace a reflection ray.
	RayDesc ray;
	const float3 nrm = normalize(input.Nrm);
	ray.Origin = hitWorldPosition();
	ray.Direction = reflect(WorldRayDirection(), nrm);
	float4 radiance = traceRadianceRay(ray, payload.RecursionDepth);

	const float3 halfAngle = normalize(ray.Direction - WorldRayDirection());
	const float NoV = max(dot(nrm, -WorldRayDirection()), 0.00001);
	const float NoL = max(dot(nrm, ray.Direction), 0.0000);
	const float NoH = max(dot(nrm, halfAngle), 0.00001);
	const float VoH = saturate(dot(-WorldRayDirection(), halfAngle));

	const float3 spec = float3(0.8, 0.8, 0.8);
	radiance.xyz *= saturate(F_Schlick(spec, VoH) * Vis_Smith(0.0, NoV, NoL) / NoH);
	
	const float3 color = saturate(dot(nrm, float3(0.0, 1.0, 0.0)) * 0.5 + 0.5);
	//const float3 color = float3(1.0 - attr.barycentrics.x - attr.barycentrics.y, attr.barycentrics.xy);
	
	payload.Color = radiance;//lerp(float4(color, 1.0), radiance, 0.25);
}

//--------------------------------------------------------------------------------------
// Ray miss
//--------------------------------------------------------------------------------------
[shader("miss")]
void missMain(inout RayPayload payload)
{
	payload.Color = environment(WorldRayDirection());
}
