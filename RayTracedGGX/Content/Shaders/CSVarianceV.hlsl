//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#define RADIUS 8

static const uint g_sampleCount = RADIUS * 2 + 1;
static const float3 g_lumBase = { 0.25, 0.5, 0.25 };

//--------------------------------------------------------------------------------------
// Texture and buffers
//--------------------------------------------------------------------------------------
RWTexture2D<float4>	g_renderTarget;
RWTexture2D<float4>	g_variance;
Texture2D			g_txSource;
Texture2D			g_txVariance;

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

[numthreads(8, 8, 1)]
void main(uint2 DTid : SV_DispatchThreadID)
{
	float4 srcs[g_sampleCount], vars[g_sampleCount];

	[unroll]
	for (uint i = 0; i < g_sampleCount; ++i)
		srcs[i] = g_txSource[uint2(DTid.x, DTid.y + i - RADIUS)];
	[unroll]
	for (i = 0; i < g_sampleCount; ++i)
		vars[i] = g_txVariance[uint2(DTid.x, DTid.y + i - RADIUS)];

	float4 m1 = 0.0, m2 = 0.0;
	[unroll]
	for (i = 0; i < g_sampleCount; ++i)
	{
		m1 += srcs[i];
		m2 += vars[i];
	}

	m1 /= g_sampleCount;
	m2 /= g_sampleCount;

	g_renderTarget[DTid] = float4(ITM(m1.xyz), m1.w);
	g_variance[DTid] = float4(ITM(m2.xyz), m2.w);
}
