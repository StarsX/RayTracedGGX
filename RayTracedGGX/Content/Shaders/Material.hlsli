//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

static const float g_roughnesses[] = { 0.5, 0.16 };

min16float getRoughness(uint instanceIdx, float2 uv)
{
	min16float roughness = min16float(g_roughnesses[instanceIdx]);
	if (instanceIdx == 0)
	{
		uint2 p = uv * 5.0;
		p &= 0x1;
		roughness = p.x ^ p.y ? roughness * 0.25 : roughness;
	}

	return roughness;
}
