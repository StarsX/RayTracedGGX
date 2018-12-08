//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

#define PI 3.1415926535897

// GGX / Trowbridge-Reitz
// [Walter et al. 2007, "Microfacet models for refraction through rough surfaces"]
float D_GGX(float Roughness, float NoH)
{
	float m = Roughness * Roughness;
	float m2 = m * m;
	float d = (NoH * m2 - NoH) * NoH + 1;	// 2 mad

	return m2 / (PI * d * d);				// 4 mul, 1 rcp
}

// [Schlick 1994, "An Inexpensive BRDF Model for Physically-Based Rendering"]
float Vis_Schlick(float Roughness, float NoV, float NoL)
{
	float k = Roughness * Roughness * 0.5;
	float Vis_SchlickV = NoV * (1 - k) + k;
	float Vis_SchlickL = NoL * (1 - k) + k;

	return 0.25 / (Vis_SchlickV * Vis_SchlickL);
}

// Smith term for GGX
// [Smith 1967, "Geometrical shadowing of a random rough surface"]
float Vis_Smith(float Roughness, float NoV, float NoL)
{
	float a = Roughness * Roughness;
	float a2 = a * a;

	float Vis_SmithV = NoV + sqrt(NoV * (NoV - NoV * a2) + a2);
	float Vis_SmithL = NoL + sqrt(NoL * (NoL - NoL * a2) + a2);

	return rcp(Vis_SmithV * Vis_SmithL);
}

// Appoximation of joint Smith term for GGX
// [Heitz 2014, "Understanding the Masking-Shadowing Function in Microfacet-Based BRDFs"]
float Vis_SmithJointApprox(float Roughness, float NoV, float NoL)
{
	float a = Roughness * Roughness;
	float Vis_SmithV = NoL * (NoV * (1 - a) + a);
	float Vis_SmithL = NoV * (NoL * (1 - a) + a);

	return 0.5 * rcp(Vis_SmithV + Vis_SmithL);
}

// [Schlick 1994, "An Inexpensive BRDF Model for Physically-Based Rendering"]
// [Lagarde 2012, "Spherical Gaussian approximation for Blinn-Phong, Phong and Fresnel"]
float3 F_Schlick(float3 SpecularColor, float VoH)
{
	float Fc = pow(1.0 - VoH, 5.0);							// 1 sub, 3 mul
	//float Fc = exp2( (-5.55473 * VoH - 6.98316) * VoH );	// 1 mad, 1 mul, 1 exp
	//return Fc + (1 - Fc) * SpecularColor;					// 1 add, 3 mad

	// Anything less than 2% is physically impossible and is instead considered to be shadowing
	//return saturate(50.0 * SpecularColor.g) * Fc + (1.0 - Fc) * SpecularColor;
	return Fc + (1.0 - Fc) * SpecularColor;
}
