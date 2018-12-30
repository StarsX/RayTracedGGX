//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

//--------------------------------------------------------------------------------------
// Texture and buffers
//--------------------------------------------------------------------------------------
RWTexture2D<float4> RenderTarget;
Texture2DArray g_currentImage	: register(t0);
Texture2D g_historyImage		: register(t1);
Texture2D<float2> g_velocity	: register(t2);

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

	const float4 current = g_currentImage[uint3(DTid, 0)];
	float4 history = g_historyImage.SampleLevel(g_sampler, tex, 0);

	const float2 velocity = g_velocity[DTid];
	const float speed = abs(velocity.x) + abs(velocity.y);
	history.w = speed > 0.25 ? 0.0 : history.w;

	const float alpha = history.w + 1.0 / 255.0;
	const float blend = history.w / alpha;
	float3 result = history.w < 1.0 ? lerp(current.xyz, history.xyz, blend) : history.xyz;

	RenderTarget[DTid] = float4(result, alpha);
	//RenderTarget[DTid] = g_currentImage[uint3(DTid, 1)];
	//RenderTarget[DTid] = float4(abs(velocity.x) > 1e-5 ? 1.0 : 0.0, abs(velocity.y) > 1e-5 ? 1.0 : 0.0, 0.0, alpha);
}
