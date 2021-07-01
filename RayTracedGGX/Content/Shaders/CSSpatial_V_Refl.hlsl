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

[numthreads(8, 8, 1)]
void main(uint2 DTid : SV_DispatchThreadID)
{
	const float3 src = g_txSource[DTid];
	float4 normC = g_txNormal[DTid];
	if (normC.w <= 0.0)
	{
		g_renderTarget[DTid] = float4(src, 0.0);
		return;
	}

	const float roughness = g_txRoughness[DTid];
	const uint radius = RADIUS;
	const uint sampleCount = radius * 2 + 1;

	//const float depthC = g_txDepth[DTid];
	normC.xyz = normC.xyz * 2.0 - 1.0;

	const float a = RoughnessSigma(roughness);
	float3 mu = 0.0, m2 = 0.0;
	float wsum = 0.0;

	[unroll]
	for (uint i = 0; i < sampleCount; ++i)
	{
		const uint2 index = uint2(DTid.x, DTid.y + i - radius);
		float4 norm = g_txNormal[index];
		float3 avg = g_txAverage[index];
		//const float depth = g_txDepth[index];
		const float rgh = g_txRoughness[index];

		norm.xyz = norm.xyz * 2.0 - 1.0;
		const float w = (norm.w > 0.0 ? 1.0 : 0.0)
			* Gaussian(radius, i, a)
			* NormalWeight(normC.xyz, norm.xyz, SIGMA_N)
			//* Gaussian(depthC, depth, SIGMA_Z);
			* RoughnessWeight(roughness, rgh, 0.0, 0.5);
		mu += avg * w;
		wsum += w;
	}

	mu /= wsum;
	const float3 result = Denoise(src, mu, roughness);

	g_renderTarget[DTid] = float4(ITM(result), 1.0);
}
