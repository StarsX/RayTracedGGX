//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#pragma once

#include "Core/XUSG.h"
#include "RayTracing/XUSGRayTracing.h"

class RayTracer
{
public:
	enum MeshIndex : uint32_t
	{
		GROUND,
		MODEL_OBJ,

		NUM_MESH
	};

	RayTracer();
	virtual ~RayTracer();

	bool Init(XUSG::RayTracing::CommandList* pCommandList, const XUSG::DescriptorTableLib::sptr& descriptorTableLib,
		uint32_t width, uint32_t height, std::vector<XUSG::Resource::uptr>& uploaders, XUSG::RayTracing::GeometryBuffer* pGeometries,
		const char* fileName, const wchar_t* envFileName, XUSG::Format rtFormat,
		const DirectX::XMFLOAT4& posScale = DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f),
		uint8_t maxGBufferMips = 1);
	void SetMetallic(uint32_t meshIdx, float metallic);
	void UpdateFrame(const XUSG::RayTracing::Device* pDevice, uint8_t frameIndex,
		DirectX::CXMVECTOR eyePt, DirectX::CXMMATRIX viewProj, float timeStep);
	void Render(XUSG::RayTracing::CommandList* pCommandList, uint8_t frameIndex);
	void UpdateAccelerationStructures(const XUSG::RayTracing::CommandList* pCommandList, uint8_t frameIndex);
	void RenderVisibility(XUSG::RayTracing::CommandList* pCommandList, uint8_t frameIndex);
	void RayTrace(XUSG::RayTracing::CommandList* pCommandList, uint8_t frameIndex);

	const XUSG::Texture2D::uptr* GetRayTracingOutputs() const;
	const XUSG::RenderTarget::uptr* GetGBuffers() const;
	const XUSG::DepthStencil::sptr GetDepth() const;

	static const uint8_t FrameCount = 3;

protected:
	enum PipelineLayoutIndex : uint8_t
	{
		VISIBILITY_LAYOUT,
		RT_GLOBAL_LAYOUT,
		RAY_GEN_LAYOUT,

		NUM_PIPELINE_LAYOUT
	};

	enum PipelineIndex : uint8_t
	{
		VISIBILITY,
		RAY_TRACING,

		NUM_PIPELINE
	};

	enum CSIndex : uint8_t
	{
		CS_RAY_TRACING
	};

	enum GlobalPipelineLayoutSlot : uint8_t
	{
		OUTPUT_VIEW,
		ACCELERATION_STRUCTURE,
		INDEX_BUFFERS,
		VERTEX_BUFFERS,
		MATERIALS,
		CONSTANTS,
		SHADER_RESOURCES
	};

	enum GBuffer : uint8_t
	{
		NORMAL,
		ROUGH_METAL,
		VELOCITY,

		NUM_GBUFFER
	};

	enum SRVTable : uint8_t
	{
		SRV_TABLE_IB,
		SRV_TABLE_VB,
		SRV_TABLE_RO,

		NUM_SRV_TABLE
	};

	enum HitGroup : uint8_t
	{
		HIT_GROUP_REFLECTION,
		HIT_GROUP_DIFFUSE,

		NUM_HIT_GROUP
	};

	bool createVB(XUSG::CommandList* pCommandList, uint32_t numVert, uint32_t stride,
		const uint8_t* pData, std::vector<XUSG::Resource::uptr>& uploaders);
	bool createIB(XUSG::CommandList* pCommandList, uint32_t numIndices,
		const uint32_t* pData, std::vector<XUSG::Resource::uptr>& uploaders);
	bool createGroundMesh(XUSG::CommandList* pCommandList, std::vector<XUSG::Resource::uptr>& uploaders);
	bool createInputLayout();
	bool createPipelineLayouts(const XUSG::RayTracing::Device* pDevice);
	bool createPipelines(XUSG::Format rtFormat, XUSG::Format dsFormat);
	bool createDescriptorTables();
	virtual bool createOutViewTable();
	bool buildAccelerationStructures(XUSG::RayTracing::CommandList* pCommandList,
		XUSG::RayTracing::GeometryBuffer* pGeometries);
	bool buildShaderTables(const XUSG::RayTracing::Device* pDevice);

	void visibility(XUSG::CommandList* pCommandList, uint8_t frameIndex);
	void rayTrace(const XUSG::RayTracing::CommandList* pCommandList, uint8_t frameIndex);

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
	XUSG::Framebuffer			m_framebuffer;

	XUSG::VertexBuffer::uptr	m_vertexBuffers[NUM_MESH];
	XUSG::IndexBuffer::uptr		m_indexBuffers[NUM_MESH];

	XUSG::Texture2D::uptr		m_outputViews[NUM_HIT_GROUP];
	XUSG::RenderTarget::uptr	m_visBuffer;
	XUSG::RenderTarget::uptr	m_gbuffers[NUM_GBUFFER];
	XUSG::DepthStencil::sptr	m_depth;

	XUSG::ConstantBuffer::uptr	m_cbPerOjects[NUM_MESH];
	XUSG::ConstantBuffer::uptr	m_cbMaterials;
	XUSG::ConstantBuffer::uptr	m_cbRaytracing;

	XUSG::Resource::uptr		m_scratch;
	XUSG::Resource::uptr		m_instances[FrameCount];

	XUSG::Texture::sptr			m_lightProbe;

	// Shader tables
	static const wchar_t* HitGroupNames[NUM_HIT_GROUP];
	static const wchar_t* RaygenShaderName;
	static const wchar_t* ClosestHitShaderNames[NUM_HIT_GROUP];
	static const wchar_t* MissShaderName;
	XUSG::RayTracing::ShaderTable::uptr	m_missShaderTable;
	XUSG::RayTracing::ShaderTable::uptr	m_hitGroupShaderTable;
	XUSG::RayTracing::ShaderTable::uptr	m_rayGenShaderTables[FrameCount];

	XUSG::ShaderLib::uptr				m_shaderLib;
	XUSG::RayTracing::PipelineLib::uptr	m_rayTracingPipelineCache;
	XUSG::Graphics::PipelineLib::uptr	m_graphicsPipelineLib;
	XUSG::Compute::PipelineLib::uptr	m_computePipelineLib;
	XUSG::PipelineLayoutLib::uptr		m_pipelineLayoutLib;
	XUSG::DescriptorTableLib::sptr		m_descriptorTableLib;
};
