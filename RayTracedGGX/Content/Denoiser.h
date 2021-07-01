//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#pragma once

#include "Core/XUSG.h"
#include "RayTracing/XUSGRayTracing.h"

class Denoiser
{
public:
	Denoiser(const XUSG::Device::sptr& device);
	virtual ~Denoiser();

	bool Init(XUSG::CommandList* pCommandList, uint32_t width, uint32_t height, XUSG::Format rtFormat,
		const XUSG::Texture2D::uptr* inputViews, const XUSG::RenderTarget::uptr* pGbuffers,
		const XUSG::DepthStencil::sptr& depth, uint8_t maxMips = 1);
	void Denoise(const XUSG::CommandList* pCommandList, uint32_t numBarriers,
		XUSG::ResourceBarrier* pBarriers, bool useSharedMem = false);
	void ToneMap(const XUSG::CommandList* pCommandList, const XUSG::Descriptor& rtv,
		uint32_t numBarriers, XUSG::ResourceBarrier* pBarriers);

protected:
	enum PipelineLayoutIndex : uint8_t
	{
		SPATIAL_H_LAYOUT,	// Spatial horizontal pass
		SPT_V_RFL_LAYOUT,	// Spatial vertical pass of reflection map
		SPT_V_DFF_LAYOUT,	// Spatial vertical pass of diffuse map
		TEMPORAL_SS_LAYOUT,	// Temporal super sampling
		TONE_MAP_LAYOUT,

		NUM_PIPELINE_LAYOUT
	};

	enum PipelineLayoutSlot : uint8_t
	{
		OUTPUT_VIEW,
		SHADER_RESOURCES,
		G_BUFFERS,
		SAMPLER = G_BUFFERS
	};

	enum PipelineIndex : uint8_t
	{
		SPATIAL_H_RFL,		// Spatial horizontal pass of reflection map
		SPATIAL_V_RFL,		// Spatial vertical pass of reflection map
		SPATIAL_H_DFF,		// Spatial horizontal pass of diffuse map
		SPATIAL_V_DFF,		// Spatial vertical pass of diffuse map
		SPATIAL_H_RFL_S,	// Spatial horizontal pass of reflection map using shared memory
		SPATIAL_V_RFL_S,	// Spatial vertical pass of reflection map using shared memory
		SPATIAL_H_DFF_S,	// Spatial horizontal pass of diffuse map using shared memory
		SPATIAL_V_DFF_S,	// Spatial vertical pass of diffuse map using shared memory
		TEMPORAL_SS,		// Temporal super sampling
		TONE_MAP,

		NUM_PIPELINE
	};

	enum GBuffer : uint8_t
	{
		BASE_COLOR,
		NORMAL,
		ROUGH_METAL,
		VELOCITY,

		NUM_GBUFFER
	};

	enum UAVResource : uint8_t
	{
		UAV_TSS,			// For temporal super sampling
		UAV_TSS1,
		UAV_FLT,			// Spatially filtered
		UAV_FLT1,

		NUM_UAV
	};

	enum SRVTable : uint8_t
	{
		SRV_TABLE_GB,
		SRV_TABLE_SPF_RFL,
		SRV_TABLE_SPF_RFL1,
		SRV_TABLE_SPF_DFF,	// For spatial filter of diffuse map
		SRV_TABLE_SPF_DFF1,
		SRV_TABLE_TSS,		// For temporal super sampling map
		SRV_TABLE_TSS1,
		SRV_TABLE_TM,		// For tone mapping
		SRV_TABLE_TM1,

		NUM_SRV_TABLE
	};

	enum UAVTable : uint8_t
	{
		UAV_TABLE_FLT,
		UAV_TABLE_FLT1,
		UAV_TABLE_TSS,
		UAV_TABLE_TSS1,

		NUM_UAV_TABLE,

		UAV_TABLE_SCT = UAV_TABLE_TSS, // For spatial horizontal filter scratch
	};

	enum FilterTerm : uint8_t
	{
		TERM_REFLECTION,
		TERM_DIFFUSE,

		NUM_TERM
	};

	bool createPipelineLayouts();
	bool createPipelines(XUSG::Format rtFormat);
	bool createDescriptorTables();

	void reflectionSpatialFilter(const XUSG::CommandList* pCommandList, uint32_t numBarriers,
		XUSG::ResourceBarrier* pBarriers, bool useSharedMem);
	void diffuseSpatialFilter(const XUSG::CommandList* pCommandList, uint32_t numBarriers,
		XUSG::ResourceBarrier* pBarriers, bool useSharedMem);
	void temporalSS(const XUSG::CommandList* pCommandList);

	XUSG::Device::sptr m_device;

	uint8_t						m_frameParity;
	DirectX::XMUINT2			m_viewport;

	XUSG::PipelineLayout		m_pipelineLayouts[NUM_PIPELINE_LAYOUT];
	XUSG::Pipeline				m_pipelines[NUM_PIPELINE];

	XUSG::DescriptorTable		m_srvTables[NUM_SRV_TABLE];
	XUSG::DescriptorTable		m_uavTables[NUM_UAV_TABLE];
	XUSG::DescriptorTable		m_samplerTable;

	XUSG::Texture2D::uptr			m_outputViews[NUM_UAV];
	XUSG::DepthStencil::sptr		m_depth;
	const XUSG::Texture2D::uptr*	m_inputViews;
	const XUSG::RenderTarget::uptr* m_pGbuffers;

	XUSG::ShaderPool::uptr				m_shaderPool;
	XUSG::Graphics::PipelineCache::uptr	m_graphicsPipelineCache;
	XUSG::Compute::PipelineCache::uptr	m_computePipelineCache;
	XUSG::PipelineLayoutCache::uptr		m_pipelineLayoutCache;
	XUSG::DescriptorTableCache::uptr	m_descriptorTableCache;
};
