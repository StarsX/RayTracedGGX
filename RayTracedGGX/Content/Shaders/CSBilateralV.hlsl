//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "FilterCommon.hlsli"

#define RADIUS 2

static const uint g_sampleCount = RADIUS * 2 + 1;

//--------------------------------------------------------------------------------------
// Texture and buffers
//--------------------------------------------------------------------------------------
RWTexture2D<float4>	g_rwRenderTarget;
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
		srcs[i] = g_txSource[uint2(DTid.x, DTid.y + i - RADIUS)];
	for (i = 0; i < g_sampleCount; ++i)
		norms[i] = g_txNormal[uint2(DTid.x, DTid.y + i - RADIUS)].xyz;
	for (i = 0; i < g_sampleCount; ++i)
		depths[i] = g_txDepth[uint2(DTid.x, DTid.y + i - RADIUS)];

	for (i = 0; i < g_sampleCount; ++i)
		norms[i] = norms[i] * 2.0 - 1.0;

	float4 sum = 0.0;
	float wsum = 0.0;
	[unroll]
	for (i = 0; i < g_sampleCount; ++i)
	{
		const float w =
			NormalWeight(norms[RADIUS], norms[i], 64.0) *
			Gaussian(depths[RADIUS], depths[i], 0.05);
		sum += srcs[i] * w;
		wsum += w;
	}

	g_rwRenderTarget[DTid] = sum / wsum;
}
