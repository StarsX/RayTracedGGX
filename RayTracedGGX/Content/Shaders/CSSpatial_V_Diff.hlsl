//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "Variance.hlsli"

//--------------------------------------------------------------------------------------
// Textures
//--------------------------------------------------------------------------------------
RWTexture2D<float4>	g_renderTarget;
Texture2D<float3>	g_txAverage;
Texture2D<float3>	g_txSquareAvg;
Texture2D			g_txDest;
Texture2D			g_txNormal;
Texture2D<float2>	g_txRoughMetal;
//Texture2D<float>	g_txDepth : register (t5);

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
	const uint radius = RADIUS;
	const uint sampleCount = radius * 2 + 1;
	normC.xyz = normC.xyz * 2.0 - 1.0;

	const float a = RoughnessSigma(1.0);
	float3 mu = 0.0, m2 = 0.0;
	float wsum = 0.0;

	[unroll]
	for (uint i = 0; i < sampleCount; ++i)
	{
		const uint2 index = uint2(DTid.x, DTid.y + i - radius);
		float4 norm = g_txNormal[index];
		const float mtl = g_txRoughMetal[index].y;

		if (norm.w <= 0.0 || mtl >= 1.0) continue;

		float3 avg = g_txAverage[index];
		float3 sqa = g_txSquareAvg[index];
		//const float depth = g_txDepth[index];

		norm.xyz = norm.xyz * 2.0 - 1.0;
		const float w = Gaussian(radius, i, a)
			* NormalWeight(normC.xyz, norm.xyz, SIGMA_N);
			//* Gaussian(depthC, depth, SIGMA_Z);
		mu += avg * w;
		m2 += sqa * w;
		wsum += w;
	}

	mu /= wsum;
	const float3 sigma = sqrt(abs(m2 / wsum - mu * mu));
	const float3 gsigma = g_gammaDiff * sigma;
	const float3 neighborMin = mu - gsigma;
	const float3 neighborMax = mu + gsigma;

	const float3 result = clipColor(TM(src), neighborMin, neighborMax);

	g_renderTarget[DTid] = float4(dest.xyz + ITM(result), dest.w);
}
