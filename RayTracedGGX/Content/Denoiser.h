//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#pragma once

#include "Core/XUSG.h"
#include "RayTracing/XUSGRayTracing.h"

class Denoiser
{
public:
	Denoiser(const XUSG::Device& device);
	virtual ~Denoiser();

	bool Init(XUSG::CommandList* pCommandList, uint32_t width, uint32_t height,
		std::vector<XUSG::Resource>& uploaders, XUSG::Format rtFormat,
		const XUSG::Texture2D::sptr& rtOut, const XUSG::RenderTarget::uptr* pGbuffers,
		const XUSG::DepthStencil::sptr& depth);
	void Denoise(const XUSG::CommandList* pCommandList, bool sharedMemVariance = false);
	void ToneMap(const XUSG::CommandList* pCommandList, const XUSG::Descriptor& rtv,
		uint32_t numBarriers, XUSG::ResourceBarrier* pBarriers);

protected:
	enum PipelineLayoutIndex : uint8_t
	{
		VARIANCE_H_LAYOUT,
		VARIANCE_V_LAYOUT,
		TEMPORAL_SS_LAYOUT,
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
		VARIANCE_H,
		VARIANCE_V,
		VARIANCE_H_S,
		VARIANCE_V_S,
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
		UAV_AVG_H,
		UAV_FLT,
		UAV_TSS,
		UAV_TSS1,

		NUM_UAV
	};

	enum SRVTable : uint8_t
	{
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
		UAV_TABLE_VAR_H,
		UAV_TABLE_VAR_H1,
		UAV_TABLE_FLT,
		UAV_TABLE_TSS,
		UAV_TABLE_TSS1,

		NUM_UAV_TABLE
	};

	bool createPipelineLayouts();
	bool createPipelines(XUSG::Format rtFormat);
	bool createDescriptorTables();

	void varianceDirect(const XUSG::CommandList* pCommandList);
	void varianceSharedMem(const XUSG::CommandList* pCommandList);
	void temporalSS(const XUSG::CommandList* pCommandList);

	XUSG::Device m_device;

	uint8_t						m_frameParity;
	DirectX::XMUINT2			m_viewport;

	XUSG::PipelineLayout		m_pipelineLayouts[NUM_PIPELINE_LAYOUT];
	XUSG::Pipeline				m_pipelines[NUM_PIPELINE];

	XUSG::DescriptorTable		m_srvTables[NUM_SRV_TABLE];
	XUSG::DescriptorTable		m_uavTables[NUM_UAV_TABLE];
	XUSG::DescriptorTable		m_samplerTable;

	XUSG::Texture2D::uptr		m_outputViews[NUM_UAV];
	XUSG::Texture2D::sptr		m_rayTracingOut;
	XUSG::DepthStencil::sptr	m_depth;
	const XUSG::RenderTarget::uptr* m_pGbuffers;

	XUSG::ShaderPool::uptr					m_shaderPool;
	XUSG::Graphics::PipelineCache::uptr		m_graphicsPipelineCache;
	XUSG::Compute::PipelineCache::uptr		m_computePipelineCache;
	XUSG::PipelineLayoutCache::uptr			m_pipelineLayoutCache;
	XUSG::DescriptorTableCache::uptr		m_descriptorTableCache;
};
