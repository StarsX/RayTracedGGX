//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "FilterCommon.hlsli"

#define RADIUS 48
#define SAMPLE_COUNT (RADIUS * 2 + 1)

//--------------------------------------------------------------------------------------
// Texture and buffers
//--------------------------------------------------------------------------------------
RWTexture2D<float4>	g_renderTarget;
RWTexture2D<float4>	g_variance;
Texture2D			g_txSource;
Texture2D<float3>	g_txNormal;
Texture2D<float>	g_txRoughness;
Texture2D<float>	g_txDepth : register (t3);

//--------------------------------------------------------------------------------------
// Samplers
//--------------------------------------------------------------------------------------
SamplerState g_sampler;

[numthreads(8, 8, 1)]
void main(uint2 DTid : SV_DispatchThreadID)
{
	float4 srcs[SAMPLE_COUNT];
	float3 norms[SAMPLE_COUNT];
	float depths[SAMPLE_COUNT];

	const float roughness = g_txRoughness[DTid];
	const uint radius = min(RADIUS * roughness, 20);
	const uint sampleCount = radius * 2 + 1;

	for (uint i = 0; i < sampleCount; ++i)
	{
		srcs[i] = g_txSource[uint2(DTid.x + i - radius, DTid.y)];
		norms[i] = g_txNormal[uint2(DTid.x + i - radius, DTid.y)].xyz;
		depths[i] = g_txDepth[uint2(DTid.x + i - radius, DTid.y)];
	}
	
	float4 m1 = 0.0, m2 = 0.0;
	float wsum = 0.0;
	for (i = 0; i < sampleCount; ++i)
	{
		srcs[i].xyz = TM(srcs[i].xyz);
		norms[i] = norms[i] * 2.0 - 1.0;
		const float w =
			NormalWeight(norms[radius], norms[i], 16.0) *
			Gaussian(depths[radius], depths[i], 0.01);
		const float4 src = srcs[i];
		const float4 wsrc = src * w;
		m1 += wsrc;
		m2 += wsrc * src;
		wsum += w;
	}

	g_renderTarget[DTid] = m1 / wsum;
	g_variance[DTid] = m2 / wsum;
}
