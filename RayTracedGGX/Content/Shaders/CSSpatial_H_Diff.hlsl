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
//Texture2D<float>	g_txDepth : register (t3);

[numthreads(8, 8, 1)]
void main(uint2 DTid : SV_DispatchThreadID)
{
	float4 normC = g_txNormal[DTid];
	if (normC.w <= 0.0 || g_txRoughMetal[DTid].y >= 1.0) return;

	//const float depthC = g_txDepth[DTid];
	normC.xyz = normC.xyz * 2.0 - 1.0;

	float3 mu = 0.0;
	float wsum = 0.0;

	const float depthC = 0.0, depth = 0.0;

	[unroll]
	for (int i = -RADIUS; i <= RADIUS; ++i)
	{
		const uint2 index = uint2((int)DTid.x + i, DTid.y);

		float4 norm = g_txNormal[index];
		const float mtl = g_txRoughMetal[index].y;

		if (norm.w <= 0.0 || mtl >= 1.0) continue;

		float3 src = g_txSource[index];
		//const float depth = g_txDepth[index];

		norm.xyz = norm.xyz * 2.0 - 1.0;
		src = TM(src);
		const float w = DiffuseWeight(normC.xyz, norm.xyz, depthC, depth);
		mu += src * w;
		wsum += w;
	}

	g_renderTarget[DTid] = mu / wsum;
}
