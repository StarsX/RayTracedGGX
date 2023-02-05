//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "SpatialFilter.hlsli"

//--------------------------------------------------------------------------------------
// Textures
//--------------------------------------------------------------------------------------
RWTexture2D<float4>	g_renderTarget;
Texture2D<float3>	g_txAverage;
Texture2D			g_txNormal;
Texture2D<float>	g_txRoughness;
//Texture2D<float>	g_txDepth : register (t4);

groupshared uint4 g_avgRghNrms[SHARED_MEM_SIZE];
//groupshared float g_depths[SHARED_MEM_SIZE];

void loadSamples(uint2 dTid, uint gTid, uint radius)
{
	const uint offset = radius * 2;
	dTid.y -= radius;

	[unroll]
	for (uint i = 0; i < 2; ++i, dTid.y += offset, gTid += offset)
	{
		const float3 avg = g_txAverage[dTid];
		float4 norm = g_txNormal[dTid];
		const float rgh = g_txRoughness[dTid];
		//const float depth = g_txDepth[dTid];

		norm.xyz = norm.xyz * 2.0 - 1.0;
		g_avgRghNrms[gTid] = uint4(pack(float4(avg, rgh)), pack(norm));
		//g_depths[gTid] = depth;
	}

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

	loadSamples(DTid, GTid.y, RADIUS);
	if (!vis)
	{
		g_renderTarget[DTid] = float4(src, 0.0);
		return;
	}

	float2 imageSize;
	g_renderTarget.GetDimensions(imageSize.x, imageSize.y);

	//const float depthC = g_depths[GTid.y + RADIUS];
	const float roughness = g_txRoughness[DTid];
	normC.xyz = normC.xyz * 2.0 - 1.0;

	const float br = GaussianRadiusFromRoughness(roughness, imageSize);
	float3 mu = 0.0, m2 = 0.0;
	float wsum = 0.0;

	const float depthC = 0.0, depth = 0.0;

	[unroll]
	for (int i = -RADIUS; i <= RADIUS; ++i)
	{
		const uint j = GTid.y + i + RADIUS;
		const float4 avgRgh = unpack(g_avgRghNrms[j].xy);
		const float4 norm = unpack(g_avgRghNrms[j].zw);

		const float w = ReflectionWeight(normC.xyz, norm, roughness, avgRgh.w, depthC, depth, i, br);
		mu += avgRgh.xyz * w;
		wsum += w;
	}

	mu /= wsum;
	const float3 result = mu;
	//const float3 result = Denoise(src, mu, roughness);

	g_renderTarget[DTid] = float4(ITM(result), 1.0);
}
