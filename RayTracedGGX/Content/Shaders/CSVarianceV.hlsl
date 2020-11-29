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
Texture2D			g_txSource;
Texture2D<float3>	g_txAverage;
Texture2D<float3>	g_txVariance;
Texture2D<float3>	g_txNormal;
Texture2D<float>	g_txDepth;

//--------------------------------------------------------------------------------------
// Samplers
//--------------------------------------------------------------------------------------
SamplerState g_sampler;

[numthreads(8, 8, 1)]
void main(uint2 DTid : SV_DispatchThreadID)
{
	float3 avgs[g_sampleCount], vars[g_sampleCount];
	float3 norms[g_sampleCount];
	float depths[g_sampleCount];

	const float4 src = g_txSource[DTid];
	[unroll]
	for (uint i = 0; i < g_sampleCount; ++i)
		avgs[i] = g_txAverage[uint2(DTid.x, DTid.y + i - RADIUS)];
	[unroll]
	for (i = 0; i < g_sampleCount; ++i)
		vars[i] = g_txVariance[uint2(DTid.x, DTid.y + i - RADIUS)];
	for (i = 0; i < g_sampleCount; ++i)
		norms[i] = g_txNormal[uint2(DTid.x, DTid.y + i - RADIUS)].xyz;
	for (i = 0; i < g_sampleCount; ++i)
		depths[i] = g_txDepth[uint2(DTid.x, DTid.y + i - RADIUS)];

	for (i = 0; i < g_sampleCount; ++i)
		norms[i] = norms[i] * 2.0 - 1.0;

	float3 mu = 0.0, m2 = 0.0;
	float wsum = 0.0;
	[unroll]
	for (i = 0; i < g_sampleCount; ++i)
	{
		const float w =
			NormalWeight(norms[RADIUS], norms[i], 16.0) *
			Gaussian(depths[RADIUS], depths[i], 0.01);
		mu += avgs[i] * w;
		m2 += vars[i] * w;
		wsum += w;
	}

	const float gamma = 0.1;

	mu /= wsum;
	const float3 sigma = sqrt(abs(m2 / wsum - mu * mu));
	const float3 gsigma = gamma * sigma;
	const float3 neighborMin = mu - gsigma;
	const float3 neighborMax = mu + gsigma;

	const float3 result = clipColor(TM(src.xyz), neighborMin, neighborMax);

	g_renderTarget[DTid] = float4(ITM(result), src.w);
}
