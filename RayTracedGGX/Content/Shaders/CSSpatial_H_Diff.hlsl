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

[numthreads(8, 8, 1)]
void main(uint2 DTid : SV_DispatchThreadID)
{
	float4 normC = g_txNormal[DTid];
	if (normC.w <= 0.0 || g_txRoughMetal[DTid].y >= 1.0) return;

	//const float depthC = g_txDepth[DTid];
	const uint radius = RADIUS;
	const uint sampleCount = radius * 2 + 1;
	normC.xyz = normC.xyz * 2.0 - 1.0;

	float3 m1 = 0.0, m2 = 0.0;
	float wsum = 0.0;

	[unroll]
	for (uint i = 0; i < sampleCount; ++i)
	{
		const uint2 index = uint2(DTid.x + i - radius, DTid.y);
		float4 norm = g_txNormal[index];
		const float mtl = g_txRoughMetal[index].y;

		if (norm.w <= 0.0 || mtl >= 1.0) continue;

		float3 src = g_txSource[index];
		//const float depth = g_txDepth[index];

		norm.xyz = norm.xyz * 2.0 - 1.0;
		src = TM(src);
		const float w = NormalWeight(normC.xyz, norm.xyz, SIGMA_N);
			//* Gaussian(depthC, depth, SIGMA_Z);
		const float3 wsrc = src * w;
		m1 += wsrc;
		m2 += wsrc * src;
		wsum += w;
	}

	g_renderTarget[DTid] = m1 / wsum;
	g_squareAvg[DTid] = m2 / wsum;
}
