//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "Variance.hlsli"

//--------------------------------------------------------------------------------------
// Texture and buffers
//--------------------------------------------------------------------------------------
RWTexture2D<float4>	g_variance;
Texture2D<float3>	g_txNormal;
Texture2D<float>	g_txRoughness;
Texture2D<float>	g_txDepth : register (t3);

[numthreads(8, 8, 1)]
void main(uint2 DTid : SV_DispatchThreadID)
{
	float4 srcs[SAMPLE_COUNT];
	float3 norms[SAMPLE_COUNT];
	float depths[SAMPLE_COUNT];

	const float roughness = g_txRoughness[DTid];
	const uint radius = RADIUS;
	const uint sampleCount = radius * 2 + 1;

	[unroll]
	for (uint i = 0; i < sampleCount; ++i)
	{
		srcs[i] = g_txSource[uint2(DTid.x + i - radius, DTid.y)];
		norms[i] = g_txNormal[uint2(DTid.x + i - radius, DTid.y)].xyz;
		depths[i] = g_txDepth[uint2(DTid.x + i - radius, DTid.y)];
	}

	[unroll]
	for (i = 0; i < sampleCount; ++i)
		norms[i] = norms[i] * 2.0 - 1.0;
	
	const float a = 128.0 * roughness * roughness;
	float4 m1 = 0.0, m2 = 0.0;
	float wsum = 0.0;

	[unroll]
	for (i = 0; i < sampleCount; ++i)
	{
		srcs[i].xyz = TM(srcs[i].xyz);
		const float w = Gaussian(radius, i, a) *
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
