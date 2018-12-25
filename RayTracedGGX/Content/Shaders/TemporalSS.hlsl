//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

//--------------------------------------------------------------------------------------
// Texture and buffers
//--------------------------------------------------------------------------------------
RWTexture2D<float4> RenderTarget;
Texture2D g_currentImage;
Texture2D g_historyImage;

//--------------------------------------------------------------------------------------
// Samplers
//--------------------------------------------------------------------------------------
SamplerState g_sampler;

[numthreads(8, 8, 1)]
void main(uint2 DTid : SV_DispatchThreadID)
{
	float2 texSize;
	g_historyImage.GetDimensions(texSize.x, texSize.y);
	const float2 tex = (DTid + 0.5) / texSize;

	const float4 current = g_currentImage[DTid];
	const float4 history = g_historyImage.SampleLevel(g_sampler, tex, 0);

	const float blend = history.w / (history.w + 1.0);

	RenderTarget[DTid] = lerp(history, current, blend);
}
