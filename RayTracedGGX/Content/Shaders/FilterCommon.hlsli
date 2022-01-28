//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

static const float3 g_lumBase = { 0.25, 0.5, 0.25 };
static const float g_gammaRefl = 0.15;
static const float g_gammaDiff = 0.15;

//--------------------------------------------------------------------------------------
// A fast invertible tone map that preserves color (Reinhard)
//--------------------------------------------------------------------------------------
float3 TM(float3 hdr)
{
	const float3 rgb = float3(hdr);

	return rgb / (1.0 + dot(rgb, g_lumBase));
}

//--------------------------------------------------------------------------------------
// Inverse of preceding function
//--------------------------------------------------------------------------------------
float3 ITM(float3 rgb)
{
	return rgb / (1.0 - dot(rgb, g_lumBase));
}

float RadianceWeight(float3 radianceC, float3 radiance)
{
	return max(exp(-0.65 * length(radianceC - radiance)), 1.0e-2);
}

float NormalWeight(float3 normC, float3 norm, float sigma)
{
	return pow(max(dot(normC, norm), 0.0), sigma);
}

float DepthWeight(float depthC, float depth, float sigma)
{
	return exp(-abs(depthC - depth) * depthC * sigma);
}

float RoughnessWeight(float roughC, float rough, float sigmaMin, float sigmaMax)
{
	return 1.0 - smoothstep(sigmaMin, sigmaMax, abs(rough - roughC));
}

float RoughnessSigma(float roughness)
{
	return 320.0 * roughness * roughness;
}

float Gaussian(float x, float m, float sigma)
{
	const float r = x - m;
	const float a = r * r / (sigma * sigma);

	return exp(-0.5 * a);
}

float3 Denoise(float3 src, float3 mu, float roughness)
{
	const float t = roughness;
	const float b = 1.0 - roughness * 2.0;
	float3 d = TM(src) - mu;
	d = max(abs(d) - t, 0.0) * sign(d);

	return mu + b * d;
}

//--------------------------------------------------------------------------------------
// Clip color
//--------------------------------------------------------------------------------------
float3 clipColor(float3 color, float3 minColor, float3 maxColor)
{
	const float3 cent = 0.5 * (maxColor + minColor);
	const float3 dist = 0.5 * (maxColor - minColor);

	const float3 disp = color - cent;
	const float3 dir = abs(disp / dist);
	const float maxComp = max(dir.x, max(dir.y, dir.z));

	if (maxComp > 1.0) return cent + disp / maxComp;
	else return color;
}