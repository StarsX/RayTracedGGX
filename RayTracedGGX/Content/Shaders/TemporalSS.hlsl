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
	float4 history = g_historyImage.SampleLevel(g_sampler, tex, 0);

	const float historyFrames = history.w * 255.0;
	const float totalFrames = historyFrames + 1.0;
	float3 result = historyFrames < 255.0 ?
		(history.xyz * historyFrames + current.xyz) / totalFrames :
		history.xyz;

	//history.w = history.w > 0.0 ? history.w / (history.w + 1.0) : 1.0;

	//const float blend = history.w <= 1.0 / 24.0 ? 0.0 : history.w;
	//history.w = history.w <= 1.0 / 24.0 ? 1.0 / 24.0 : history.w;

	RenderTarget[DTid] = float4(result, totalFrames / 255.0);
	//RenderTarget[DTid] = float4(lerp(history.xyz, current.xyz, blend), history.w);
}
