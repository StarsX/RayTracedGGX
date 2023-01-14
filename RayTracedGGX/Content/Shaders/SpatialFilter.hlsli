//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "FilterCommon.hlsli"

#define THREADS_PER_WAVE 32
#define RADIUS 10
#define SAMPLE_COUNT (RADIUS * 2 + 1)
#define SHARED_MEM_SIZE (THREADS_PER_WAVE + RADIUS * 2)

#define SIGMA_G ((RADIUS + 0.5) / 3.0)
#define SIGMA_Z 4.0

static const float g_zNear = 1.0f;
static const float g_zFar = 1000.0f;

//--------------------------------------------------------------------------------------
// Texture
//--------------------------------------------------------------------------------------
Texture2D<float3>	g_txSource;

//--------------------------------------------------------------------------------------
// Sampler
//--------------------------------------------------------------------------------------
SamplerState g_sampler;

uint2 pack(float3 f)
{
	const uint3 u = f32tof16(f);

	return uint2(u.x | (u.y << 16), u.z);
}

uint2 pack(float4 f)
{
	const uint4 u = f32tof16(f);

	return uint2(u.x | (u.y << 16), u.z | (u.w << 16));
}

float4 unpack(uint2 u)
{
	return f16tof32(uint4(u.x & 0xffff, u.x >> 16, u.y & 0xffff, u.y >> 16));
}

//--------------------------------------------------------------------------------------
// Unproject and return z in viewing space
//--------------------------------------------------------------------------------------
float UnprojectZ(float depth)
{
	static const float3 unproj = { g_zNear - g_zFar, g_zFar, g_zNear * g_zFar };

	return unproj.z / (depth * unproj.x + unproj.y);
}

float ReflectionWeight(float3 normC, float4 norm, float rghC, float rgh,
	float depthC, float depth, float radius, int blurRadius)
{
	float w = norm.w > 0.0 ? 1.0 : 0.0;
	w *= Gaussian(radius, blurRadius);
	w *= NormalWeight(normC, norm.xyz, 512.0);
	//w *= Gaussian(depthC, depth, SIGMA_Z);
	w *= RoughnessWeight(rghC, rgh, 0.0, 0.5);

	return w;
}

float DiffuseWeight(float3 normC, float3 norm, float depthC, float depth)
{
	float w = NormalWeight(normC, norm, 32.0);
	//w *= Gaussian(depthC, depth, SIGMA_Z);

	return w;
}

float DiffuseWeight(float3 normC, float4 norm, float depthC, float depth, float mtl)
{
	float w = norm.w > 0.0 && mtl < 1.0 ? 1.0 : 0.0;
	w *= DiffuseWeight(normC, norm.xyz, depthC, depth);

	return w;
}
