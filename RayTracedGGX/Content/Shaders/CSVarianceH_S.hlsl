//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "Variance.hlsli"

//--------------------------------------------------------------------------------------
// Textures
//--------------------------------------------------------------------------------------
RWTexture2D<float3>	g_renderTarget;
RWTexture2D<float3>	g_squareAvg;
Texture2D			g_txNormal;
Texture2D<float>	g_txRoughness;
//Texture2D<float>	g_txDepth : register (t3);

groupshared float3 g_srcs[SHARED_MEM_SIZE];
groupshared float4 g_norms[SHARED_MEM_SIZE];
//groupshared float g_depths[SHARED_MEM_SIZE];
groupshared float g_rghs[SHARED_MEM_SIZE];

void loadSamples(uint2 dTid, uint gTid, uint radius)
{
	const uint offset = radius * 2;
	dTid.x -= radius;

	[unroll]
	for (uint i = 0; i < 2; ++i, dTid.x += 32, gTid += 32)
	{
		const float3 src = g_txSource[dTid];
		float4 norm = g_txNormal[dTid];
		//g_depths[gTid] = g_txDepth[dTid];
		g_rghs[gTid] = g_txRoughness[dTid];

		norm.xyz = norm.xyz * 2.0 - 1.0;
		g_srcs[gTid] = TM(src);
		g_norms[gTid] = norm;
	}

	GroupMemoryBarrierWithGroupSync();
}

[numthreads(THREADS_PER_WAVE, 1, 1)]
void main(uint2 DTid : SV_DispatchThreadID, uint2 GTid : SV_GroupThreadID)
{
	const bool vis = g_txNormal[DTid].w > 0.0;
	if (WaveActiveAllTrue(!vis)) return;
	const uint radius = RADIUS;
	loadSamples(DTid, GTid.x, radius);
	if (!vis) return;

	const uint c = GTid.x + radius;
	const float3 normC = g_norms[c].xyz;

	const float roughness = g_txRoughness[DTid];
	const uint sampleCount = radius * 2 + 1;

	const float a = RoughnessSigma(roughness);
	float3 m1 = 0.0, m2 = 0.0;
	float wsum = 0.0;

	//const float depthC = g_depths[c];

	[unroll]
	for (uint i = 0; i < sampleCount; ++i)
	{
		const uint j = GTid.x + i;
		const float4 norm = g_norms[j];
		const float w = (norm.w > 0.0 ? 1.0 : 0.0)
			* Gaussian(radius, i, a)
			* NormalWeight(normC, norm.xyz, SIGMA_N)
			//* Gaussian(depthC, g_depths[j], SIGMA_Z);
			* RoughnessWeight(roughness, g_rghs[j], 0.0, 0.5);
		const float3 src = g_srcs[j];
		const float3 wsrc = src * w;
		m1 += wsrc;
		m2 += wsrc * src;
		wsum += w;
	}

	g_renderTarget[DTid] = m1 / wsum;
	g_squareAvg[DTid] = m2 / wsum;
}
