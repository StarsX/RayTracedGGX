//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

//--------------------------------------------------------------------------------------
// Structs
//--------------------------------------------------------------------------------------
struct HitGroupConstants
{
	matrix Normal;
	float2 Hammersley;
};

//--------------------------------------------------------------------------------------
// Constant buffers
//--------------------------------------------------------------------------------------
ConstantBuffer<HitGroupConstants> l_hitGroupCB : register(b1);

//--------------------------------------------------------------------------------------
// Compute direction in local space
//--------------------------------------------------------------------------------------
float3 computeLocalDirectionGGX(float a, bool isCentroidSample)
{
	const float2 xi = isCentroidSample ? 0.0 : l_hitGroupCB.Hammersley;
	const float phi = 2.0 * PI * xi.x;

	// Only near the specular direction according to the roughness for importance sampling
	const float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
	const float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

	return float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

//--------------------------------------------------------------------------------------
// Transform local to world space
//--------------------------------------------------------------------------------------
float3x3 computeLocalToWorld(float3 normal)
{
	// Using right-hand coord
	const float3 up = abs(normal.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0.xx);
	const float3 xAxis = normalize(cross(up, normal));
	const float3 yAxis = cross(normal, xAxis);
	const float3 zAxis = normal;

	return float3x3(xAxis, yAxis, zAxis);
}

// Compute local direction first and transform it to world space
float3 computeDirectionGGX(float a, float3 normal, bool isCentroidSample = false)
{
	const float3 localDir = computeLocalDirectionGGX(a, isCentroidSample);
	const float3x3 tanSpace = computeLocalToWorld(normal);

	return tanSpace[0] * localDir.x + tanSpace[1] * localDir.y + tanSpace[2] * localDir.z;
}
