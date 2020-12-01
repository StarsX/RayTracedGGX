//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "Variance.hlsli"

//--------------------------------------------------------------------------------------
// Texture and buffers
//--------------------------------------------------------------------------------------
RWTexture2D<float4>	g_variance;
Texture2D			g_txNormal;
Texture2D<float>	g_txRoughness;
Texture2D<float>	g_txDepth : register (t3);

void loadSamples(uint2 dTid, out float4 srcs[64], out float3 norms[64], out float depths[64])
{
	dTid.x -= 16;
	float4 src0 = g_txSource[dTid];
	float3 norm0 = g_txNormal[dTid].xyz;
	const float depth0 = g_txDepth[dTid];

	dTid.x += 32;
	float4 src1 = g_txSource[dTid];
	float3 norm1 = g_txNormal[dTid].xyz;
	const float depth1 = g_txDepth[dTid];

	src0.xyz = TM(src0.xyz);
	src1.xyz = TM(src1.xyz);
	norm0 = norm0 * 2.0 - 1.0;
	norm1 = norm1 * 2.0 - 1.0;

	[unroll]
	for (uint i = 0; i < 32; ++i)
	{
		srcs[i] = WaveReadLaneAt(src0, i);
		norms[i] = WaveReadLaneAt(norm0, i);
		depths[i] = WaveReadLaneAt(depth0, i);
	}

	[unroll]
	for (i = 0; i < 32; ++i)
	{
		const uint j = i + 32;
		srcs[j] = WaveReadLaneAt(src1, i);
		norms[j] = WaveReadLaneAt(norm1, i);
		depths[j] = WaveReadLaneAt(depth1, i);
	}
}

[numthreads(32, 1, 1)]
void main(uint2 DTid : SV_DispatchThreadID, uint2 GTid : SV_GroupThreadID)
{
	if (g_txNormal[DTid].w <= 0.0) return;

	float4 srcs[64];
	float3 norms[64];
	float depths[64];
	loadSamples(DTid, srcs, norms, depths);

	const float roughness = g_txRoughness[DTid];
	const uint radius = 16;
	const uint sampleCount = 16 * 2 + 1;

	const uint start = GTid.x;
	const uint center = GTid.x + radius;

	const float a = 128.0 * roughness * roughness;
	float4 m1 = 0.0, m2 = 0.0;
	float wsum = 0.0;

	[unroll]
	for (uint i = 0; i < sampleCount; ++i)
	{
		const uint j = start + i;
		const float w = Gaussian(radius, i, a) *
			NormalWeight(norms[center], norms[j], 16.0) *
			Gaussian(depths[center], depths[j], 0.01);
		const float4 src = srcs[j];
		const float4 wsrc = src * w;
		m1 += wsrc;
		m2 += wsrc * src;
		wsum += w;
	}

	g_renderTarget[DTid] = m1 / wsum;
	g_variance[DTid] = m2 / wsum;
}
