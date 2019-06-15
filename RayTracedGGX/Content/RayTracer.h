//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

#pragma once

#include "Core/XUSG.h"
#include "RayTracing/XUSGRayTracing.h"

class RayTracer
{
public:
	enum MeshIndex : uint8_t
	{
		GROUND,
		MODEL_OBJ,

		NUM_MESH
	};

	enum RayTracingPipeline : uint8_t
	{
		TEST,
		GGX,

		NUM_RAYTRACE_PIPELINE
	};

	RayTracer(const XUSG::RayTracing::Device &device);
	virtual ~RayTracer();

	bool Init(const XUSG::RayTracing::CommandList &commandList, uint32_t width,
		uint32_t height, XUSG::Resource *vbUploads, XUSG::Resource *ibUploads,
		XUSG::RayTracing::Geometry *geometries, const char *fileName, XUSG::Format rtFormat,
		const DirectX::XMFLOAT4 &posScale = DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f));
	void SetPipeline(RayTracingPipeline pipeline);
	void UpdateFrame(uint32_t frameIndex, DirectX::CXMVECTOR eyePt, DirectX::CXMMATRIX viewProj, bool isPaused);
	void Render(const XUSG::RayTracing::CommandList &commandList, uint32_t frameIndex);
	void ToneMap(const XUSG::RayTracing::CommandList &commandList, const XUSG::RenderTargetTable &rtvTable,
		uint32_t numBarriers, XUSG::ResourceBarrier *pBarriers);
	void ClearHistory(const XUSG::RayTracing::CommandList &commandList);

	static const uint32_t FrameCount = 3;

protected:
	enum PipelineLayoutIndex : uint8_t
	{
		GLOBAL_LAYOUT,
		RAY_GEN_LAYOUT,
		HIT_GROUP_LAYOUT,
		GBUFFER_PASS_LAYOUT,
		RESAMPLE_LAYOUT,
		TEMPORAL_SS_LAYOUT,
		TONE_MAP_LAYOUT,

		NUM_PIPELINE_LAYOUT
	};

	enum GlobalPipelineLayoutSlot : uint8_t
	{
		OUTPUT_VIEW,
		SHADER_RESOURCES,
		ACCELERATION_STRUCTURE = SHADER_RESOURCES,
		SAMPLER,
		INDEX_BUFFERS,
		VERTEX_BUFFERS
	};

	enum PipelineIndex : uint8_t
	{
		GBUFFER_PASS,
		SPATIAL_PASS,
		TEMPORAL_SS,
		TEMPORAL_AA,
		TONE_MAP,

		NUM_PIPELINE
	};

	enum SRVTable : uint8_t
	{
		SRV_TABLE_IB,
		SRV_TABLE_VB,
		SRV_TABLE_SPATIAL,
		SRV_TABLE_SPATIAL1,
		SRV_TABLE_TS,
		SRV_TABLE_TS1,
		SRV_TABLE_TM,
		SRV_TABLE_TM1,

		NUM_SRV_TABLE
	};

	enum UAVTable : uint8_t
	{
		UAV_TABLE_OUTPUT,
		UAV_TABLE_SPATIAL,
		UAV_TABLE_SPATIAL1,
		UAV_TABLE_TSAMP,
		UAV_TABLE_TSAMP1,

		NUM_UAV_TABLE
	};

	struct RayGenConstants
	{
		DirectX::XMMATRIX	ProjToWorld;
		DirectX::XMVECTOR	EyePt;
		DirectX::XMFLOAT2	Jitter;
	};

	struct HitGroupConstants
	{
		DirectX::XMMATRIX	Normal;
		uint32_t			FrameIndex;
	};

	struct BasePassConstants
	{
		DirectX::XMFLOAT4X4	WorldViewProj;
		DirectX::XMFLOAT4X4	WorldViewProjPrev;
	};

	bool createVB(const XUSG::RayTracing::CommandList &commandList, uint32_t numVert,
		uint32_t stride, const uint8_t *pData, XUSG::Resource &vbUpload);
	bool createIB(const XUSG::RayTracing::CommandList &commandList, uint32_t numIndices,
		const uint32_t *pData, XUSG::Resource &ibUpload);
	bool createGroundMesh(const XUSG::RayTracing::CommandList &commandList,
		XUSG::Resource &vbUpload, XUSG::Resource &ibUpload);
	bool createInputLayout();
	bool createPipelineLayouts();
	bool createPipelines(XUSG::Format rtFormat);
	bool createDescriptorTables();
	bool buildAccelerationStructures(const XUSG::RayTracing::CommandList &commandList,
		XUSG::RayTracing::Geometry *geometries);
	bool buildShaderTables();

	void updateAccelerationStructures(const XUSG::RayTracing::CommandList &commandList, uint32_t frameIndex);
	void rayTrace(const XUSG::RayTracing::CommandList &commandList, uint32_t frameIndex);
	void gbufferPass(const XUSG::RayTracing::CommandList &commandList);
	void spatialPass(const XUSG::RayTracing::CommandList &commandList, uint8_t dst, uint8_t src, uint8_t srcSRV);
	void temporalSS(const XUSG::RayTracing::CommandList &commandList);

	XUSG::RayTracing::Device m_device;

	uint32_t	m_numIndices[NUM_MESH];
	uint8_t		m_frameParity;

	DirectX::XMUINT2	m_viewport;
	DirectX::XMFLOAT4	m_posScale;
	DirectX::XMFLOAT4X4 m_worlds[NUM_MESH];
	BasePassConstants	m_cbBasePass[NUM_MESH];

	static const uint32_t NumUAVs = NUM_MESH + 1 + NUM_UAV_TABLE;
	XUSG::RayTracing::BottomLevelAS m_bottomLevelASs[NUM_MESH];
	XUSG::RayTracing::TopLevelAS m_topLevelAS;

	XUSG::InputLayout			m_inputLayout;
	XUSG::PipelineLayout		m_pipelineLayouts[NUM_PIPELINE_LAYOUT];
	XUSG::RayTracing::Pipeline	m_rayTracingPipelines[NUM_RAYTRACE_PIPELINE];
	XUSG::Pipeline				m_pipelines[NUM_PIPELINE];

	XUSG::DescriptorTable	m_srvTables[NUM_SRV_TABLE];
	XUSG::DescriptorTable	m_uavTables[NUM_UAV_TABLE];
	XUSG::DescriptorTable	m_samplerTable;
	XUSG::RenderTargetTable	m_rtvTable;

	XUSG::VertexBuffer		m_vertexBuffers[NUM_MESH];
	XUSG::IndexBuffer		m_indexBuffers[NUM_MESH];

	XUSG::Texture2D			m_outputViews[NUM_UAV_TABLE];
	XUSG::RenderTarget		m_velocity;
	XUSG::DepthStencil		m_depth;

	XUSG::Resource			m_scratch;
	XUSG::Resource			m_instances[FrameCount];

	RayTracingPipeline		m_pipeIndex;

	// Shader tables
	static const wchar_t *HitGroupName;
	static const wchar_t *RaygenShaderName;
	static const wchar_t *ClosestHitShaderName;
	static const wchar_t *MissShaderName;
	XUSG::RayTracing::ShaderTable	m_missShaderTables[NUM_RAYTRACE_PIPELINE];
	XUSG::RayTracing::ShaderTable	m_hitGroupShaderTables[FrameCount][NUM_RAYTRACE_PIPELINE];
	XUSG::RayTracing::ShaderTable	m_rayGenShaderTables[FrameCount][NUM_RAYTRACE_PIPELINE];

	XUSG::ShaderPool				m_shaderPool;
	XUSG::RayTracing::PipelineCache	m_rayTracingPipelineCache;
	XUSG::Graphics::PipelineCache	m_graphicsPipelineCache;
	XUSG::Compute::PipelineCache	m_computePipelineCache;
	XUSG::PipelineLayoutCache		m_pipelineLayoutCache;
	XUSG::DescriptorTableCache		m_descriptorTableCache;
};
