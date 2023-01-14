//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "SpatialFilter.hlsli"

//--------------------------------------------------------------------------------------
// Textures
//--------------------------------------------------------------------------------------
RWTexture2D<float4>	g_renderTarget;
Texture2D<float3>	g_txAverage		: register (t1);
Texture2D			g_txDest		: register (t2);
Texture2D			g_txNormal		: register (t3);
Texture2D<float2>	g_txRoughMetal	: register (t4);
//Texture2D<float>	g_txDepth		: register (t5);

[numthreads(8, 8, 1)]
void main(uint2 DTid : SV_DispatchThreadID)
{
	const float4 dest = g_txDest[DTid];
	float4 normC = g_txNormal[DTid];
	if (normC.w <= 0.0 || g_txRoughMetal[DTid].y >= 1.0)
	{
		g_renderTarget[DTid] = dest;
		return;
	}

	const float3 src = g_txSource[DTid];
	//const float depthC = g_txDepth[DTid];
	normC.xyz = normC.xyz * 2.0 - 1.0;

	float3 mu = 0.0, m2 = 0.0;
	float wsum = 0.0;

	const float depthC = 0.0, depth = 0.0;

	[unroll]
	for (int i = -RADIUS; i <= RADIUS; ++i)
	{
		const uint2 index = uint2(DTid.x, (int)DTid.y + i);

		float4 norm = g_txNormal[index];
		const float mtl = g_txRoughMetal[index].y;

		if (norm.w <= 0.0 || mtl >= 1.0) continue;

		float3 avg = g_txAverage[index];
		//const float depth = g_txDepth[index];

		norm.xyz = norm.xyz * 2.0 - 1.0;
		const float w = DiffuseWeight(normC.xyz, norm.xyz, depthC, depth);
		mu += avg * w;
		wsum += w;
	}

	mu /= wsum;
	const float3 result = mu;
	//const float3 result = Denoise(src, mu, 1.0);

	g_renderTarget[DTid] = float4(dest.xyz + ITM(result), dest.w);
}
