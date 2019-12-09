//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#define PI 3.1415926535897

// GGX / Trowbridge-Reitz
// [Walter et al. 2007, "Microfacet models for refraction through rough surfaces"]
float D_GGX(float roughness, float NoH)
{
	const float m = roughness * roughness;
	const float m2 = m * m;
	const float d = (NoH * m2 - NoH) * NoH + 1;	// 2 mad

	return m2 / (PI * d * d);					// 4 mul, 1 rcp
}

// [Schlick 1994, "An Inexpensive BRDF Model for Physically-Based Rendering"]
float Vis_Schlick(float roughness, float NoV, float NoL)
{
	const float k = roughness * roughness * 0.5;
	const float vis_SchlickV = NoV * (1 - k) + k;
	const float vis_SchlickL = NoL * (1 - k) + k;

	return 0.25 / (vis_SchlickV * vis_SchlickL);
}

// Smith term for GGX
// [Smith 1967, "Geometrical shadowing of a random rough surface"]
float Vis_Smith(float roughness, float NoV, float NoL)
{
	const float a = roughness * roughness;
	const float a2 = a * a;

	const float vis_SmithV = NoV + sqrt(NoV * (NoV - NoV * a2) + a2);
	const float vis_SmithL = NoL + sqrt(NoL * (NoL - NoL * a2) + a2);

	return 1.0 / (vis_SmithV * vis_SmithL);
}

// Appoximation of joint Smith term for GGX
// [Heitz 2014, "Understanding the Masking-Shadowing Function in Microfacet-Based BRDFs"]
float Vis_SmithJointApprox(float roughness, float NoV, float NoL)
{
	const float a = roughness * roughness;
	const float vis_SmithV = NoL * (NoV * (1 - a) + a);
	const float vis_SmithL = NoV * (NoL * (1 - a) + a);

	return 0.5 / (vis_SmithV + vis_SmithL);
}

// [Schlick 1994, "An Inexpensive BRDF Model for Physically-Based Rendering"]
// [Lagarde 2012, "Spherical Gaussian approximation for Blinn-Phong, Phong and Fresnel"]
float3 F_Schlick(float3 specularColor, float VoH)
{
	const float fc = pow(1.0 - VoH, 5.0);					// 1 sub, 3 mul
	//float fc = exp2( (-5.55473 * VoH - 6.98316) * VoH );	// 1 mad, 1 mul, 1 exp
	//return fc + (1 - fc) * specularColor;					// 1 add, 3 mad

	// Anything less than 2% is physically impossible and is instead considered to be shadowing
	return saturate(50.0 * specularColor.g) * fc + (1.0 - fc) * specularColor;
}

float3 EnvBRDFApprox(float3 SpecularColor, float Roughness, float NoV)
{
	// [ Lazarov 2013, "Getting More Physical in Call of Duty: Black Ops II" ]
	// Adaptation to fit our G term.
	const float4 c0 = { -1.0, -0.0275, -0.572, 0.022 };
	const float4 c1 = { 1.0, 0.0425, 1.04, -0.04 };
	float4 r = Roughness * c0 + c1;
	float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
	float2 AB = float2(-1.04, 1.04) * a004 + r.zw;

	AB.y *= saturate(50.0 * SpecularColor.y);

	return SpecularColor * AB.x + AB.y;
}
