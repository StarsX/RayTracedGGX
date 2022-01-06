//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#define NUM_MESH 2

//--------------------------------------------------------------------------------------
// Constant buffer
//--------------------------------------------------------------------------------------
cbuffer cbPerObject : register (b0)
{
	float4 g_baseColors[NUM_MESH];
	float2 g_roughMetals[NUM_MESH];
};

float2 getUV(float3 norm, float3 pos, float3 scl)
{
	float2 uv = abs(norm.x) * pos.yz * scl.yz;
	uv += abs(norm.y) * pos.zx * scl.zx;
	uv += abs(norm.z) * pos.xy * scl.xy;

	return uv * 0.5 + 0.5;
}

min16float4 getBaseColor(uint instanceIdx, float2 uv)
{
	return min16float4(g_baseColors[instanceIdx]);
}

min16float getRoughness(uint instanceIdx, float2 uv, min16float roughness)
{
	if (instanceIdx == 0)
	{
		uint2 p = uv * 5.0;
		p &= 0x1;
		roughness = p.x ^ p.y ? roughness * 0.25 : roughness;
	}

	return roughness;
}

min16float2 getRoughMetal(uint instanceIdx, float2 uv)
{
	const float2 roughMetal = g_roughMetals[instanceIdx];
	const min16float roughness = getRoughness(instanceIdx, uv, min16float(roughMetal.x));

	return min16float2(roughness, roughMetal.y);
}

min16float getRoughness(uint instanceIdx, float2 uv)
{
	const float roughness = g_roughMetals[instanceIdx].x;

	return getRoughness(instanceIdx, uv, min16float(roughness));
}

min16float getMetallic(uint instanceIdx, float2 uv)
{
	return min16float(g_roughMetals[instanceIdx].y);
}
