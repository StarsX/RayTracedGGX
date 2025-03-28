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

[numthreads(8, 8, 1)]
void main(uint2 DTid : SV_DispatchThreadID)
{
	float4 normC = g_txNormal[DTid];
	if (normC.w <= 0.0) return;

	float2 imageSize;
	g_renderTarget.GetDimensions(imageSize.x, imageSize.y);

	const float roughness = g_txRoughness[DTid];
	const float depthC = g_txDepth[DTid];
	normC.xyz = normC.xyz * 2.0 - 1.0;
	
	const float br = GaussianRadiusFromRoughness(roughness, imageSize);
	float3 mu = 0.0;
	float wsum = 0.0;

	[unroll]
	for (int i = -RADIUS; i <= RADIUS; ++i)
	{
		const uint2 index = uint2((int)DTid.x + i, DTid.y);

		float4 norm = g_txNormal[index];
		float3 src = g_txSource[index];
		const float depth = g_txDepth[index];
		const float rgh = g_txRoughness[index];

		norm.xyz = norm.xyz * 2.0 - 1.0;
		src = TM(src);
		const float w = ReflectionWeight(normC.xyz, norm, roughness, rgh, depthC, depth, abs(i), br);
		mu += src * w;
		wsum += w;
	}

	g_renderTarget[DTid] = mu / wsum;
}
