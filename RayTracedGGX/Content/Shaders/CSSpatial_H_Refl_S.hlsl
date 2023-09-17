//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "SpatialFilter.hlsli"

//--------------------------------------------------------------------------------------
// Textures
//--------------------------------------------------------------------------------------
RWTexture2D<float3>	g_renderTarget;
Texture2D			g_txNormal		: register (t1);
Texture2D<float>	g_txRoughness	: register (t2);
Texture2D<float>	g_txDepth		: register (t3);

groupshared uint4 g_srcRghNrms[SHARED_MEM_SIZE];
groupshared float g_depths[SHARED_MEM_SIZE];

void loadSamples(uint2 dTid, uint gTid, uint radius)
{
	const uint offset = radius * 2;
	dTid.x -= radius;

	[unroll]
	for (uint i = 0; i < 2; ++i, dTid.x += 32, gTid += 32)
	{
		float3 src = g_txSource[dTid];
		float4 norm = g_txNormal[dTid];
		const float rgh = g_txRoughness[dTid];
		const float depth = g_txDepth[dTid];

		src = TM(src);
		norm.xyz = norm.xyz * 2.0 - 1.0;
		g_srcRghNrms[gTid] = uint4(pack(float4(src, rgh)), pack(norm));
		g_depths[gTid] = depth;
	}

	GroupMemoryBarrierWithGroupSync();
}

[numthreads(THREADS_PER_WAVE, 1, 1)]
void main(uint2 DTid : SV_DispatchThreadID, uint2 GTid : SV_GroupThreadID)
{
	float4 normC = g_txNormal[DTid];
	const bool vis = normC.w > 0.0;
	if (WaveActiveAllTrue(!vis)) return;

	loadSamples(DTid, GTid.x, RADIUS);
	if (!vis) return;

	float2 imageSize;
	g_renderTarget.GetDimensions(imageSize.x, imageSize.y);

	const float roughness = g_txRoughness[DTid];
	const float depthC = g_depths[GTid.x + RADIUS];
	normC.xyz = normC.xyz * 2.0 - 1.0;

	const float br = GaussianRadiusFromRoughness(roughness, imageSize);
	float3 mu = 0.0;
	float wsum = 0.0;

	[unroll]
	for (int i = -RADIUS; i <= RADIUS; ++i)
	{
		const uint j = GTid.x + i + RADIUS;
		const float4 srcRgh = unpack(g_srcRghNrms[j].xy);
		const float4 norm = unpack(g_srcRghNrms[j].zw);
		const float depth = g_depths[j];

		const float w = ReflectionWeight(normC.xyz, norm, roughness, srcRgh.w, depthC, depth, i, br);
		mu += srcRgh.xyz * w;
		wsum += w;
	}

	g_renderTarget[DTid] = mu / wsum;
}
