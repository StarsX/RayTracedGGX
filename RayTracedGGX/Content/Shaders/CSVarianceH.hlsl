//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "FilterCommon.hlsli"

#define RADIUS 20

static const uint g_sampleCount = RADIUS * 2 + 1;

//--------------------------------------------------------------------------------------
// Texture and buffers
//--------------------------------------------------------------------------------------
RWTexture2D<float4>	g_renderTarget;
RWTexture2D<float4>	g_variance;
Texture2D			g_txSource;
Texture2D<float3>	g_txNormal;
Texture2D<float>	g_txDepth;

//--------------------------------------------------------------------------------------
// Samplers
//--------------------------------------------------------------------------------------
SamplerState g_sampler;

[numthreads(8, 8, 1)]
void main(uint2 DTid : SV_DispatchThreadID)
{
	float4 srcs[g_sampleCount];
	float3 norms[g_sampleCount];
	float depths[g_sampleCount];

	[unroll]
	for (uint i = 0; i < g_sampleCount; ++i)
		srcs[i] = g_txSource[uint2(DTid.x + i - RADIUS, DTid.y)];
	for (i = 0; i < g_sampleCount; ++i)
		norms[i] = g_txNormal[uint2(DTid.x + i - RADIUS, DTid.y)].xyz;
	for (i = 0; i < g_sampleCount; ++i)
		depths[i] = g_txDepth[uint2(DTid.x + i - RADIUS, DTid.y)];

	for (i = 0; i < g_sampleCount; ++i)
	{
		srcs[i].xyz = TM(srcs[i].xyz);
		norms[i] = norms[i] * 2.0 - 1.0;
	}
	
	float4 m1 = 0.0, m2 = 0.0;
	float wsum = 0.0;
	[unroll]
	for (i = 0; i < g_sampleCount; ++i)
	{
		const float w =
			NormalWeight(norms[RADIUS], norms[i], 16.0) *
			Gaussian(depths[RADIUS], depths[i], 0.01);
		const float4 src = srcs[i];
		const float4 wsrc = src * w;
		m1 += wsrc;
		m2 += wsrc * src;
		wsum += w;
	}

	g_renderTarget[DTid] = m1 / wsum;
	g_variance[DTid] = m2 / wsum;
}
