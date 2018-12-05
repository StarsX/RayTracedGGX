//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

#pragma once

class ObjLoader
{
public:
	struct float3
	{
		float x;
		float y;
		float z;

		float3() = default;
		constexpr float3(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}
		explicit float3(const float *pArray) : x(pArray[0]), y(pArray[1]), z(pArray[2]) {}

		float3 &operator= (const float3& Float3) { x = Float3.x; y = Float3.y; z = Float3.z; return *this; }
	};

	struct Vertex
	{
		float3	m_vPosition;
		float3	m_vNormal;
	};

	using vVertex	= std::vector<Vertex>;
	using vuint		= std::vector<uint32_t>;

	ObjLoader();
	virtual ~ObjLoader();

	bool Import(const char *pszFilename, const bool bRecomputeNorm = true, const bool bNeedBound = true);

	const uint32_t GetNumVertices() const;
	const uint32_t GetNumIndices() const;
	const uint32_t GetVertexStride() const;
	const uint8_t *GetVertices() const;
	const uint32_t *GetIndices() const;

	const float3& GetCenter() const;
	const float GetRadius() const;

protected:
	void importGeometryFirstPass(FILE *pFile);
	void importGeometrySecondPass(FILE *pFile);
	void loadIndex(FILE *pFile, uint32_t &uNumTri);
	void computeNormal();
	void computeBound();

	vVertex		m_vVertices;
	vuint		m_vIndices;
	vuint		m_vTIndices;
	vuint		m_vNIndices;

	float3		m_vCenter;
	float		m_fRadius;
};
