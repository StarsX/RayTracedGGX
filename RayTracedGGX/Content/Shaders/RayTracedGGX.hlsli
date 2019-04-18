//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

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
	float2	Jitter;
};

//--------------------------------------------------------------------------------------
// Constant buffers
//--------------------------------------------------------------------------------------
ConstantBuffer<RayGenConstants> l_cbRayGen : register(b0);

//--------------------------------------------------------------------------------------
// Texture and buffers
//--------------------------------------------------------------------------------------
RWTexture2DArray<float4>	RenderTarget	: register(u0);
RaytracingAS				g_scene : register(t0);

// IA buffers
Buffer<uint>				g_indexBuffers[]	: register(t0, space1);
StructuredBuffer<Vertex>	g_vertexBuffers[]	: register(t0, space2);

//--------------------------------------------------------------------------------------
// Samplers
//--------------------------------------------------------------------------------------
SamplerState g_sampler;

//--------------------------------------------------------------------------------------
// Structs
//--------------------------------------------------------------------------------------
struct HitGroupConstants
{
	matrix Normal;
	float2 Hammersley;
};

//--------------------------------------------------------------------------------------
// Constant buffers
//--------------------------------------------------------------------------------------
ConstantBuffer<HitGroupConstants> l_cbHitGroup : register(b1);

//--------------------------------------------------------------------------------------
// Compute direction in local space
//--------------------------------------------------------------------------------------
float3 computeLocalDirectionGGX(float a, bool isCentroidSample)
{
	const float2 xi = isCentroidSample ? 0.0 : l_cbHitGroup.Hammersley;
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
float3 computeDirectionGGX(float a, float3 normal, bool isCentroidSample = false)
{
	const float3 localDir = computeLocalDirectionGGX(a, isCentroidSample);
	const float3x3 tanSpace = computeLocalToWorld(normal);

	return tanSpace[0] * localDir.x + tanSpace[1] * localDir.y + tanSpace[2] * localDir.z;
}

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
		payload.RecursionDepth = currentRayRecursionDepth;
		TraceRay(g_scene, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, ~0, 0, 1, 0, ray, payload);
	}

	return payload;
}

//--------------------------------------------------------------------------------------
// Generate a ray in world space for a camera pixel corresponding to an index
// from the dispatched 2D grid.
//--------------------------------------------------------------------------------------
void generateCameraRay(uint3 index, out float3 origin, out float3 direction)
{
	// Fallback layer has no depth
	uint2 dim = DispatchRaysDimensions().xy;
	dim.y >>= 1;

	const float2 xy = index.xy + (index.z ? 0.5 : l_cbRayGen.Jitter); // jitter from the middle of the pixel.
	float2 screenPos = xy / dim * 2.0 - 1.0;

	// Invert Y for Y-up-style NDC.
	screenPos.y = -screenPos.y;

	// Unproject the pixel coordinate into a ray.
	float4 world = mul(float4(screenPos, 0.0, 1.0), l_cbRayGen.ProjToWorld);
	world.xyz /= world.w;

	origin = l_cbRayGen.EyePt;
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

	// Fallback layer has no depth
	uint3 index = DispatchRaysIndex();
	index.yz = uint2(index.y >> 1, index.y & 1);

	// Generate a ray for a camera pixel corresponding to an index from the dispatched 2D grid.
	generateCameraRay(index, ray.Origin, ray.Direction);

	RayPayload payload = traceRadianceRay(ray, 0);

	// Write the raytraced color to the output texture.
	const float a = payload.RecursionDepth > 0 ? 1.0 : 0.0;
	RenderTarget[index] = float4(payload.Color, a);
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
