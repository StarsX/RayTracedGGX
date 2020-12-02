//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "FilterCommon.hlsli"

#define THREADS_PER_WAVE 32
#define RADIUS 16
#define SAMPLE_COUNT (RADIUS * 2 + 1)
#define SHARED_MEM_SIZE (THREADS_PER_WAVE + RADIUS * 2)

#define SIGMA_N 16.0
#define SIGMA_Z 0.01

//--------------------------------------------------------------------------------------
// Texture and buffers
//--------------------------------------------------------------------------------------
RWTexture2D<float4>	g_renderTarget;
Texture2D			g_txSource;

//--------------------------------------------------------------------------------------
// Samplers
//--------------------------------------------------------------------------------------
SamplerState g_sampler;
