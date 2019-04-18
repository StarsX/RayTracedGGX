//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

//--------------------------------------------------------------------------------------
// Definitions
//--------------------------------------------------------------------------------------
#define	_VARIANCE_AABB_		1

#define	NUM_NEIGHBORS		8
#define	NUM_SAMPLES			(NUM_NEIGHBORS + 1)
#define	NUM_NEIGHBORS_H		4

#define GET_LUMA(v)			dot(v, g_lumBase)

//--------------------------------------------------------------------------------------
// Constants
//--------------------------------------------------------------------------------------
static const min16float3 g_lumBase = { 0.25, 0.5, 0.25 };
static int2 g_texOffsets[] =
{
	int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1),
	int2(-1, -1), int2(1, -1), int2(1, 1), int2(-1, 1)
};

//--------------------------------------------------------------------------------------
// Texture and buffers
//--------------------------------------------------------------------------------------
RWTexture2D<float4>	RenderTarget;
Texture2DArray		g_txCurrent;
Texture2D			g_txHistory;
Texture2D<float2>	g_velocity;

//--------------------------------------------------------------------------------------
// Samplers
//--------------------------------------------------------------------------------------
SamplerState g_sampler;

//--------------------------------------------------------------------------------------
// Maxinum velocity of 3x3
//--------------------------------------------------------------------------------------
min16float4 VelocityMax(int2 tex)
{
	min16float4 velocity = min16float2(g_velocity[tex]).xyxy;
	min16float speedSq = dot(velocity.xy, velocity.xy);

	min16float2 velocities[NUM_NEIGHBORS_H];
	[unroll]
	for (uint i = 0; i < NUM_NEIGHBORS_H; ++i)
		velocities[i] = min16float2(g_velocity[tex + g_texOffsets[i + NUM_NEIGHBORS_H]]);

	//[unroll]
	for (i = 0; i < NUM_NEIGHBORS_H; ++i)
	{
		const min16float speedSqN = dot(velocities[i], velocities[i]);
		if (speedSqN > speedSq)
		{
			velocity.xy = velocities[i];
			speedSq = speedSqN;
		}
	}

	return velocity;
}

//--------------------------------------------------------------------------------------
// Minimum and maxinum of the neighbor samples, returning Gaussian blurred color
//--------------------------------------------------------------------------------------
min16float4 NeighborMinMax(out min16float4 neighborMin, out min16float4 neighborMax,
	min16float4 center, int2 tex, min16float gamma = 1.0)
{
	static min16float weights[] =
	{
		0.5, 0.5, 0.5, 0.5,
		0.25, 0.25, 0.25, 0.25
	};

	min16float4 neighbors[NUM_NEIGHBORS];
	[unroll]
	for (uint i = 0; i < NUM_NEIGHBORS; ++i)
		neighbors[i] = min16float4(g_txCurrent[uint3(tex + g_texOffsets[i], 1)]);

	min16float4 gaussian = center;

#if	_VARIANCE_AABB_
#define m1	mu
	min16float3 mu = center.xyz;
	min16float3 m2 = m1 * m1;
#else
	neighborMin.xyz = neighborMax.xyz = mu;
	neighborMin.xyz = min(current, neighborMin.xyz);
	neighborMax.xyz = max(current, neighborMax.xyz);
#endif

	//[unroll]
	for (i = 0; i < NUM_NEIGHBORS; ++i)
	{
		gaussian += neighbors[i] * weights[i];

#if	_VARIANCE_AABB_
		m1 += neighbors[i].xyz;
		m2 += neighbors[i].xyz * neighbors[i].xyz;
#else
		neighborMin.xyz = min(neighbors[i], neighborMin.xyz);
		neighborMax.xyz = max(neighbors[i], neighborMax.xyz);
#endif
	}

#if	_VARIANCE_AABB_
	mu /= NUM_SAMPLES;
	const min16float3 sigma = sqrt(abs(m2 / NUM_SAMPLES - mu * mu));
	const min16float3 gsigma = gamma * sigma;
	neighborMin.xyz = mu - gsigma;
	neighborMax.xyz = mu + gsigma;
	neighborMin.w = GET_LUMA(mu - sigma);
	neighborMax.w = GET_LUMA(mu + sigma);
#else
	neighborMin.w = GET_LUMA(neighborMin.xyz);
	neighborMax.w = GET_LUMA(neighborMax.xyz);
#endif

	gaussian /= 4.0;

	return gaussian;
}

//--------------------------------------------------------------------------------------
// Clip color
//--------------------------------------------------------------------------------------
min16float3 clipColor(min16float3 color, min16float3 minColor, min16float3 maxColor)
{
	const min16float3 cent = 0.5 * (maxColor + minColor);
	const min16float3 dist = 0.5 * (maxColor - minColor);

	const min16float3 disp = color - cent;
	const min16float3 dir = abs(disp / dist);
	const min16float maxComp = max(dir.x, max(dir.y, dir.z));

	if (maxComp > 1.0) return cent + disp / maxComp;
	else return color;
}

[numthreads(8, 8, 1)]
void main(uint2 DTid : SV_DispatchThreadID)
{
	float2 texSize;
	g_txHistory.GetDimensions(texSize.x, texSize.y);
	const float2 tex = (DTid + 0.5) / texSize;

	const min16float4 current = min16float4(g_txCurrent[uint3(DTid, 0)]);
	const min16float4 curCent = min16float4(g_txCurrent[uint3(DTid, 1)]);	// Centroid sample for color clipping
	const min16float4 velocity = VelocityMax(DTid);
	const float2 texBack = tex - velocity.xy;
	min16float4 history = min16float4(g_txHistory.SampleLevel(g_sampler, texBack, 0));
	history.xyz *= history.xyz;

	const min16float speed = abs(velocity.x) + abs(velocity.y);

	min16float4 neighborMin, neighborMax;
	min16float4 filtered = NeighborMinMax(neighborMin, neighborMax, curCent, DTid);
	//filtered.xyz = lerp(current.xyz, filtered.xyz, saturate(speed * 32.0));

	history.xyz = clipColor(history.xyz, neighborMin.xyz, neighborMax.xyz);

	/*min16float currentWeight = 1.0 - history.w;
	currentWeight /= currentWeight + 1.0;
	//currentWeight = saturate(currentWeight + speed * 32.0);
	history.w = 1.0 - currentWeight;*/
	history.w = speed > 0.0 ? 0.125 : history.w;
	const min16float alpha = history.w + 1.0 / 255.0;
	min16float blend = history.w < 1.0 ? history.w / alpha : 1.0;
	blend = filtered.w > 0.0 ? blend : 0.0;
	
	const min16float3 result = lerp(current.xyz, history.xyz, blend);

	RenderTarget[DTid] = min16float4(sqrt(result), alpha);
}
