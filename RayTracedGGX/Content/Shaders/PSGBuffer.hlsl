//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

//--------------------------------------------------------------------------------------
// Structs
//--------------------------------------------------------------------------------------
struct PSIn
{
	float4	Pos		: SV_POSITION;
	float4	CSPos	: POSCURRENT;
	float4	TSPos 	: POSHISTORY;
	float3	Norm	: NORMAL;
};

struct PSOut
{
	min16float4 Normal		: SV_TARGET0;
	min16float Roughness	: SV_TARGET1;
	min16float2 Velocity	: SV_TARGET2;
};

//--------------------------------------------------------------------------------------
// Constant buffer
//--------------------------------------------------------------------------------------
cbuffer cbPerObject
{
	uint g_instanceIdx;
};

static const float g_roughnesses[] = { 0.4, 0.1 };

//--------------------------------------------------------------------------------------
// Base geometry-buffer pass
//--------------------------------------------------------------------------------------
PSOut main(PSIn input)
{
	PSOut output;

	const float2 csPos = input.CSPos.xy / input.CSPos.w;
	const float2 tsPos = input.TSPos.xy / input.TSPos.w;
	const min16float2 velocity = min16float2(csPos - tsPos) * min16float2(0.5, -0.5);
	output.Normal = min16float4(normalize(input.Norm) * 0.5 + 0.5, (g_instanceIdx + 1) / 2.0);
	output.Roughness = min16float(g_roughnesses[g_instanceIdx]);
	output.Velocity = velocity;

	return output;
}
