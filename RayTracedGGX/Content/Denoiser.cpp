//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "Denoiser.h"

using namespace std;
using namespace DirectX;
using namespace XUSG;

Denoiser::Denoiser() :
	m_frameParity(0)
{
	m_shaderLib = ShaderLib::MakeUnique();
}

Denoiser::~Denoiser()
{
}

bool Denoiser::Init(CommandList* pCommandList, const DescriptorTableLib::sptr& descriptorTableLib,
	uint32_t width, uint32_t height, Format rtFormat, const Texture2D::uptr* inputViews,
	const RenderTarget::uptr* pGbuffers, const DepthStencil::sptr& depth, uint8_t maxMips)
{
	const auto pDevice = pCommandList->GetDevice();
	m_graphicsPipelineLib = Graphics::PipelineLib::MakeUnique(pDevice);
	m_computePipelineLib = Compute::PipelineLib::MakeUnique(pDevice);
	m_pipelineLayoutLib = PipelineLayoutLib::MakeUnique(pDevice);
	m_descriptorTableLib = descriptorTableLib;

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

	const uint8_t mipCount = CalculateMipLevels(width, height);
	for (auto& outputView : m_outputViews) outputView = Texture2D::MakeUnique();
	for (uint8_t i = UAV_TSS; i <= UAV_TSS1; ++i)
		XUSG_N_RETURN(m_outputViews[i]->Create(pDevice, width, height,
			Format::R16G16B16A16_FLOAT, 1, ResourceFlag::ALLOW_UNORDERED_ACCESS,
			(min)(mipCount, maxMips), 1, false, MemoryFlag::NONE, namesUAV[i]), false);

	for (uint8_t i = UAV_FLT_RFL; i <= UAV_FLT_DFF; ++i)
		XUSG_N_RETURN(m_outputViews[i]->Create(pDevice, width, height,
			Format::R16G16B16A16_FLOAT, 1, ResourceFlag::ALLOW_UNORDERED_ACCESS,
			min<uint8_t>(mipCount - 1, maxMips), 1, false, MemoryFlag::NONE,
			namesUAV[i]), false);

	// Create pipelines
	XUSG_N_RETURN(createPipelineLayouts(), false);
	XUSG_N_RETURN(createPipelines(rtFormat), false);
	XUSG_N_RETURN(createDescriptorTables(), false);

	return true;
}

void Denoiser::Denoise(CommandList* pCommandList, uint32_t numBarriers,
	ResourceBarrier* pBarriers, bool useSharedMem)
{
	m_frameParity = !m_frameParity;

	// Bind the acceleration structure, and dispatch rays.
	reflectionSpatialFilter(pCommandList, numBarriers, pBarriers, useSharedMem);
	diffuseSpatialFilter(pCommandList, numBarriers, pBarriers, useSharedMem);
	temporalSS(pCommandList);
}

void Denoiser::ToneMap(CommandList* pCommandList, const Descriptor& rtv,
	uint32_t numBarriers, ResourceBarrier* pBarriers)
{
	// Bind the acceleration structure, and dispatch rays.
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
	const auto& sampler = m_descriptorTableLib->GetSampler(SamplerPreset::LINEAR_CLAMP);

	// This is a pipeline layout for spatial horizontal pass
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(OUTPUT_VIEW, DescriptorType::UAV, 1, 0, 0, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		pipelineLayout->SetRange(SHADER_RESOURCES, DescriptorType::SRV, 1, 0);
		pipelineLayout->SetRange(G_BUFFERS, DescriptorType::SRV, 3, 1);
		XUSG_X_RETURN(m_pipelineLayouts[SPATIAL_H_LAYOUT], pipelineLayout->GetPipelineLayout(m_pipelineLayoutLib.get(),
			PipelineLayoutFlag::NONE, L"SpatialHPipelineLayout"), false);
	}

	// This is a pipeline layout for spatial vertical pass of reflection map
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(OUTPUT_VIEW, DescriptorType::UAV, 1, 0, 0, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		pipelineLayout->SetRange(SHADER_RESOURCES, DescriptorType::SRV, 2, 0);
		pipelineLayout->SetRange(G_BUFFERS, DescriptorType::SRV, 3, 2);
		XUSG_X_RETURN(m_pipelineLayouts[SPT_V_RFL_LAYOUT], pipelineLayout->GetPipelineLayout(m_pipelineLayoutLib.get(),
			PipelineLayoutFlag::NONE, L"ReflectionSpatialVPipelineLayout"), false);
	}

	// This is a pipeline layout for spatial vertical pass of diffuse map
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(OUTPUT_VIEW, DescriptorType::UAV, 1, 0, 0, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		pipelineLayout->SetRange(SHADER_RESOURCES, DescriptorType::SRV, 3, 0);
		pipelineLayout->SetRange(G_BUFFERS, DescriptorType::SRV, 3, 3);
		XUSG_X_RETURN(m_pipelineLayouts[SPT_V_DFF_LAYOUT], pipelineLayout->GetPipelineLayout(m_pipelineLayoutLib.get(),
			PipelineLayoutFlag::NONE, L"DiffuseSpatialVPipelineLayout"), false);
	}

	// This is a pipeline layout for temporal super sampling
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(OUTPUT_VIEW, DescriptorType::UAV, 1, 0, 0, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		pipelineLayout->SetRange(SHADER_RESOURCES, DescriptorType::SRV, 3, 0);
		pipelineLayout->SetStaticSamplers(&sampler, 1, 0);
		XUSG_X_RETURN(m_pipelineLayouts[TEMPORAL_SS_LAYOUT], pipelineLayout->GetPipelineLayout(m_pipelineLayoutLib.get(),
			PipelineLayoutFlag::NONE, L"TemporalSSPipelineLayout"), false);
	}

	// This is a pipeline layout for tone mapping
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::SRV, 1, 0);
		pipelineLayout->SetShaderStage(0, Shader::Stage::PS);
		XUSG_X_RETURN(m_pipelineLayouts[TONE_MAP_LAYOUT], pipelineLayout->GetPipelineLayout(m_pipelineLayoutLib.get(),
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
		XUSG_N_RETURN(m_shaderLib->CreateShader(Shader::Stage::CS, csIndex, L"CSSpatial_H_Refl.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[SPATIAL_H_LAYOUT]);
		state->SetShader(m_shaderLib->GetShader(Shader::Stage::CS, csIndex++));
		XUSG_X_RETURN(m_pipelines[SPATIAL_H_RFL], state->GetPipeline(m_computePipelineLib.get(), L"ReflectionSpatialHPass"), false);
	}

	// Spatial vertical pass of reflection map
	{
		XUSG_N_RETURN(m_shaderLib->CreateShader(Shader::Stage::CS, csIndex, L"CSSpatial_V_Refl.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[SPT_V_RFL_LAYOUT]);
		state->SetShader(m_shaderLib->GetShader(Shader::Stage::CS, csIndex++));
		XUSG_X_RETURN(m_pipelines[SPATIAL_V_RFL], state->GetPipeline(m_computePipelineLib.get(), L"ReflectionSpatialVPass"), false);
	}

	// Spatial horizontal pass of diffuse map
	{
		XUSG_N_RETURN(m_shaderLib->CreateShader(Shader::Stage::CS, csIndex, L"CSSpatial_H_Diff.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[SPATIAL_H_LAYOUT]);
		state->SetShader(m_shaderLib->GetShader(Shader::Stage::CS, csIndex++));
		XUSG_X_RETURN(m_pipelines[SPATIAL_H_DFF], state->GetPipeline(m_computePipelineLib.get(), L"DiffuseSpatialHPass"), false);
	}

	// Spatial vertical pass of diffuse map
	{
		XUSG_N_RETURN(m_shaderLib->CreateShader(Shader::Stage::CS, csIndex, L"CSSpatial_V_Diff.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[SPT_V_DFF_LAYOUT]);
		state->SetShader(m_shaderLib->GetShader(Shader::Stage::CS, csIndex++));
		XUSG_X_RETURN(m_pipelines[SPATIAL_V_DFF], state->GetPipeline(m_computePipelineLib.get(), L"DiffuseSpatialVPass"), false);
	}

	// Spatial horizontal pass of reflection map using shared memory
	{
		XUSG_N_RETURN(m_shaderLib->CreateShader(Shader::Stage::CS, csIndex, L"CSSpatial_H_Refl_S.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[SPATIAL_H_LAYOUT]);
		state->SetShader(m_shaderLib->GetShader(Shader::Stage::CS, csIndex++));
		XUSG_X_RETURN(m_pipelines[SPATIAL_H_RFL_S], state->GetPipeline(m_computePipelineLib.get(), L"ReflectionSpatialHSharedMem"), false);
	}

	// Spatial vertical pass of reflection map using shared memory
	{
		XUSG_N_RETURN(m_shaderLib->CreateShader(Shader::Stage::CS, csIndex, L"CSSpatial_V_Refl_S.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[SPT_V_RFL_LAYOUT]);
		state->SetShader(m_shaderLib->GetShader(Shader::Stage::CS, csIndex++));
		XUSG_X_RETURN(m_pipelines[SPATIAL_V_RFL_S], state->GetPipeline(m_computePipelineLib.get(), L"ReflectionSpatialVSharedMem"), false);
	}

	// Spatial horizontal pass of diffuse map using shared memory
	{
		XUSG_N_RETURN(m_shaderLib->CreateShader(Shader::Stage::CS, csIndex, L"CSSpatial_H_Diff_S.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[SPATIAL_H_LAYOUT]);
		state->SetShader(m_shaderLib->GetShader(Shader::Stage::CS, csIndex++));
		XUSG_X_RETURN(m_pipelines[SPATIAL_H_DFF_S], state->GetPipeline(m_computePipelineLib.get(), L"DiffuseSpatialHSharedMem"), false);
	}

	// Spatial vertical pass of diffuse map
	{
		XUSG_N_RETURN(m_shaderLib->CreateShader(Shader::Stage::CS, csIndex, L"CSSpatial_V_Diff_S.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[SPT_V_DFF_LAYOUT]);
		state->SetShader(m_shaderLib->GetShader(Shader::Stage::CS, csIndex++));
		XUSG_X_RETURN(m_pipelines[SPATIAL_V_DFF_S], state->GetPipeline(m_computePipelineLib.get(), L"DiffuseSpatialVSharedMem"), false);
	}

	// Temporal super sampling
	{
		XUSG_N_RETURN(m_shaderLib->CreateShader(Shader::Stage::CS, csIndex, L"CSTemporalSS.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[TEMPORAL_SS_LAYOUT]);
		state->SetShader(m_shaderLib->GetShader(Shader::Stage::CS, csIndex++));
		XUSG_X_RETURN(m_pipelines[TEMPORAL_SS], state->GetPipeline(m_computePipelineLib.get(), L"TemporalSS"), false);
	}

	// Tone mapping
	{
		XUSG_N_RETURN(m_shaderLib->CreateShader(Shader::Stage::VS, vsIndex, L"VSScreenQuad.cso"), false);
		XUSG_N_RETURN(m_shaderLib->CreateShader(Shader::Stage::PS, psIndex, L"PSToneMap.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[TONE_MAP_LAYOUT]);
		state->SetShader(Shader::Stage::VS, m_shaderLib->GetShader(Shader::Stage::VS, vsIndex++));
		state->SetShader(Shader::Stage::PS, m_shaderLib->GetShader(Shader::Stage::PS, psIndex++));
		state->DSSetState(Graphics::DEPTH_STENCIL_NONE, m_graphicsPipelineLib.get());
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->OMSetNumRenderTargets(1);
		state->OMSetRTVFormat(0, rtFormat);
		XUSG_X_RETURN(m_pipelines[TONE_MAP], state->GetPipeline(m_graphicsPipelineLib.get(), L"ToneMapping"), false);
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
		XUSG_X_RETURN(m_uavTables[UAV_TABLE_FLT + i], descriptorTable->GetCbvSrvUavTable(m_descriptorTableLib.get()), false);
	}

	// Temporal SS output UAVs
	for (uint8_t i = 0; i < 2; ++i)
	{
		if (m_outputViews[UAV_TSS + i]->GetNumMips() <= 1)
		{
			const auto descriptorTable = Util::DescriptorTable::MakeUnique();
			descriptorTable->SetDescriptors(0, 1, &m_outputViews[UAV_TSS + i]->GetUAV());
			XUSG_X_RETURN(m_uavTables[UAV_TABLE_TSS + i], descriptorTable->GetCbvSrvUavTable(m_descriptorTableLib.get()), false);
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
		XUSG_X_RETURN(m_srvTables[SRV_TABLE_GB], descriptorTable->GetCbvSrvUavTable(m_descriptorTableLib.get()), false);
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
		XUSG_X_RETURN(m_srvTables[SRV_TABLE_SPF_RFL + i], descriptorTable->GetCbvSrvUavTable(m_descriptorTableLib.get()), false);
	}

	for (uint8_t i = 0; i < 2; ++i)
	{
		const Descriptor descriptors[] =
		{
			m_inputViews[TERM_DIFFUSE]->GetSRV(),
			m_outputViews[UAV_TSS + i]->GetSRV(),	// Reuse it as scratch
			m_outputViews[UAV_FLT_RFL]->GetSRV()
		};
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		XUSG_X_RETURN(m_srvTables[SRV_TABLE_SPF_DFF + i], descriptorTable->GetCbvSrvUavTable(m_descriptorTableLib.get()), false);
	}

	// Temporal SS input SRVs
	for (uint8_t i = 0; i < 2; ++i)
	{
		const Descriptor descriptors[] =
		{
			m_outputViews[UAV_FLT_DFF]->GetSRV(),
			m_outputViews[UAV_TSS + !i]->GetSRV(),
			m_pGbuffers[VELOCITY]->GetSRV()
		};
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		XUSG_X_RETURN(m_srvTables[SRV_TABLE_TSS + i], descriptorTable->GetCbvSrvUavTable(m_descriptorTableLib.get()), false);
	}

	// Tone mapping SRVs
	for (uint8_t i = 0; i < 2; ++i)
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_outputViews[UAV_TSS + i]->GetSRV());
		XUSG_X_RETURN(m_srvTables[SRV_TABLE_TM + i], descriptorTable->GetCbvSrvUavTable(m_descriptorTableLib.get()), false);
	}

	return true;
}

void Denoiser::reflectionSpatialFilter(CommandList* pCommandList, uint32_t numBarriers,
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
			pCommandList->Dispatch(XUSG_DIV_UP(m_viewport.x, 32), m_viewport.y, 1);
		}
		else
		{
			pCommandList->SetPipelineState(m_pipelines[SPATIAL_H_RFL]);
			pCommandList->Dispatch(XUSG_DIV_UP(m_viewport.x, 8), XUSG_DIV_UP(m_viewport.y, 8), 1);
		}
	}

	// Vertical pass
	{
		numBarriers = m_outputViews[UAV_FLT_RFL]->SetBarrier(pBarriers, ResourceState::UNORDERED_ACCESS, 0, 0);
		numBarriers = m_outputViews[UAV_TSS + m_frameParity]->SetBarrier(pBarriers, ResourceState::NON_PIXEL_SHADER_RESOURCE, numBarriers, 0);
		pCommandList->Barrier(numBarriers, pBarriers);

		pCommandList->SetComputePipelineLayout(m_pipelineLayouts[SPT_V_RFL_LAYOUT]);
		pCommandList->SetComputeDescriptorTable(OUTPUT_VIEW, m_uavTables[UAV_TABLE_FLT_RFL]);
		pCommandList->SetComputeDescriptorTable(SHADER_RESOURCES, m_srvTables[SRV_TABLE_SPF_RFL + m_frameParity]);
		pCommandList->SetComputeDescriptorTable(G_BUFFERS, m_srvTables[SRV_TABLE_GB]);

		if (useSharedMem)
		{
			pCommandList->SetPipelineState(m_pipelines[SPATIAL_V_RFL_S]);
			pCommandList->Dispatch(m_viewport.x, XUSG_DIV_UP(m_viewport.y, 32), 1);
		}
		else
		{
			pCommandList->SetPipelineState(m_pipelines[SPATIAL_V_RFL]);
			pCommandList->Dispatch(XUSG_DIV_UP(m_viewport.x, 8), XUSG_DIV_UP(m_viewport.y, 8), 1);
		}
	}
}

void Denoiser::diffuseSpatialFilter(CommandList* pCommandList, uint32_t numBarriers,
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
			pCommandList->Dispatch(XUSG_DIV_UP(m_viewport.x, 32), m_viewport.y, 1);
		}
		else
		{
			pCommandList->SetPipelineState(m_pipelines[SPATIAL_H_DFF]);
			pCommandList->Dispatch(XUSG_DIV_UP(m_viewport.x, 8), XUSG_DIV_UP(m_viewport.y, 8), 1);
		}
	}

	// Vertical pass
	{
		numBarriers = m_outputViews[UAV_FLT_DFF]->SetBarrier(pBarriers, ResourceState::UNORDERED_ACCESS, 0, 0);
		numBarriers = m_outputViews[UAV_FLT_RFL]->SetBarrier(pBarriers, ResourceState::NON_PIXEL_SHADER_RESOURCE, numBarriers, 0);
		numBarriers = m_outputViews[UAV_TSS + m_frameParity]->SetBarrier(pBarriers, ResourceState::NON_PIXEL_SHADER_RESOURCE, numBarriers, 0);
		pCommandList->Barrier(numBarriers, pBarriers);

		pCommandList->SetComputePipelineLayout(m_pipelineLayouts[SPT_V_DFF_LAYOUT]);
		pCommandList->SetComputeDescriptorTable(OUTPUT_VIEW, m_uavTables[UAV_TABLE_FLT_DFF]);
		pCommandList->SetComputeDescriptorTable(SHADER_RESOURCES, m_srvTables[SRV_TABLE_SPF_DFF + m_frameParity]);
		pCommandList->SetComputeDescriptorTable(G_BUFFERS, m_srvTables[SRV_TABLE_GB]);

		if (useSharedMem)
		{
			pCommandList->SetPipelineState(m_pipelines[SPATIAL_V_DFF_S]);
			pCommandList->Dispatch(m_viewport.x, XUSG_DIV_UP(m_viewport.y, 32), 1);
		}
		else
		{
			pCommandList->SetPipelineState(m_pipelines[SPATIAL_V_DFF]);
			pCommandList->Dispatch(XUSG_DIV_UP(m_viewport.x, 8), XUSG_DIV_UP(m_viewport.y, 8), 1);
		}
	}
}

void Denoiser::temporalSS(CommandList* pCommandList)
{
	ResourceBarrier barriers[5];
	auto numBarriers = m_outputViews[UAV_TSS + m_frameParity]->SetBarrier(barriers, ResourceState::UNORDERED_ACCESS, 0, 0);
	numBarriers = m_outputViews[UAV_FLT_DFF]->SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE, numBarriers, 0);
	numBarriers = m_outputViews[UAV_TSS + !m_frameParity]->SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE, numBarriers, 0);
	numBarriers = m_pGbuffers[VELOCITY]->SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE,
		numBarriers, XUSG_BARRIER_ALL_SUBRESOURCES, BarrierFlag::END_ONLY);
	pCommandList->Barrier(numBarriers, barriers);

	pCommandList->SetComputePipelineLayout(m_pipelineLayouts[TEMPORAL_SS_LAYOUT]);
	pCommandList->SetComputeDescriptorTable(OUTPUT_VIEW, m_uavTables[UAV_TABLE_TSS + m_frameParity]);
	pCommandList->SetComputeDescriptorTable(SHADER_RESOURCES, m_srvTables[SRV_TABLE_TSS + m_frameParity]);

	pCommandList->SetPipelineState(m_pipelines[TEMPORAL_SS]);
	pCommandList->Dispatch(XUSG_DIV_UP(m_viewport.x, 8), XUSG_DIV_UP(m_viewport.y, 8), 1);
}
