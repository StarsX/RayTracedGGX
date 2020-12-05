//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
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

	RayTracer(const XUSG::RayTracing::Device& device);
	virtual ~RayTracer();

	bool Init(XUSG::RayTracing::CommandList* pCommandList, uint32_t width, uint32_t height,
		std::vector<XUSG::Resource>& uploaders, XUSG::RayTracing::Geometry* geometries,
		const char* fileName, const wchar_t* envFileName, XUSG::Format rtFormat,
		const DirectX::XMFLOAT4& posScale = DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f));
	void UpdateFrame(uint32_t frameIndex, DirectX::CXMVECTOR eyePt, DirectX::CXMMATRIX viewProj, float timeStep);
	void Render(const XUSG::RayTracing::CommandList* pCommandList, uint32_t frameIndex, bool sharedMemVariance = false);
	void ToneMap(const XUSG::RayTracing::CommandList* pCommandList, const XUSG::Descriptor& rtv,
		uint32_t numBarriers, XUSG::ResourceBarrier* pBarriers);

	const XUSG::Texture2D::sptr& GetRayTracingOutput() const;
	const XUSG::RenderTarget::uptr* GetGBuffers() const;
	const XUSG::DepthStencil::sptr GetDepth() const;

	static const uint32_t FrameCount = 3;

protected:
	enum PipelineLayoutIndex : uint8_t
	{
		GLOBAL_LAYOUT,
		RAY_GEN_LAYOUT,
		GBUFFER_PASS_LAYOUT,
		VARIANCE_H_LAYOUT,
		VARIANCE_V_LAYOUT,
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
		VERTEX_BUFFERS,
		CONSTANTS,
		G_BUFFERS
	};

	enum PipelineIndex : uint8_t
	{
		GBUFFER_PASS,
		VARIANCE_H_PASS,
		VARIANCE_V_PASS,
		VARIANCE_H_FAST,
		VARIANCE_V_FAST,
		TEMPORAL_SS,
		TONE_MAP,

		NUM_PIPELINE
	};

	enum GBuffer : uint8_t
	{
		NORMAL,
		ROUGHNESS,
		VELOCITY,

		NUM_GBUFFER
	};

	enum UAVResource : uint8_t
	{
		UAV_RT_OUT,
		UAV_AVG_H,
		UAV_FLT,
		UAV_TSS,
		UAV_TSS1,

		NUM_UAV
	};

	enum SRVTable : uint8_t
	{
		SRV_TABLE_IB,
		SRV_TABLE_VB,
		SRV_TABLE_GB,
		SRV_TABLE_VAR,
		SRV_TABLE_VAR1,
		SRV_TABLE_TSS,
		SRV_TABLE_TSS1,
		SRV_TABLE_TM,
		SRV_TABLE_TM1,

		NUM_SRV_TABLE
	};

	enum UAVTable : uint8_t
	{
		UAV_TABLE_RT_OUT,
		UAV_TABLE_VAR_H,
		UAV_TABLE_VAR_H1,
		UAV_TABLE_FLT,
		UAV_TABLE_TSS,
		UAV_TABLE_TSS1,

		NUM_UAV_TABLE
	};

	struct RayGenConstants
	{
		DirectX::XMMATRIX	ProjToWorld;
		DirectX::XMVECTOR	EyePt;
	};

	struct GlobalConstants
	{
		DirectX::XMFLOAT3X4	Normal;
		uint32_t			FrameIndex;
	};

	struct BasePassConstants
	{
		DirectX::XMFLOAT4X4	WorldViewProj;
		DirectX::XMFLOAT4X4	WorldViewProjPrev;
		DirectX::XMFLOAT3X4	Normal;
		DirectX::XMFLOAT2	ProjBias;
	};

	bool createVB(XUSG::RayTracing::CommandList* pCommandList, uint32_t numVert,
		uint32_t stride, const uint8_t* pData, std::vector<XUSG::Resource>& uploaders);
	bool createIB(XUSG::RayTracing::CommandList* pCommandList, uint32_t numIndices,
		const uint32_t* pData, std::vector<XUSG::Resource>& uploaders);
	bool createGroundMesh(XUSG::RayTracing::CommandList* pCommandList,
		std::vector<XUSG::Resource>& uploaders);
	bool createInputLayout();
	bool createPipelineLayouts();
	bool createPipelines(XUSG::Format rtFormat);
	bool createDescriptorTables();
	bool buildAccelerationStructures(const XUSG::RayTracing::CommandList* pCommandList,
		XUSG::RayTracing::Geometry* geometries);
	bool buildShaderTables();

	void updateAccelerationStructures(const XUSG::RayTracing::CommandList* pCommandList, uint32_t frameIndex);
	void rayTrace(const XUSG::RayTracing::CommandList* pCommandList, uint32_t frameIndex);
	void gbufferPass(const XUSG::RayTracing::CommandList* pCommandList);
	void varianceDirect(const XUSG::RayTracing::CommandList* pCommandList);
	void varianceSharedMem(const XUSG::RayTracing::CommandList* pCommandList);
	void temporalSS(const XUSG::RayTracing::CommandList* pCommandList);

	XUSG::RayTracing::Device m_device;

	uint32_t			m_numIndices[NUM_MESH];
	uint8_t				m_frameParity;

	DirectX::XMUINT2	m_viewport;
	DirectX::XMFLOAT4	m_posScale;
	DirectX::XMFLOAT4X4 m_worlds[NUM_MESH];
	BasePassConstants	m_cbBasePass[NUM_MESH];

	XUSG::RayTracing::BottomLevelAS::uptr m_bottomLevelASs[NUM_MESH];
	XUSG::RayTracing::TopLevelAS::uptr m_topLevelAS;

	XUSG::InputLayout			m_inputLayout;
	XUSG::PipelineLayout		m_pipelineLayouts[NUM_PIPELINE_LAYOUT];
	XUSG::RayTracing::Pipeline	m_rayTracingPipeline;
	XUSG::Pipeline				m_pipelines[NUM_PIPELINE];

	XUSG::DescriptorTable		m_srvTables[NUM_SRV_TABLE];
	XUSG::DescriptorTable		m_uavTables[NUM_UAV_TABLE];
	XUSG::DescriptorTable		m_samplerTable;
	XUSG::Framebuffer			m_framebuffer;

	XUSG::VertexBuffer::uptr	m_vertexBuffers[NUM_MESH];
	XUSG::IndexBuffer::uptr		m_indexBuffers[NUM_MESH];

	XUSG::Texture2D::sptr		m_outputViews[NUM_UAV];
	XUSG::RenderTarget::uptr	m_gbuffers[NUM_GBUFFER];
	XUSG::DepthStencil::sptr	m_depth;

	XUSG::Resource		m_scratch;
	XUSG::Resource		m_instances[FrameCount];

	std::shared_ptr<XUSG::ResourceBase> m_lightProbe;

	// Shader tables
	static const wchar_t* HitGroupName;
	static const wchar_t* RaygenShaderName;
	static const wchar_t* ClosestHitShaderName;
	static const wchar_t* MissShaderName;
	XUSG::RayTracing::ShaderTable::uptr	m_missShaderTable;
	XUSG::RayTracing::ShaderTable::uptr	m_hitGroupShaderTable;
	XUSG::RayTracing::ShaderTable::uptr	m_rayGenShaderTables[FrameCount];

	XUSG::ShaderPool::uptr					m_shaderPool;
	XUSG::RayTracing::PipelineCache::uptr	m_rayTracingPipelineCache;
	XUSG::Graphics::PipelineCache::uptr		m_graphicsPipelineCache;
	XUSG::Compute::PipelineCache::uptr		m_computePipelineCache;
	XUSG::PipelineLayoutCache::uptr			m_pipelineLayoutCache;
	XUSG::DescriptorTableCache::uptr		m_descriptorTableCache;

	GlobalConstants m_cbRaytracing;
};
