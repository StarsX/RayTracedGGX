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

	RayTracer(const XUSG::RayTracing::Device::sptr& device);
	virtual ~RayTracer();

	bool Init(XUSG::RayTracing::CommandList* pCommandList, uint32_t width, uint32_t height,
		std::vector<XUSG::Resource::uptr>& uploaders, XUSG::RayTracing::GeometryBuffer* pGeometries,
		const char* fileName, const wchar_t* envFileName, XUSG::Format rtFormat,
		const DirectX::XMFLOAT4& posScale = DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f),
		uint8_t maxGBufferMips = 1);
	void UpdateFrame(uint8_t frameIndex, DirectX::CXMVECTOR eyePt, DirectX::CXMMATRIX viewProj, float timeStep);
	void Render(const XUSG::RayTracing::CommandList* pCommandList, uint8_t frameIndex);
	void UpdateAccelerationStructures(const XUSG::RayTracing::CommandList* pCommandList, uint8_t frameIndex);
	void RenderGeometry(const XUSG::RayTracing::CommandList* pCommandList, uint8_t frameIndex);
	void RayTrace(const XUSG::RayTracing::CommandList* pCommandList, uint8_t frameIndex);

	const XUSG::Texture2D::sptr& GetRayTracingOutput() const;
	const XUSG::RenderTarget::uptr* GetGBuffers() const;
	const XUSG::DepthStencil::sptr GetDepth() const;

	static const uint8_t FrameCount = 3;

protected:
	enum PipelineLayoutIndex : uint8_t
	{
		GBUFFER_PASS_LAYOUT,
		RT_GLOBAL_LAYOUT,
		RAY_GEN_LAYOUT,

		NUM_PIPELINE_LAYOUT
	};

	enum PipelineIndex : uint8_t
	{
		GBUFFER_PASS,
		RAY_TRACING,

		NUM_PIPELINE
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

	enum GBuffer : uint8_t
	{
		NORMAL,
		ROUGHNESS,
		VELOCITY,

		NUM_GBUFFER
	};

	enum SRVTable : uint8_t
	{
		SRV_TABLE_IB,
		SRV_TABLE_VB,
		SRV_TABLE_GB,

		NUM_SRV_TABLE
	};

	bool createVB(XUSG::RayTracing::CommandList* pCommandList, uint32_t numVert,
		uint32_t stride, const uint8_t* pData, std::vector<XUSG::Resource::uptr>& uploaders);
	bool createIB(XUSG::RayTracing::CommandList* pCommandList, uint32_t numIndices,
		const uint32_t* pData, std::vector<XUSG::Resource::uptr>& uploaders);
	bool createGroundMesh(XUSG::RayTracing::CommandList* pCommandList,
		std::vector<XUSG::Resource::uptr>& uploaders);
	bool createInputLayout();
	bool createPipelineLayouts();
	bool createPipelines(XUSG::Format rtFormat);
	bool createDescriptorTables();
	bool buildAccelerationStructures(const XUSG::RayTracing::CommandList* pCommandList,
		XUSG::RayTracing::GeometryBuffer* pGeometries);
	bool buildShaderTables();

	void gbufferPass(const XUSG::CommandList* pCommandList, uint8_t frameIndex);
	void rayTrace(const XUSG::RayTracing::CommandList* pCommandList, uint8_t frameIndex);

	XUSG::RayTracing::Device::sptr m_device;

	uint32_t			m_numIndices[NUM_MESH];

	DirectX::XMUINT2	m_viewport;
	DirectX::XMFLOAT4	m_posScale;
	DirectX::XMFLOAT4X4 m_worlds[NUM_MESH];
	DirectX::XMFLOAT4X4 m_worldViewProjs[NUM_MESH];

	XUSG::RayTracing::BottomLevelAS::uptr m_bottomLevelASs[NUM_MESH];
	XUSG::RayTracing::TopLevelAS::uptr m_topLevelAS;

	const XUSG::InputLayout*	m_pInputLayout;
	XUSG::PipelineLayout		m_pipelineLayouts[NUM_PIPELINE_LAYOUT];
	XUSG::Pipeline				m_pipelines[NUM_PIPELINE];

	XUSG::DescriptorTable		m_srvTables[NUM_SRV_TABLE];
	XUSG::DescriptorTable		m_uavTable;
	XUSG::DescriptorTable		m_samplerTable;
	XUSG::Framebuffer			m_framebuffer;

	XUSG::VertexBuffer::uptr	m_vertexBuffers[NUM_MESH];
	XUSG::IndexBuffer::uptr		m_indexBuffers[NUM_MESH];

	XUSG::Texture2D::sptr		m_outputView;
	XUSG::RenderTarget::uptr	m_gbuffers[NUM_GBUFFER];
	XUSG::DepthStencil::sptr	m_depth;

	XUSG::ConstantBuffer::uptr	m_cbBasePass[NUM_MESH];
	XUSG::ConstantBuffer::uptr	m_cbRaytracing;

	XUSG::Resource::uptr		m_scratch;
	XUSG::Resource::uptr		m_instances[FrameCount];

	XUSG::ShaderResource::sptr	m_lightProbe;

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
};
