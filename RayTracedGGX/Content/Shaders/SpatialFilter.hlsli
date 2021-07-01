//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "FilterCommon.hlsli"

#define THREADS_PER_WAVE 32
#define RADIUS 16
#define SAMPLE_COUNT (RADIUS * 2 + 1)
#define SHARED_MEM_SIZE (THREADS_PER_WAVE + RADIUS * 2)

#define SIGMA_N 16.0
#define SIGMA_Z 0.01

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
