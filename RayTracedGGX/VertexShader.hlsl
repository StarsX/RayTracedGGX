struct PSInput
{
	float4 position : SV_POSITION;
	float2 uv : TEXCOORD;
	float4 color : COLOR;
};

cbuffer SceneConstantBuffer : register(b0)
{
	float4 offset;
};

PSInput main(float4 position : POSITION, float2 uv : TEXCOORD, uint iid : SV_InstanceID)
{
	PSInput result;

	result.position = position + offset + float4(0.0, 0.25 - iid * 0.5, 0.9 + iid * 0.05, 0.0);
	result.uv = uv;
	result.color = iid ? float4(1.0, 0.0.xx, 1.0) : float4(0.0.xx, 1.0, 1.0);

	return result;
}
