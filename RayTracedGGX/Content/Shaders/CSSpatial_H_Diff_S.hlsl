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
Texture2D<float2>	g_txRoughMetal;
//Texture2D<float>	g_txDepth : register (t3);

groupshared uint4 g_srcMtlNrms[SHARED_MEM_SIZE];

void loadSamples(uint2 dTid, uint gTid, uint radius)
{
	const uint offset = radius * 2;
	dTid.x -= radius;

	[unroll]
	for (uint i = 0; i < 2; ++i, dTid.x += 32, gTid += 32)
	{
		float3 src = g_txSource[dTid];
		float4 norm = g_txNormal[dTid];
		//const float depth = g_txDepth[dTid];
		const float mtl = g_txRoughMetal[dTid].y;

		src = TM(src);
		norm.xyz = norm.xyz * 2.0 - 1.0;
		g_srcMtlNrms[gTid] = uint4(pack(float4(src, mtl)), pack(norm));
	}

	GroupMemoryBarrierWithGroupSync();
}

[numthreads(THREADS_PER_WAVE, 1, 1)]
void main(uint2 DTid : SV_DispatchThreadID, uint2 GTid : SV_GroupThreadID)
{
	float4 normC = g_txNormal[DTid];
	const bool vis = normC.w > 0.0 && g_txRoughMetal[DTid].y < 1.0;
	if (WaveActiveAllTrue(!vis)) return;
	const uint radius = RADIUS;
	loadSamples(DTid, GTid.x, radius);
	if (!vis) return;

	//const float depthC = g_depths[GTid.x + radius];
	const uint sampleCount = radius * 2 + 1;
	normC.xyz = normC.xyz * 2.0 - 1.0;

	float3 m1 = 0.0, m2 = 0.0;
	float wsum = 0.0;

	[unroll]
	for (uint i = 0; i < sampleCount; ++i)
	{
		const uint j = GTid.x + i;
		const float4 srcMtl = unpack(g_srcMtlNrms[j].xy);
		const float4 norm = unpack(g_srcMtlNrms[j].zw);

		const float w = (norm.w > 0.0 && srcMtl.w < 1.0 ? 1.0 : 0.0)
			* NormalWeight(normC.xyz, norm.xyz, SIGMA_N);
			//* Gaussian(depthC, g_depths[j], SIGMA_Z);
		const float3 wsrc = srcMtl.xyz * w;
		m1 += wsrc;
		m2 += wsrc * srcMtl.xyz;
		wsum += w;
	}

	g_renderTarget[DTid] = m1 / wsum;
	g_squareAvg[DTid] = m2 / wsum;
}
