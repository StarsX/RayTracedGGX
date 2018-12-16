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
	float3	Color;
	uint	RecursionDepth;
};

struct RayGenConstants
{
	matrix	ProjToWorld;
	float3	EyePt;
};

struct SceneConstants
{
	float3x3 Normal;
};

//--------------------------------------------------------------------------------------
// Constant buffers
//--------------------------------------------------------------------------------------
ConstantBuffer<RayGenConstants>	l_rayGenCB	: register(b0);
ConstantBuffer<SceneConstants>	g_sceneCB	: register(b1);

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
float3 environment(float3 dir)
{
	const float a = dot(dir, float3(0.0, 1.0, 0.0)) * 0.5 + 0.5;

	return lerp(float3(0.0, 0.16, 0.64), 1.0, a);
}

// Trace a radiance ray into the scene and returns a shaded color.
RayPayload traceRadianceRay(RayDesc ray, uint currentRayRecursionDepth)
{
	RayPayload payload;

	if (currentRayRecursionDepth >= MAX_RECURSION_DEPTH)
		payload.Color = environment(ray.Direction);// *0.5;
	else
	{
		// Set TMin to a zero value to avoid aliasing artifacts along contact areas.
		// Note: make sure to enable face culling so as to avoid surface face fighting.
		ray.TMin = 0.0;
		ray.TMax = 10000.0;
		payload.Color = 0.0.xxx;
		payload.RecursionDepth = currentRayRecursionDepth + 1;
		TraceRay(g_scene, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, ~0, 0, 1, 0, ray, payload);
	}

	return payload;
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

	RayPayload payload = traceRadianceRay(ray, 0);
	float3 color = sqrt(payload.Color);

	// Write the raytraced color to the output texture.
	const float a = payload.RecursionDepth > 1 ? 1.0 : 0.0;
	RenderTarget[DispatchRaysIndex().xy] = float4(color, a);
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
	const float3 N = normalize(InstanceIndex() ? input.Nrm : mul(input.Nrm, g_sceneCB.Normal));
	ray.Origin = hitWorldPosition();
	ray.Direction = reflect(WorldRayDirection(), N);
	float3 radiance = traceRadianceRay(ray, payload.RecursionDepth).Color;

	const float3 V = -WorldRayDirection();
	const float3 H = normalize(V + ray.Direction);
	const float NoV = saturate(dot(N, V));
	//const float NoL = saturate(dot(N, ray.Direction));
	//const float NoH = saturate(dot(N, H));
	const float NoH = 1.0;
	const float VoH = saturate(dot(V, H));

	const float3 specColors[] =
	{
		float3(1.00, 0.71, 0.29),
		float3(0.95, 0.93, 0.88)
	};
	const float3 F = F_Schlick(specColors[InstanceIndex()], VoH);
	//const float Vis = Vis_Schlick(0.0, NoV, NoL);
	radiance *= F * saturate(0.25 / (NoV * NoH));

	//const float3 color = float3(1.0 - attr.barycentrics.x - attr.barycentrics.y, attr.barycentrics.xy);
	
	payload.Color = radiance;
}

//--------------------------------------------------------------------------------------
// Ray miss
//--------------------------------------------------------------------------------------
[shader("miss")]
void missMain(inout RayPayload payload)
{
	payload.Color = environment(WorldRayDirection());
}
