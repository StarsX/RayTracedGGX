//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "Variance.hlsli"

//--------------------------------------------------------------------------------------
// Texture and buffers
//--------------------------------------------------------------------------------------
Texture2D<float3>	g_txAverage;
Texture2D<float3>	g_txVariance;
Texture2D			g_txNormal;
Texture2D<float>	g_txRoughness;
Texture2D<float>	g_txDepth : register (t5);

void loadSamples(uint2 dTid, out float3 avgs[64], out float3 vars[64], out float3 norms[64], out float depths[64])
{
	dTid.y -= 16;
	const float3 avg0 = g_txAverage[dTid];
	const float3 var0 = g_txVariance[dTid];
	float3 norm0 = g_txNormal[dTid].xyz;
	const float depth0 = g_txDepth[dTid];

	dTid.y += 32;
	const float3 avg1 = g_txAverage[dTid];
	const float3 var1 = g_txVariance[dTid];
	float3 norm1 = g_txNormal[dTid].xyz;
	const float depth1 = g_txDepth[dTid];

	norm0 = norm0 * 2.0 - 1.0;
	norm1 = norm1 * 2.0 - 1.0;

	[unroll]
	for (uint i = 0; i < 32; ++i)
	{
		avgs[i] = WaveReadLaneAt(avg0, i);
		vars[i] = WaveReadLaneAt(var0, i);
		norms[i] = WaveReadLaneAt(norm0, i);
		depths[i] = WaveReadLaneAt(depth0, i);
	}

	[unroll]
	for (i = 0; i < 32; ++i)
	{
		const uint j = i + 32;
		avgs[j] = WaveReadLaneAt(avg1, i);
		vars[j] = WaveReadLaneAt(var1, i);
		norms[j] = WaveReadLaneAt(norm1, i);
		depths[j] = WaveReadLaneAt(depth1, i);
	}
}

[numthreads(1, 32, 1)]
void main(uint2 DTid : SV_DispatchThreadID, uint2 GTid : SV_GroupThreadID)
{
	const float4 src = g_txSource[DTid];
	if (g_txNormal[DTid].w <= 0.0)
	{
		g_renderTarget[DTid] = src;
		return;
	}

	float3 avgs[64], vars[64], norms[64];
	float depths[64];
	loadSamples(DTid, avgs, vars, norms, depths);

	const float roughness = g_txRoughness[DTid];
	const uint radius = 16;
	const uint sampleCount = 16 * 2 + 1;

	const uint start = GTid.y;
	const uint center = GTid.y + radius;

	const float a = 128.0 * roughness * roughness;
	float3 mu = 0.0, m2 = 0.0;
	float wsum = 0.0;

	[unroll]
	for (uint i = 0; i < sampleCount; ++i)
	{
		const uint j = start + i;
		const float w = Gaussian(radius, i, a) *
			NormalWeight(norms[center], norms[j], 16.0) *
			Gaussian(depths[center], depths[j], 0.01);
		mu += avgs[j] * w;
		m2 += vars[j] * w;
		wsum += w;
	}

	const float gamma = 0.25;

	mu /= wsum;
	const float3 sigma = sqrt(abs(m2 / wsum - mu * mu));
	const float3 gsigma = gamma * sigma;
	const float3 neighborMin = mu - gsigma;
	const float3 neighborMax = mu + gsigma;

	const float3 result = clipColor(TM(src.xyz), neighborMin, neighborMax);

	g_renderTarget[DTid] = float4(ITM(result), src.w);
}
