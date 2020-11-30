//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "FilterCommon.hlsli"

#define RADIUS 20
#define SAMPLE_COUNT (RADIUS * 2 + 1)

//--------------------------------------------------------------------------------------
// Texture and buffers
//--------------------------------------------------------------------------------------
RWTexture2D<float4>	g_renderTarget;
Texture2D			g_txSource;
Texture2D<float3>	g_txAverage;
Texture2D<float3>	g_txVariance;
Texture2D<float3>	g_txNormal;
Texture2D<float>	g_txRoughness;
Texture2D<float>	g_txDepth : register (t5);

//--------------------------------------------------------------------------------------
// Samplers
//--------------------------------------------------------------------------------------
SamplerState g_sampler;

[numthreads(8, 8, 1)]
void main(uint2 DTid : SV_DispatchThreadID)
{
	float3 avgs[SAMPLE_COUNT], vars[SAMPLE_COUNT];
	float3 norms[SAMPLE_COUNT];
	float depths[SAMPLE_COUNT];

	const float4 src = g_txSource[DTid];
	const float roughness = g_txRoughness[DTid];
	const uint radius = RADIUS;
	const uint sampleCount = radius * 2 + 1;
	for (uint i = 0; i < sampleCount; ++i)
	{
		avgs[i] = g_txAverage[uint2(DTid.x, DTid.y + i - radius)];
		vars[i] = g_txVariance[uint2(DTid.x, DTid.y + i - radius)];
		norms[i] = g_txNormal[uint2(DTid.x, DTid.y + i - radius)].xyz;
		depths[i] = g_txDepth[uint2(DTid.x, DTid.y + i - radius)];
	}

	const float a = 128.0 * roughness * roughness;
	float3 mu = 0.0, m2 = 0.0;
	float wsum = 0.0;
	for (i = 0; i < sampleCount; ++i)
	{
		norms[i] = norms[i] * 2.0 - 1.0;
		const float w = Gaussian(radius, i, a) *
			NormalWeight(norms[radius], norms[i], 16.0) *
			Gaussian(depths[radius], depths[i], 0.01);
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
