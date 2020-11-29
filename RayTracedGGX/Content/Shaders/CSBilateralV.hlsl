//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#define RADIUS 10

static const uint g_sampleCount = RADIUS * 2 + 1;
static const float3 g_lumBase = { 0.25, 0.5, 0.25 };

//--------------------------------------------------------------------------------------
// Texture and buffers
//--------------------------------------------------------------------------------------
RWTexture2D<float4>	g_rwRenderTarget;
Texture2D			g_txSource;
Texture2D<float3>	g_txNormal;
Texture2D<float>	g_txDepth;

//--------------------------------------------------------------------------------------
// Samplers
//--------------------------------------------------------------------------------------
SamplerState g_sampler;

//--------------------------------------------------------------------------------------
// Inverse of preceding function
//--------------------------------------------------------------------------------------
float3 ITM(float3 rgb)
{
	return rgb / max(1.0 - dot(rgb, g_lumBase), 1e-4);
}

float NormalWeight(float3 normal_p, float3 normal_q, float sigma)
{
	return pow(max(dot(normal_p, normal_q), 0.0), sigma);
}

float RoughnessWeight(float roughness_p, float roughness_q, float sigma_min, float sigma_max)
{
	return 1.0 - smoothstep(sigma_min, sigma_max, abs(roughness_p - roughness_q));
}

float Gaussian(float x, float m, float sigma)
{
	const float r = x - m;
	const float a = dot(r, r) / (sigma * sigma);

	return exp(-0.5 * a);
}

[numthreads(8, 8, 1)]
void main(uint2 DTid : SV_DispatchThreadID)
{
	float4 srcs[g_sampleCount];
	float3 norms[g_sampleCount];
	float depths[g_sampleCount];

	[unroll]
	for (uint i = 0; i < g_sampleCount; ++i)
		srcs[i] = g_txSource[uint2(DTid.x, DTid.y + i - RADIUS)];
	for (i = 0; i < g_sampleCount; ++i)
		norms[i] = g_txNormal[uint2(DTid.x, DTid.y + i - RADIUS)].xyz;
	for (i = 0; i < g_sampleCount; ++i)
		depths[i] = g_txDepth[uint2(DTid.x, DTid.y + i - RADIUS)];

	for (i = 0; i < g_sampleCount; ++i)
		norms[i] = norms[i] * 2.0 - 1.0;

	float4 sum = 0.0;
	float wsum = 0.0;
	[unroll]
	for (i = 0; i < g_sampleCount; ++i)
	{
		const float w =
			NormalWeight(norms[RADIUS], norms[i], 64.0) *
			Gaussian(depths[RADIUS], depths[i], 0.05);
		sum += srcs[i] * w;
		wsum += w;
	}

	g_rwRenderTarget[DTid] = sum / wsum;
}
