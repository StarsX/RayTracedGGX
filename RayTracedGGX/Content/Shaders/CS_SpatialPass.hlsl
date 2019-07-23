//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

//--------------------------------------------------------------------------------------
// Texture and buffers
//--------------------------------------------------------------------------------------
RWTexture2D<float4>	RenderTarget;
Texture2D			g_txSource;

//--------------------------------------------------------------------------------------
// Samplers
//--------------------------------------------------------------------------------------
SamplerState g_sampler;

[numthreads(8, 8, 1)]
void main(uint2 DTid : SV_DispatchThreadID)
{
	float2 texSize;
	g_txSource.GetDimensions(texSize.x, texSize.y);
	const float2 tex00 = DTid / texSize;
	const float2 tex01 = float2(DTid.x + 1, DTid.y) / texSize;
	const float2 tex10 = float2(DTid.x, DTid.y + 1) / texSize;
	const float2 tex11 = float2(DTid.x + 1, DTid.y + 1) / texSize;

	float4 result = g_txSource.SampleLevel(g_sampler, tex00, 0);
	result += g_txSource.SampleLevel(g_sampler, tex01, 0);
	result += g_txSource.SampleLevel(g_sampler, tex10, 0);
	result += g_txSource.SampleLevel(g_sampler, tex11, 0);

	RenderTarget[DTid] = result / 4.0;
}
