//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

#pragma once

#include "Core/XUSG.h"
#include "RayTracing/XUSGRayTracing.h"

class RayTracer
{
public:
	enum MeshIndex
	{
		MODEL_OBJ,
		GROUND,

		NUM_MESH
	};

	RayTracer(const XUSG::RayTracing::Device &device, const XUSG::RayTracing::CommandList &commandList);
	virtual ~RayTracer();

	bool Init(uint32_t width, uint32_t height, XUSG::Resource *vbUploads, XUSG::Resource *ibUploads,
		const char *fileName, const DirectX::XMFLOAT4 &posScale = DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f));
	void UpdateFrame(uint32_t frameIndex, DirectX::CXMVECTOR eyePt, DirectX::CXMMATRIX viewProj);
	void Render(uint32_t frameIndex, const XUSG::Descriptor &dsv);

	const XUSG::Texture2D &GetOutputView(uint32_t frameIndex, XUSG::ResourceState dstState = XUSG::ResourceState(0));

	static const uint32_t FrameCount = 3;

protected:
	enum PipelineLayoutIndex
	{
		GLOBAL_LAYOUT,
		RAY_GEN_LAYOUT,
		//HIT_LAYOUT,

		NUM_PIPELINE_LAYOUT
	};

	enum GlobalPipelineLayoutSlot
	{
		OUTPUT_VIEW,
		ACCELERATION_STRUCTURE,
		SAMPLER,
		INDEX_BUFFERS,
		VERTEX_BUFFERS,
		SCENE_CONSTANTS
	};

	enum PipelineIndex
	{
		TEST,

		NUM_PIPELINE_INDEX
	};

	enum SRVTable
	{
		SRV_TABLE_AS,
		SRV_TABLE_IB,
		SRV_TABLE_VB,

		NUM_SRV_TABLE
	};

	enum UAVTable
	{
		UAV_TABLE_OUTPUT,

		NUM_UAV_TABLE
	};

	struct RayGenConstants
	{
		DirectX::XMFLOAT4X4	ProjToWorld;
		DirectX::XMFLOAT3	EyePt;
	};

	bool createVB(uint32_t numVert, uint32_t stride, const uint8_t *pData, XUSG::Resource &vbUpload);
	bool createIB(uint32_t numIndices, const uint32_t *pData, XUSG::Resource &ibUpload);
	bool createGroundMesh(XUSG::Resource &vbUpload, XUSG::Resource &ibUpload);

	void createPipelineLayouts();
	bool createPipeline();
	void createDescriptorTables();

	bool buildAccelerationStructures();
	void buildShaderTables();
	void updateAccelerationStructures();
	void rayTrace(uint32_t frameIndex);

	XUSG::RayTracing::Device m_device;
	XUSG::RayTracing::CommandList m_commandList;

	DirectX::XMUINT2	m_viewport;
	DirectX::XMFLOAT4	m_posScale;
	DirectX::XMFLOAT4X4	m_rot;
	RayGenConstants		m_cbRayGens[FrameCount];

	static const uint32_t NumUAVs = FrameCount + NUM_MESH + 1;
	XUSG::RayTracing::BottomLevelAS m_bottomLevelASs[NUM_MESH];
	XUSG::RayTracing::TopLevelAS m_topLevelAS;

	XUSG::Blob m_shaderLib;
	XUSG::PipelineLayout m_pipelineLayouts[NUM_PIPELINE_LAYOUT];
	XUSG::RayTracing::Pipeline m_pipelines[NUM_PIPELINE_INDEX];

	XUSG::DescriptorTable	m_srvTables[NUM_SRV_TABLE];
	XUSG::DescriptorTable	m_uavTables[FrameCount][NUM_UAV_TABLE];
	XUSG::DescriptorTable	m_samplerTable;

	XUSG::VertexBuffer		m_vertexBuffers[NUM_MESH];
	XUSG::IndexBuffer		m_indexBuffers[NUM_MESH];

	XUSG::Texture2D			m_outputViews[FrameCount];

	XUSG::Resource			m_scratch;
	XUSG::Resource			m_instances;

	// Shader tables
	static const wchar_t *HitGroupName;
	static const wchar_t *RaygenShaderName;
	static const wchar_t *ClosestHitShaderName;
	static const wchar_t *MissShaderName;
	XUSG::RayTracing::ShaderTable	m_missShaderTable;
	XUSG::RayTracing::ShaderTable	m_hitGroupShaderTable;
	XUSG::RayTracing::ShaderTable	m_rayGenShaderTables[FrameCount];

	XUSG::RayTracing::PipelineCache	m_pipelineCache;
	XUSG::PipelineLayoutCache		m_pipelineLayoutCache;
	XUSG::DescriptorTableCache		m_descriptorTableCache;
};
