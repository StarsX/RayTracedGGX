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
};

//--------------------------------------------------------------------------------------
// Base geometry-buffer pass
//--------------------------------------------------------------------------------------
min16float4 main(PSIn input) : SV_TARGET
{
	const float2 csPos = input.CSPos.xy / input.CSPos.w;
	const float2 tsPos = input.TSPos.xy / input.TSPos.w;
	const min16float2 velocity = min16float2(csPos - tsPos) * min16float2(0.5, -0.5);
	const min16float4 result = min16float4(velocity, 0.0.xx);

	return result;
}
