#define NUM_TEXTURES	8

struct PSInput
{
	float4 position : SV_POSITION;
	float2 uv : TEXCOORD;
	float4 color : COLOR;
};

Texture2D g_textures[NUM_TEXTURES];
RWTexture2D<float4> g_rwTexture;
SamplerState g_sampler;

[earlydepthstencil]
void main(PSInput input)
{
	float4 color = 0.0;

	[unroll]
	for (uint i = 0; i < NUM_TEXTURES; ++i)
		color += g_textures[i].Sample(g_sampler, input.uv);
	
	color *= input.color / NUM_TEXTURES;

	g_rwTexture[input.position.xy] = color;
}
