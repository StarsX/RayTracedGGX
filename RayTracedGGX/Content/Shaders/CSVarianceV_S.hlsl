//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "Variance.hlsli"

#define HALF_SHARED_MEM_SIZE (SHARED_MEM_SIZE >> 1)

//--------------------------------------------------------------------------------------
// Textures
//--------------------------------------------------------------------------------------
RWTexture2D<float4>	g_renderTarget;
Texture2D<float3>	g_txAverage;
Texture2D<float3>	g_txSquareAvg;
Texture2D			g_txNormal;
Texture2D<float>	g_txRoughness;
//Texture2D<float>	g_txDepth : register (t5);

groupshared uint4 g_avgRghs[SHARED_MEM_SIZE];
groupshared uint4 g_norms[HALF_SHARED_MEM_SIZE];

void loadSamples(uint2 dTid, uint gTid, uint radius)
{
	const uint offset = radius * 2;
	dTid.y -= radius;

	const uint nTid = gTid;
	uint2 norms[2];

	[unroll]
	for (uint i = 0; i < 2; ++i, dTid.y += offset, gTid += offset)
	{
		const float3 avg = g_txAverage[dTid];
		const float3 sqa = g_txSquareAvg[dTid];
		float4 norm = g_txNormal[dTid];
		//const float depth = g_txDepth[dTid];
		const float rgh = g_txRoughness[dTid];

		const uint4 avgRgh = uint4(pack(float4(avg, rgh)), pack(sqa));
		//const uint4 avgRgh = uint4(pack(float4(avg, rgh)), pack(float4(sqa, depth)));
		norm.xyz = norm.xyz * 2.0 - 1.0;
		g_avgRghs[gTid] = avgRgh;
		norms[i] = pack(norm);
	}

	g_norms[nTid] = uint4(norms[0], norms[1]);

	GroupMemoryBarrierWithGroupSync();
}

[numthreads(1, THREADS_PER_WAVE, 1)]
void main(uint2 DTid : SV_DispatchThreadID, uint2 GTid : SV_GroupThreadID)
{
	const float3 src = g_txSource[DTid];
	float4 normC = g_txNormal[DTid];
	const bool vis = normC.w > 0.0;
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

	//const float depthC = g_depths[GTid.y + radius];
	normC.xyz = normC.xyz * 2.0 - 1.0;

	const float roughness = g_txRoughness[DTid];
	const uint sampleCount = radius * 2 + 1;

	const float a = RoughnessSigma(roughness);
	float3 mu = 0.0, m2 = 0.0;
	float wsum = 0.0;

	[unroll]
	for (uint i = 0; i < sampleCount; ++i)
	{
		const uint j = GTid.y + i;
		const uint k = j % HALF_SHARED_MEM_SIZE;
		const float4 avgRgh = unpack(g_avgRghs[j].xy);
		const float3 sqa = unpack(g_avgRghs[j].zw).xyz;
		const float4 norm = unpack(j < HALF_SHARED_MEM_SIZE ? g_norms[k].xy : g_norms[k].zw);

		const float w = (norm.w > 0.0 ? 1.0 : 0.0)
			* Gaussian(radius, i, a)
			* NormalWeight(normC.xyz, norm.xyz, SIGMA_N)
			//* Gaussian(depthC, g_depths[j], SIGMA_Z);
			* RoughnessWeight(roughness, avgRgh.w, 0.0, 0.5);
		mu += avgRgh.xyz * w;
		m2 += sqa * w;
		wsum += w;
	}

	mu /= wsum;
	const float3 sigma = sqrt(abs(m2 / wsum - mu * mu));
	const float3 gsigma = g_gamma * sigma;
	const float3 neighborMin = mu - gsigma;
	const float3 neighborMax = mu + gsigma;

	const float3 result = clipColor(TM(src.xyz), neighborMin, neighborMax);

	g_renderTarget[DTid] = float4(ITM(result), 1.0);
}
