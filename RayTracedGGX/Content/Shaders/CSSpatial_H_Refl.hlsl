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
Texture2D<float>	g_txRoughness;
//Texture2D<float>	g_txDepth : register (t3);

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
	float3 m1 = 0.0, m2 = 0.0;
	float wsum = 0.0;

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
		const float w = (norm.w > 0.0 ? 1.0 : 0.0)
			* Gaussian(radius, i, a)
			* NormalWeight(normC.xyz, norm.xyz, SIGMA_N)
			//* Gaussian(depthC, depth, SIGMA_Z);
			* RoughnessWeight(roughness, rgh, 0.0, 0.5);
		const float3 wsrc = src * w;
		m1 += wsrc;
		m2 += wsrc * src;
		wsum += w;
	}

	g_renderTarget[DTid] = m1 / wsum;
	g_squareAvg[DTid] = m2 / wsum;
}
