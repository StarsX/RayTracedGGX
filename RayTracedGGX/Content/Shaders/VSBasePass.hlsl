//--------------------------------------------------------------------------------------
// By XU, Tianchen
//--------------------------------------------------------------------------------------

//--------------------------------------------------------------------------------------
// Structs
//--------------------------------------------------------------------------------------
struct VSIn
{
	float3	Pos	: POSITION;
	float3	Nrm	: NORMAL;
};

struct VSOut
{
	float4	Pos		: SV_POSITION;
	float4	CSPos	: POSCURRENT;
	float4	TSPos 	: POSHISTORY;
};

//--------------------------------------------------------------------------------------
// Constant buffer
//--------------------------------------------------------------------------------------
cbuffer cbPerObject
{
	matrix	g_worldViewProj;
	matrix	g_worldViewProjPrev;
};

//--------------------------------------------------------------------------------------
// Base geometry pass
//--------------------------------------------------------------------------------------
VSOut main(VSIn input)
{
	VSOut output;

	const float4 pos = { input.Pos, 1.0 };
	output.Pos = mul(pos, g_worldViewProj);
	output.TSPos = mul(pos, g_worldViewProjPrev);
	output.CSPos = output.Pos;

	return output;
}
