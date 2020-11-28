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

//--------------------------------------------------------------------------------------
// Samplers
//--------------------------------------------------------------------------------------
SamplerState g_sampler;

//--------------------------------------------------------------------------------------
// A fast invertible tone map that preserves color (Reinhard)
//--------------------------------------------------------------------------------------
float3 TM(float3 hdr)
{
	const float3 rgb = float3(hdr);

	return rgb / (1.0 + dot(rgb, g_lumBase));
}

[numthreads(8, 8, 1)]
void main(uint2 DTid : SV_DispatchThreadID)
{
	float4 srcs[g_sampleCount];

	[unroll]
	for (uint i = 0; i < g_sampleCount; ++i)
		srcs[i] = g_txSource[uint2(DTid.x + i - RADIUS, DTid.y)];
	
	float4 m1 = 0.0, m2 = 0.0;
	[unroll]
	for (i = 0; i < g_sampleCount; ++i)
	{
		const float4 srcTM = { TM(srcs[i].xyz), srcs[i].w };
		m1 += srcTM;
		m2 += srcTM * srcTM;
	}

	g_renderTarget[DTid] = m1 / g_sampleCount;
	g_variance[DTid] = m2 / g_sampleCount;
}
