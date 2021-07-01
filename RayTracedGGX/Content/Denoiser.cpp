//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "Denoiser.h"

using namespace std;
using namespace DirectX;
using namespace XUSG;

Denoiser::Denoiser(const Device::sptr& device) :
	m_device(device),
	m_frameParity(0)
{
	m_shaderPool = ShaderPool::MakeUnique();
	m_graphicsPipelineCache = Graphics::PipelineCache::MakeUnique(device.get());
	m_computePipelineCache = Compute::PipelineCache::MakeUnique(device.get());
	m_pipelineLayoutCache = PipelineLayoutCache::MakeUnique(device.get());
	m_descriptorTableCache = DescriptorTableCache::MakeUnique(device.get(), L"DenoiserDescriptorTableCache");
}

Denoiser::~Denoiser()
{
}

bool Denoiser::Init(CommandList* pCommandList, uint32_t width, uint32_t height, Format rtFormat,
	const Texture2D::uptr* inputViews, const RenderTarget::uptr* pGbuffers,
	const DepthStencil::sptr& depth, uint8_t maxMips)
{
	m_viewport = XMUINT2(width, height);
	m_inputViews = inputViews;
	m_pGbuffers = pGbuffers;
	m_depth = depth;

	// Create output views
	const wchar_t* namesUAV[] =
	{
		L"TemporalSSOut0",
		L"TemporalSSOut1",
		L"FilteredOut",
		L"FilteredOut1"
	};

	const uint8_t mipCount = max<uint8_t>(Log2((max)(width, height)), 0) + 1;

	for (auto& outputView : m_outputViews) outputView = Texture2D::MakeUnique();
	for (uint8_t i = UAV_TSS; i <= UAV_TSS1; ++i)
		N_RETURN(m_outputViews[i]->Create(m_device.get(), width, height,
			Format::R16G16B16A16_FLOAT, 1, ResourceFlag::ALLOW_UNORDERED_ACCESS,
			(min)(mipCount, maxMips), 1, MemoryType::DEFAULT, false, namesUAV[i]), false);

	N_RETURN(m_outputViews[UAV_FLT]->Create(m_device.get(), width, height,
		Format::R16G16B16A16_FLOAT, 1, ResourceFlag::ALLOW_UNORDERED_ACCESS,
		min<uint8_t>(mipCount - 1, maxMips), 1, MemoryType::DEFAULT, false, namesUAV[UAV_FLT]), false);
	N_RETURN(m_outputViews[UAV_FLT1]->Create(m_device.get(), width, height,
		Format::R16G16B16A16_FLOAT, 1, ResourceFlag::ALLOW_UNORDERED_ACCESS,
		1, 1, MemoryType::DEFAULT, false, namesUAV[UAV_FLT1]), false);

	// Create pipelines
	N_RETURN(createPipelineLayouts(), false);
	N_RETURN(createPipelines(rtFormat), false);
	N_RETURN(createDescriptorTables(), false);

	return true;
}

void Denoiser::Denoise(const CommandList* pCommandList, uint32_t numBarriers,
	ResourceBarrier* pBarriers, bool useSharedMem)
{
	m_frameParity = !m_frameParity;

	// Bind the heaps, acceleration structure and dispatch rays.
	const DescriptorPool descriptorPools[] =
	{
		m_descriptorTableCache->GetDescriptorPool(CBV_SRV_UAV_POOL),
		m_descriptorTableCache->GetDescriptorPool(SAMPLER_POOL)
	};
	pCommandList->SetDescriptorPools(static_cast<uint32_t>(size(descriptorPools)), descriptorPools);

	reflectionSpatialFilter(pCommandList, numBarriers, pBarriers, useSharedMem);
	diffuseSpatialFilter(pCommandList, numBarriers, pBarriers, useSharedMem);
	temporalSS(pCommandList);
}

void Denoiser::ToneMap(const CommandList* pCommandList, const Descriptor& rtv,
	uint32_t numBarriers, ResourceBarrier* pBarriers)
{
	// Bind the heaps, acceleration structure and dispatch rays.
	const DescriptorPool descriptorPools[] =
	{
		m_descriptorTableCache->GetDescriptorPool(CBV_SRV_UAV_POOL),
		m_descriptorTableCache->GetDescriptorPool(SAMPLER_POOL)
	};
	pCommandList->SetDescriptorPools(static_cast<uint32_t>(size(descriptorPools)), descriptorPools);

	numBarriers = m_outputViews[UAV_TSS + m_frameParity]->SetBarrier(
		pBarriers, ResourceState::PIXEL_SHADER_RESOURCE, numBarriers, 0);
	pCommandList->Barrier(numBarriers, pBarriers);

	// Set render target
	pCommandList->OMSetRenderTargets(1, &rtv);

	// Set descriptor tables
	pCommandList->SetGraphicsPipelineLayout(m_pipelineLayouts[TONE_MAP_LAYOUT]);
	pCommandList->SetGraphicsDescriptorTable(0, m_srvTables[SRV_TABLE_TM + m_frameParity]);

	// Set pipeline state
	pCommandList->SetPipelineState(m_pipelines[TONE_MAP]);

	// Set viewport
	Viewport viewport(0.0f, 0.0f, static_cast<float>(m_viewport.x), static_cast<float>(m_viewport.y));
	RectRange scissorRect(0, 0, m_viewport.x, m_viewport.y);
	pCommandList->RSSetViewports(1, &viewport);
	pCommandList->RSSetScissorRects(1, &scissorRect);

	pCommandList->IASetPrimitiveTopology(PrimitiveTopology::TRIANGLELIST);
	pCommandList->Draw(3, 1, 0, 0);
}

bool Denoiser::createPipelineLayouts()
{
	// This is a pipeline layout for spatial horizontal pass
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(OUTPUT_VIEW, DescriptorType::UAV, 1, 0, 0, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		pipelineLayout->SetRange(SHADER_RESOURCES, DescriptorType::SRV, 1, 0);
		pipelineLayout->SetRange(G_BUFFERS, DescriptorType::SRV, 3, 1);
		X_RETURN(m_pipelineLayouts[SPATIAL_H_LAYOUT], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"SpatialHPipelineLayout"), false);
	}

	// This is a pipeline layout for spatial vertical pass of reflection map
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(OUTPUT_VIEW, DescriptorType::UAV, 1, 0, 0, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		pipelineLayout->SetRange(SHADER_RESOURCES, DescriptorType::SRV, 2, 0);
		pipelineLayout->SetRange(G_BUFFERS, DescriptorType::SRV, 3, 2);
		X_RETURN(m_pipelineLayouts[SPT_V_RFL_LAYOUT], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"ReflectionSpatialVPipelineLayout"), false);
	}

	// This is a pipeline layout for spatial vertical pass of diffuse map
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(OUTPUT_VIEW, DescriptorType::UAV, 1, 0, 0, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		pipelineLayout->SetRange(SHADER_RESOURCES, DescriptorType::SRV, 3, 0);
		pipelineLayout->SetRange(G_BUFFERS, DescriptorType::SRV, 3, 3);
		X_RETURN(m_pipelineLayouts[SPT_V_DFF_LAYOUT], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"DiffuseSpatialVPipelineLayout"), false);
	}

	// This is a pipeline layout for temporal super sampling
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(OUTPUT_VIEW, DescriptorType::UAV, 1, 0, 0, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		pipelineLayout->SetRange(SHADER_RESOURCES, DescriptorType::SRV, 3, 0);
		pipelineLayout->SetRange(SAMPLER, DescriptorType::SAMPLER, 1, 0);
		X_RETURN(m_pipelineLayouts[TEMPORAL_SS_LAYOUT], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"TemporalSSPipelineLayout"), false);
	}

	// This is a pipeline layout for tone mapping
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::SRV, 1, 0);
		pipelineLayout->SetShaderStage(0, Shader::Stage::PS);
		X_RETURN(m_pipelineLayouts[TONE_MAP_LAYOUT], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"ToneMappingPipelineLayout"), false);
	}

	return true;
}

bool Denoiser::createPipelines(Format rtFormat)
{
	auto vsIndex = 0u;
	auto psIndex = 0u;
	auto csIndex = 0u;

	// Spatial horizontal pass of reflection map
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"CSSpatial_H_Refl.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[SPATIAL_H_LAYOUT]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[SPATIAL_H_RFL], state->GetPipeline(m_computePipelineCache.get(), L"ReflectionSpatialHPass"), false);
	}

	// Spatial vertical pass of reflection map
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"CSSpatial_V_Refl.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[SPT_V_RFL_LAYOUT]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[SPATIAL_V_RFL], state->GetPipeline(m_computePipelineCache.get(), L"ReflectionSpatialVPass"), false);
	}

	// Spatial horizontal pass of diffuse map
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"CSSpatial_H_Diff.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[SPATIAL_H_LAYOUT]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[SPATIAL_H_DFF], state->GetPipeline(m_computePipelineCache.get(), L"DiffuseSpatialHPass"), false);
	}

	// Spatial vertical pass of diffuse map
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"CSSpatial_V_Diff.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[SPT_V_DFF_LAYOUT]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[SPATIAL_V_DFF], state->GetPipeline(m_computePipelineCache.get(), L"DiffuseSpatialVPass"), false);
	}

	// Spatial horizontal pass of reflection map using shared memory
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"CSSpatial_H_Refl_S.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[SPATIAL_H_LAYOUT]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[SPATIAL_H_RFL_S], state->GetPipeline(m_computePipelineCache.get(), L"ReflectionSpatialHSharedMem"), false);
	}

	// Spatial vertical pass of reflection map using shared memory
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"CSSpatial_V_Refl_S.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[SPT_V_RFL_LAYOUT]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[SPATIAL_V_RFL_S], state->GetPipeline(m_computePipelineCache.get(), L"ReflectionSpatialVSharedMem"), false);
	}

	// Spatial horizontal pass of diffuse map using shared memory
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"CSSpatial_H_Diff_S.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[SPATIAL_H_LAYOUT]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[SPATIAL_H_DFF_S], state->GetPipeline(m_computePipelineCache.get(), L"DiffuseSpatialHSharedMem"), false);
	}

	// Spatial vertical pass of diffuse map
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"CSSpatial_V_Diff_S.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[SPT_V_DFF_LAYOUT]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[SPATIAL_V_DFF_S], state->GetPipeline(m_computePipelineCache.get(), L"DiffuseSpatialVSharedMem"), false);
	}

	// Temporal super sampling
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"CSTemporalSS.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[TEMPORAL_SS_LAYOUT]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[TEMPORAL_SS], state->GetPipeline(m_computePipelineCache.get(), L"TemporalSS"), false);
	}

	// Tone mapping
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::VS, vsIndex, L"VSScreenQuad.cso"), false);
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, psIndex, L"PSToneMap.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[TONE_MAP_LAYOUT]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, vsIndex++));
		state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, psIndex++));
		state->DSSetState(Graphics::DEPTH_STENCIL_NONE, m_graphicsPipelineCache.get());
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->OMSetNumRenderTargets(1);
		state->OMSetRTVFormat(0, rtFormat);
		X_RETURN(m_pipelines[TONE_MAP], state->GetPipeline(m_graphicsPipelineCache.get(), L"ToneMapping"), false);
	}

	return true;
}

bool Denoiser::createDescriptorTables()
{
	// Spatial filter output UAVs
	for (uint8_t i = 0; i < NUM_TERM; ++i)
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_outputViews[UAV_FLT + i]->GetUAV());
		X_RETURN(m_uavTables[UAV_TABLE_FLT + i], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	// Temporal SS output UAVs
	for (uint8_t i = 0; i < 2; ++i)
	{
		if (m_outputViews[UAV_TSS + i]->GetNumMips() <= 1)
		{
			const auto descriptorTable = Util::DescriptorTable::MakeUnique();
			descriptorTable->SetDescriptors(0, 1, &m_outputViews[UAV_TSS + i]->GetUAV());
			X_RETURN(m_uavTables[UAV_TABLE_TSS + i], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
		}
	}

	// G-buffer SRVs
	{
		const Descriptor descriptors[] =
		{
			m_pGbuffers[NORMAL]->GetSRV(),
			m_pGbuffers[ROUGH_METAL]->GetSRV(),
			m_depth->GetSRV()
		};
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		X_RETURN(m_srvTables[SRV_TABLE_GB], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	// Spatial filter input SRVs
	for (uint8_t i = 0; i < 2; ++i)
	{
		const Descriptor descriptors[] =
		{
			m_inputViews[TERM_REFLECTION]->GetSRV(),
			m_outputViews[UAV_TSS + i]->GetSRV()	// Reuse it as scratch
		};
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		X_RETURN(m_srvTables[SRV_TABLE_SPF_RFL + i], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	for (uint8_t i = 0; i < 2; ++i)
	{
		const Descriptor descriptors[] =
		{
			m_inputViews[TERM_DIFFUSE]->GetSRV(),
			m_outputViews[UAV_TSS + i]->GetSRV(),	// Reuse it as scratch
			m_outputViews[UAV_FLT]->GetSRV()
		};
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		X_RETURN(m_srvTables[SRV_TABLE_SPF_DFF + i], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	// Temporal SS input SRVs
	for (uint8_t i = 0; i < 2; ++i)
	{
		const Descriptor descriptors[] =
		{
			m_outputViews[UAV_FLT1]->GetSRV(),
			m_outputViews[UAV_TSS + !i]->GetSRV(),
			m_pGbuffers[VELOCITY]->GetSRV()
		};
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		X_RETURN(m_srvTables[SRV_TABLE_TSS + i], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	// Tone mapping SRVs
	for (uint8_t i = 0; i < 2; ++i)
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_outputViews[UAV_TSS + i]->GetSRV());
		X_RETURN(m_srvTables[SRV_TABLE_TM + i], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	// Create the sampler
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		const auto samplerLinearClamp = SamplerPreset::LINEAR_CLAMP;
		descriptorTable->SetSamplers(0, 1, &samplerLinearClamp, m_descriptorTableCache.get());
		X_RETURN(m_samplerTable, descriptorTable->GetSamplerTable(m_descriptorTableCache.get()), false);
	}

	return true;
}

void Denoiser::reflectionSpatialFilter(const CommandList* pCommandList, uint32_t numBarriers,
	ResourceBarrier* pBarriers, bool useSharedMem)
{
	// Horizontal pass
	{
		numBarriers = m_outputViews[UAV_TSS + m_frameParity]->SetBarrier(pBarriers, ResourceState::UNORDERED_ACCESS, 0, 0);
		numBarriers = m_inputViews[TERM_REFLECTION]->SetBarrier(pBarriers, ResourceState::NON_PIXEL_SHADER_RESOURCE, numBarriers);
		pCommandList->Barrier(numBarriers, pBarriers);

		pCommandList->SetComputePipelineLayout(m_pipelineLayouts[SPATIAL_H_LAYOUT]);
		pCommandList->SetComputeDescriptorTable(OUTPUT_VIEW, m_uavTables[UAV_TABLE_SCT + m_frameParity]);
		pCommandList->SetComputeDescriptorTable(SHADER_RESOURCES, m_srvTables[SRV_TABLE_SPF_RFL]);
		pCommandList->SetComputeDescriptorTable(G_BUFFERS, m_srvTables[SRV_TABLE_GB]);

		if (useSharedMem)
		{
			pCommandList->SetPipelineState(m_pipelines[SPATIAL_H_RFL_S]);
			pCommandList->Dispatch(DIV_UP(m_viewport.x, 32), m_viewport.y, 1);
		}
		else
		{
			pCommandList->SetPipelineState(m_pipelines[SPATIAL_H_RFL]);
			pCommandList->Dispatch(DIV_UP(m_viewport.x, 8), DIV_UP(m_viewport.y, 8), 1);
		}
	}

	// Vertical pass
	{
		numBarriers = m_outputViews[UAV_FLT]->SetBarrier(pBarriers, ResourceState::UNORDERED_ACCESS, 0, 0);
		numBarriers = m_outputViews[UAV_TSS + m_frameParity]->SetBarrier(pBarriers, ResourceState::NON_PIXEL_SHADER_RESOURCE, numBarriers, 0);
		pCommandList->Barrier(numBarriers, pBarriers);

		pCommandList->SetComputePipelineLayout(m_pipelineLayouts[SPT_V_RFL_LAYOUT]);
		pCommandList->SetComputeDescriptorTable(OUTPUT_VIEW, m_uavTables[UAV_TABLE_FLT]);
		pCommandList->SetComputeDescriptorTable(SHADER_RESOURCES, m_srvTables[SRV_TABLE_SPF_RFL + m_frameParity]);
		pCommandList->SetComputeDescriptorTable(G_BUFFERS, m_srvTables[SRV_TABLE_GB]);

		if (useSharedMem)
		{
			pCommandList->SetPipelineState(m_pipelines[SPATIAL_V_RFL_S]);
			pCommandList->Dispatch(m_viewport.x, DIV_UP(m_viewport.y, 32), 1);
		}
		else
		{
			pCommandList->SetPipelineState(m_pipelines[SPATIAL_V_RFL]);
			pCommandList->Dispatch(DIV_UP(m_viewport.x, 8), DIV_UP(m_viewport.y, 8), 1);
		}
	}
}

void Denoiser::diffuseSpatialFilter(const CommandList* pCommandList, uint32_t numBarriers,
	ResourceBarrier* pBarriers, bool useSharedMem)
{
	// Horizontal pass
	{
		numBarriers = m_outputViews[UAV_TSS + m_frameParity]->SetBarrier(pBarriers, ResourceState::UNORDERED_ACCESS, 0, 0);
		numBarriers = m_inputViews[TERM_DIFFUSE]->SetBarrier(pBarriers, ResourceState::NON_PIXEL_SHADER_RESOURCE, numBarriers);
		pCommandList->Barrier(numBarriers, pBarriers);

		pCommandList->SetComputePipelineLayout(m_pipelineLayouts[SPATIAL_H_LAYOUT]);
		pCommandList->SetComputeDescriptorTable(OUTPUT_VIEW, m_uavTables[UAV_TABLE_SCT + m_frameParity]);
		pCommandList->SetComputeDescriptorTable(SHADER_RESOURCES, m_srvTables[SRV_TABLE_SPF_DFF]);
		pCommandList->SetComputeDescriptorTable(G_BUFFERS, m_srvTables[SRV_TABLE_GB]);

		if (useSharedMem)
		{
			pCommandList->SetPipelineState(m_pipelines[SPATIAL_H_DFF_S]);
			pCommandList->Dispatch(DIV_UP(m_viewport.x, 32), m_viewport.y, 1);
		}
		else
		{
			pCommandList->SetPipelineState(m_pipelines[SPATIAL_H_DFF]);
			pCommandList->Dispatch(DIV_UP(m_viewport.x, 8), DIV_UP(m_viewport.y, 8), 1);
		}
	}

	// Vertical pass
	{
		numBarriers = m_outputViews[UAV_FLT1]->SetBarrier(pBarriers, ResourceState::UNORDERED_ACCESS, 0, 0);
		numBarriers = m_outputViews[UAV_FLT]->SetBarrier(pBarriers, ResourceState::NON_PIXEL_SHADER_RESOURCE, numBarriers, 0);
		numBarriers = m_outputViews[UAV_TSS + m_frameParity]->SetBarrier(pBarriers, ResourceState::NON_PIXEL_SHADER_RESOURCE, numBarriers, 0);
		pCommandList->Barrier(numBarriers, pBarriers);

		pCommandList->SetComputePipelineLayout(m_pipelineLayouts[SPT_V_DFF_LAYOUT]);
		pCommandList->SetComputeDescriptorTable(OUTPUT_VIEW, m_uavTables[UAV_TABLE_FLT1]);
		pCommandList->SetComputeDescriptorTable(SHADER_RESOURCES, m_srvTables[SRV_TABLE_SPF_DFF + m_frameParity]);
		pCommandList->SetComputeDescriptorTable(G_BUFFERS, m_srvTables[SRV_TABLE_GB]);

		if (useSharedMem)
		{
			pCommandList->SetPipelineState(m_pipelines[SPATIAL_V_DFF_S]);
			pCommandList->Dispatch(m_viewport.x, DIV_UP(m_viewport.y, 32), 1);
		}
		else
		{
			pCommandList->SetPipelineState(m_pipelines[SPATIAL_V_DFF]);
			pCommandList->Dispatch(DIV_UP(m_viewport.x, 8), DIV_UP(m_viewport.y, 8), 1);
		}
	}
}

void Denoiser::temporalSS(const CommandList* pCommandList)
{
	ResourceBarrier barriers[5];
	auto numBarriers = m_outputViews[UAV_TSS + m_frameParity]->SetBarrier(barriers, ResourceState::UNORDERED_ACCESS, 0, 0);
	numBarriers = m_outputViews[UAV_FLT1]->SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE, numBarriers, 0);
	numBarriers = m_outputViews[UAV_TSS + !m_frameParity]->SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE, numBarriers, 0);
	numBarriers = m_pGbuffers[VELOCITY]->SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE,
		numBarriers, BARRIER_ALL_SUBRESOURCES, BarrierFlag::END_ONLY);
	pCommandList->Barrier(numBarriers, barriers);

	pCommandList->SetComputePipelineLayout(m_pipelineLayouts[TEMPORAL_SS_LAYOUT]);
	pCommandList->SetComputeDescriptorTable(OUTPUT_VIEW, m_uavTables[UAV_TABLE_TSS + m_frameParity]);
	pCommandList->SetComputeDescriptorTable(SHADER_RESOURCES, m_srvTables[SRV_TABLE_TSS + m_frameParity]);
	pCommandList->SetComputeDescriptorTable(SAMPLER, m_samplerTable);

	pCommandList->SetPipelineState(m_pipelines[TEMPORAL_SS]);
	pCommandList->Dispatch(DIV_UP(m_viewport.x, 8), DIV_UP(m_viewport.y, 8), 1);
}
