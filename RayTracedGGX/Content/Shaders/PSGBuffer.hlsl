//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "Material.hlsli"

//--------------------------------------------------------------------------------------
// Structs
//--------------------------------------------------------------------------------------
struct PSIn
{
	float4	Pos		: SV_POSITION;
	float4	CSPos	: POSCURRENT;
	float4	TSPos 	: POSHISTORY;
	float3	Norm	: NORMAL;
	float2	UV		: TEXCOORD;
	float4	Color	: COLOR;
};

struct PSOut
{
	min16float4 BaseColor	: SV_TARGET0;
	min16float4 Normal		: SV_TARGET1;
	min16float Roughness	: SV_TARGET2;
	min16float2 Velocity	: SV_TARGET3;
};

//--------------------------------------------------------------------------------------
// Constant buffer
//--------------------------------------------------------------------------------------
cbuffer cbPerObject
{
	uint g_instanceIdx;
};

//--------------------------------------------------------------------------------------
// Base geometry-buffer pass
//--------------------------------------------------------------------------------------
PSOut main(PSIn input)
{
	PSOut output;

	const float2 csPos = input.CSPos.xy / input.CSPos.w;
	const float2 tsPos = input.TSPos.xy / input.TSPos.w;
	const min16float2 velocity = min16float2(csPos - tsPos) * min16float2(0.5, -0.5);
	output.BaseColor = min16float4(input.Color);
	output.Normal = min16float4(normalize(input.Norm) * 0.5 + 0.5, (g_instanceIdx + 1) / 2.0);
	output.Roughness = getRoughness(g_instanceIdx, input.UV);
	output.Velocity = velocity;

	return output;
}
