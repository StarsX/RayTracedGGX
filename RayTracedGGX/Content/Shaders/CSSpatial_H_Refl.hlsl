//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "SpatialFilter.hlsli"

//--------------------------------------------------------------------------------------
// Textures
//--------------------------------------------------------------------------------------
RWTexture2D<float3>	g_renderTarget;
Texture2D			g_txNormal;
Texture2D<float>	g_txRoughness;
//Texture2D<float>	g_txDepth : register (t4);

[numthreads(8, 8, 1)]
void main(uint2 DTid : SV_DispatchThreadID)
{
	float4 normC = g_txNormal[DTid];
	if (normC.w <= 0.0) return;

	const float roughness = g_txRoughness[DTid];
	//const float depthC = g_txDepth[DTid];
	const uint radius = RADIUS;
	const uint sampleCount = radius * 2 + 1;
	normC.xyz = normC.xyz * 2.0 - 1.0;
	
	const float a = RoughnessSigma(roughness);
	float3 mu = 0.0;
	float wsum = 0.0;

	const float depthC = 0.0, depth = 0.0;

	[unroll]
	for (uint i = 0; i < sampleCount; ++i)
	{
		const uint2 index = uint2(DTid.x + i - radius, DTid.y);
		float4 norm = g_txNormal[index];
		float3 src = g_txSource[index];
		//const float depth = g_txDepth[index];
		const float rgh = g_txRoughness[index];

		norm.xyz = norm.xyz * 2.0 - 1.0;
		src = TM(src);
		const float w = ReflectionWeight(normC.xyz, norm, roughness, rgh, depthC, depth, radius, i, a);
		mu += src * w;
		wsum += w;
	}

	g_renderTarget[DTid] = mu / wsum;
}
