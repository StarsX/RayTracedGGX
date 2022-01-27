//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "SpatialFilter.hlsli"

//--------------------------------------------------------------------------------------
// Textures
//--------------------------------------------------------------------------------------
RWTexture2D<float3>	g_renderTarget;
Texture2D			g_txNormal;
Texture2D<float2>	g_txRoughMetal;
//Texture2D<float>	g_txDepth : register (t4);

groupshared uint4 g_srcMtlNrms[SHARED_MEM_SIZE];
//groupshared float g_depths[SHARED_MEM_SIZE];

void loadSamples(uint2 dTid, uint gTid, uint radius)
{
	const uint offset = radius * 2;
	dTid.x -= radius;

	[unroll]
	for (uint i = 0; i < 2; ++i, dTid.x += 32, gTid += 32)
	{
		float3 src = g_txSource[dTid];
		float4 norm = g_txNormal[dTid];
		const float mtl = g_txRoughMetal[dTid].y;
		//const float depth = g_txDepth[dTid];

		src = TM(src);
		norm.xyz = norm.xyz * 2.0 - 1.0;
		g_srcMtlNrms[gTid] = uint4(pack(float4(src, mtl)), pack(norm));
		//g_depths[gTid] = depth;
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

	float3 mu = 0.0;
	float wsum = 0.0;

	const float depthC = 0.0, depth = 0.0;

	[unroll]
	for (uint i = 0; i < sampleCount; ++i)
	{
		const uint j = GTid.x + i;
		const float4 srcMtl = unpack(g_srcMtlNrms[j].xy);
		const float4 norm = unpack(g_srcMtlNrms[j].zw);

		const float w = DiffuseWeight(normC.xyz, norm, depthC, depth, srcMtl.w);
		mu += srcMtl.xyz * w;
		wsum += w;
	}

	g_renderTarget[DTid] = mu / wsum;
}
