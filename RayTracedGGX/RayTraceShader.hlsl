//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

#define NUM_TEXTURES	8

struct Vertex
{
	float3 Pos;
	float2 Tex;
};

struct RayGenConstantBuffer
{
	float4 viewport;	// (left, top, right, bottom);
	float4 offset;
};

RaytracingAccelerationStructure Scene : register(t0);
RWTexture2D<float4> RenderTarget : register(u0);
ConstantBuffer<RayGenConstantBuffer> l_rayGenCB : register(b0);

Buffer<uint> g_indexBuffer : register(t1);
StructuredBuffer<Vertex> g_vertexBuffer : register(t2);

Texture2D g_textures[NUM_TEXTURES] : register(t0, space1);
SamplerState g_sampler : register(s0);

typedef BuiltInTriangleIntersectionAttributes MyAttributes;
struct RayPayload
{
	float4 color;
};

[shader("raygeneration")]
void MyRaygenShader()
{
	float2 lerpValues = DispatchRaysIndex().xy / (float2)DispatchRaysDimensions();

	// Orthographic projection since we're raytracing in screen space.
	float3 rayDir = float3(0.0.xx, 1.0);
	float3 origin = float3(lerp(l_rayGenCB.viewport.xy, l_rayGenCB.viewport.zw, lerpValues), -1.0f);
	origin -= l_rayGenCB.offset.xyz;

	// Trace the ray.
	// Set the ray's extents.
	RayDesc ray;
	ray.Origin = origin;
	ray.Direction = rayDir;
	// Set TMin to a non-zero small value to avoid aliasing issues due to floating - point errors.
	// TMin should be kept small to prevent missing geometry at close contact areas.
	ray.TMin = 0.001;
	ray.TMax = 10000.0;
	RayPayload payload = { 0.0.xxxx };
	TraceRay(Scene, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, ~0, 0, 1, 0, ray, payload);

	// Write the raytraced color to the output texture.
	RenderTarget[DispatchRaysIndex().xy] = payload.color;
}

Vertex GetInput(float2 barycentrics)
{
	const uint baseIdx = PrimitiveIndex() * 3;
	const uint3 indices =
	{
		g_indexBuffer[baseIdx],
		g_indexBuffer[baseIdx + 1],
		g_indexBuffer[baseIdx + 2]
	};

	// Retrieve corresponding vertex normals for the triangle vertices.
	Vertex vertices[3] =
	{
		g_vertexBuffer[indices[0]],
		g_vertexBuffer[indices[1]],
		g_vertexBuffer[indices[2]]
	};

	Vertex input;
	input.Pos = vertices[0].Pos +
		barycentrics.x * (vertices[1].Pos - vertices[0].Pos) +
		barycentrics.y * (vertices[2].Pos - vertices[0].Pos);

	input.Tex = vertices[0].Tex +
		barycentrics.x * (vertices[1].Tex - vertices[0].Tex) +
		barycentrics.y * (vertices[2].Tex - vertices[0].Tex);

	return input;
}

[shader("closesthit")]
void MyClosestHitShader(inout RayPayload payload, MyAttributes attr)
{
	Vertex input = GetInput(attr.barycentrics);

	float4 color = 0.0;

	[unroll]
	for (uint i = 0; i < NUM_TEXTURES; ++i)
		color += g_textures[i].SampleLevel(g_sampler, input.Tex, 0);
	color /= NUM_TEXTURES;

	payload.color = color;

	//float3 barycentrics = float3(1.0 - attr.barycentrics.x - attr.barycentrics.y, attr.barycentrics.xy);
	//payload.color = float4(input.Tex, 0.0, 1.0);
}

[shader("miss")]
void MyMissShader(inout RayPayload payload)
{
	payload.color = float4(0.0, 0.4, 0.8, 1.0);
}
