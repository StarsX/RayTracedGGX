//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "Variance.hlsli"

//--------------------------------------------------------------------------------------
// Textures
//--------------------------------------------------------------------------------------
RWTexture2D<float4>	g_renderTarget;
Texture2D<float3>	g_txAverage;
Texture2D<float3>	g_txSquareAvg;
Texture2D			g_txNormal;
Texture2D<float>	g_txRoughness;
//Texture2D<float>	g_txDepth : register (t5);

groupshared float3 g_avgs[SHARED_MEM_SIZE];
groupshared float3 g_sqas[SHARED_MEM_SIZE];
groupshared float4 g_norms[SHARED_MEM_SIZE];
//groupshared float g_depths[SHARED_MEM_SIZE];
groupshared float g_rghs[SHARED_MEM_SIZE];

void loadSamples(uint2 dTid, uint gTid, uint radius)
{
	const uint offset = radius * 2;
	dTid.y -= radius;

	[unroll]
	for (uint i = 0; i < 2; ++i, dTid.y += offset, gTid += offset)
	{
		float4 norm = g_txNormal[dTid];
		g_avgs[gTid] = g_txAverage[dTid];
		g_sqas[gTid] = g_txSquareAvg[dTid];
		//g_depths[gTid] = g_txDepth[dTid];
		g_rghs[gTid] = g_txRoughness[dTid];

		norm.xyz = norm.xyz * 2.0 - 1.0;
		g_norms[gTid] = norm;
	}

	GroupMemoryBarrierWithGroupSync();
}

[numthreads(1, THREADS_PER_WAVE, 1)]
void main(uint2 DTid : SV_DispatchThreadID, uint2 GTid : SV_GroupThreadID)
{
	const float3 src = g_txSource[DTid];
	const bool vis = g_txNormal[DTid].w > 0.0;
	if (WaveActiveAllTrue(!vis))
	{
		g_renderTarget[DTid] = float4(src, 0.0);
		return;
	}
	const uint radius = RADIUS;
	loadSamples(DTid, GTid.y, radius);
	if (!vis)
	{
		g_renderTarget[DTid] = float4(src, 0.0);
		return;
	}

	const uint c = GTid.y + radius;
	const float3 normC = g_norms[c].xyz;

	const float roughness = g_txRoughness[DTid];
	const uint sampleCount = radius * 2 + 1;

	const float a = RoughnessSigma(roughness);
	float3 mu = 0.0, m2 = 0.0;
	float wsum = 0.0;

	//const float depthC = g_depths[c];

	[unroll]
	for (uint i = 0; i < sampleCount; ++i)
	{
		const uint j = GTid.y + i;
		const float4 norm = g_norms[j];
		const float w = (norm.w > 0.0 ? 1.0 : 0.0)
			* Gaussian(radius, i, a)
			* NormalWeight(normC.xyz, norm.xyz, SIGMA_N)
			//* Gaussian(depthC, g_depths[j], SIGMA_Z);
			* RoughnessWeight(roughness, g_rghs[j], 0.0, 0.5);
		mu += g_avgs[j] * w;
		m2 += g_sqas[j] * w;
		wsum += w;
	}

	const float gamma = 0.25;

	mu /= wsum;
	const float3 sigma = sqrt(abs(m2 / wsum - mu * mu));
	const float3 gsigma = gamma * sigma;
	const float3 neighborMin = mu - gsigma;
	const float3 neighborMax = mu + gsigma;

	const float3 result = clipColor(TM(src.xyz), neighborMin, neighborMax);

	g_renderTarget[DTid] = float4(ITM(result), 1.0);
}
