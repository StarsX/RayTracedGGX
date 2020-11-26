//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "Optional/XUSGObjLoader.h"
#include "RayTracer.h"

using namespace std;
using namespace DirectX;
using namespace XUSG;
using namespace XUSG::RayTracing;

const wchar_t* RayTracer::HitGroupName = L"hitGroup";
const wchar_t* RayTracer::RaygenShaderName = L"raygenMain";
const wchar_t* RayTracer::ClosestHitShaderName = L"closestHitMain";
const wchar_t* RayTracer::MissShaderName = L"missMain";

RayTracer::RayTracer(const RayTracing::Device& device) :
	m_device(device),
	m_frameParity(0),
	m_instances(),
	m_pipeIndex(TEST)
{
	m_shaderPool = ShaderPool::MakeUnique();
	m_rayTracingPipelineCache = RayTracing::PipelineCache::MakeUnique(device);
	m_graphicsPipelineCache = Graphics::PipelineCache::MakeUnique(device.Common);
	m_computePipelineCache = Compute::PipelineCache::MakeUnique(device.Common);
	m_descriptorTableCache = DescriptorTableCache::MakeUnique(device.Common, L"RayTracerDescriptorTableCache");
	m_pipelineLayoutCache = PipelineLayoutCache::MakeUnique(device.Common);

	AccelerationStructure::SetUAVCount(NUM_MESH + 1);
}

RayTracer::~RayTracer()
{
}

bool RayTracer::Init(RayTracing::CommandList* pCommandList, uint32_t width, uint32_t height,
	vector<Resource>& uploaders, Geometry* geometries, const char* fileName,
	Format rtFormat, const XMFLOAT4& posScale)
{
	m_viewport = XMUINT2(width, height);
	m_posScale = posScale;

	// Load inputs
	ObjLoader objLoader;
	if (!objLoader.Import(fileName, true, true)) return false;
	N_RETURN(createVB(pCommandList, objLoader.GetNumVertices(), objLoader.GetVertexStride(), objLoader.GetVertices(), uploaders), false);
	N_RETURN(createIB(pCommandList, objLoader.GetNumIndices(), objLoader.GetIndices(), uploaders), false);

	N_RETURN(createGroundMesh(pCommandList, uploaders), false);

	// Create raytracing pipelines
	N_RETURN(createInputLayout(), false);
	N_RETURN(createPipelineLayouts(), false);
	N_RETURN(createPipelines(rtFormat), false);

	// Create output view and build acceleration structures
	for (auto& texture : m_outputViews) texture = Texture2D::MakeUnique();
	m_outputViews[UAV_TABLE_RT_OUT]->Create(m_device.Common, width, height,
		Format::R16G16B16A16_FLOAT, 1, ResourceFlag::ALLOW_UNORDERED_ACCESS,
		1, 1, MemoryType::DEFAULT, false, L"RayTracingOut");
	m_outputViews[UAV_TABLE_SPATIAL]->Create(m_device.Common, width, height,
		Format::R16G16B16A16_FLOAT, 1, ResourceFlag::ALLOW_UNORDERED_ACCESS,
		1, 1, MemoryType::DEFAULT, false, L"SpatialOut0");
	m_outputViews[UAV_TABLE_SPATIAL1]->Create(m_device.Common, width, height, Format::R16G16B16A16_FLOAT,
		1, ResourceFlag::ALLOW_UNORDERED_ACCESS, 1, 1, MemoryType::DEFAULT, false, L"SpatialOut1");
	m_outputViews[UAV_TABLE_TSAMP]->Create(m_device.Common, width, height, Format::R16G16B16A16_FLOAT,
		1, ResourceFlag::ALLOW_UNORDERED_ACCESS, 1, 1, MemoryType::DEFAULT, false, L"TemporalSSOut0");
	m_outputViews[UAV_TABLE_TSAMP1]->Create(m_device.Common, width, height, Format::R16G16B16A16_FLOAT,
		1, ResourceFlag::ALLOW_UNORDERED_ACCESS, 1, 1, MemoryType::DEFAULT, false, L"TemporalSSOut1");

	m_gbuffers[NORMAL] = RenderTarget::MakeUnique();
	m_gbuffers[NORMAL]->Create(m_device.Common, width, height, Format::R10G10B10A2_UNORM,
		1, ResourceFlag::NONE, 1, 1, nullptr, false, L"Normal");
	m_gbuffers[VELOCITY] = RenderTarget::MakeUnique();
	m_gbuffers[VELOCITY]->Create(m_device.Common, width, height, Format::R16G16_FLOAT,
		1, ResourceFlag::NONE, 1, 1, nullptr, false, L"Velocity");

	m_depth = DepthStencil::MakeUnique();
	m_depth->Create(m_device.Common, width, height, Format::D24_UNORM_S8_UINT,
		ResourceFlag::NONE, 1, 1, 1, 1.0f, 0, false, L"Depth");

	N_RETURN(buildAccelerationStructures(pCommandList, geometries), false);
	N_RETURN(buildShaderTables(), false);

	return true;
}

void RayTracer::SetPipeline(RayTracingPipeline pipeline)
{
	m_pipeIndex = pipeline;
}

static const XMFLOAT2& IncrementalHalton()
{
	static auto haltonBase = XMUINT2(0, 0);
	static auto halton = XMFLOAT2(0.0f, 0.0f);

	// Base 2
	{
		// Bottom bit always changes, higher bits
		// Change less frequently.
		auto change = 0.5f;
		auto oldBase = haltonBase.x++;
		auto diff = haltonBase.x ^ oldBase;

		// Diff will be of the form 0*1+, i.e. one bits up until the last carry.
		// Expected iterations = 1 + 0.5 + 0.25 + ... = 2
		do
		{
			halton.x += (oldBase & 1) ? -change : change;
			change *= 0.5f;

			diff = diff >> 1;
			oldBase = oldBase >> 1;
		} while (diff);
	}

	// Base 3
	{
		const auto oneThird = 1.0f / 3.0f;
		auto mask = 0x3u;	// Also the max base 3 digit
		auto add = 0x1u;	// Amount to add to force carry once digit == 3
		auto change = oneThird;
		++haltonBase.y;

		// Expected iterations: 1.5
		while (true)
		{
			if ((haltonBase.y & mask) == mask)
			{
				haltonBase.y += add;	// Force carry into next 2-bit digit
				halton.y -= 2 * change;

				mask = mask << 2;
				add = add << 2;

				change *= oneThird;
			}
			else
			{
				halton.y += change;	// We know digit n has gone from a to a + 1
				break;
			}
		};
	}

	return halton;
}

void RayTracer::UpdateFrame(uint32_t frameIndex, CXMVECTOR eyePt, CXMMATRIX viewProj, bool isPaused)
{
	const auto halton = IncrementalHalton();
	XMFLOAT2 projBias =
	{
		(halton.x * 2.0f - 1.0f) / m_viewport.x,
		(halton.y * 2.0f - 1.0f) / m_viewport.y
	};

	{
		const auto projToWorld = XMMatrixInverse(nullptr, viewProj);
		RayGenConstants cbRayGen = { XMMatrixTranspose(projToWorld), eyePt };

		m_rayGenShaderTables[frameIndex][m_pipeIndex]->Reset();
		m_rayGenShaderTables[frameIndex][m_pipeIndex]->AddShaderRecord(*ShaderRecord::MakeUnique(m_device,
			m_rayTracingPipelines[m_pipeIndex], RaygenShaderName, &cbRayGen, sizeof(RayGenConstants)));
	}

	{
		static auto angle = 0.0f;
		angle += isPaused ? 0.0f : 0.025f * XM_PI / 180.0f;
		const auto rot = XMMatrixRotationY(angle);

		const auto n = 256u;
		XMStoreFloat3x4(&m_cbRaytracing.Normal, rot);
		++m_cbRaytracing.FrameIndex;
		m_cbRaytracing.FrameIndex %= n;

		XMMATRIX worlds[NUM_MESH] =
		{
			XMMatrixScaling(10.0f, 0.5f, 10.0f) * XMMatrixTranslation(0.0f, -0.5f, 0.0f),
			XMMatrixScaling(m_posScale.w, m_posScale.w, m_posScale.w) * rot *
			XMMatrixTranslation(m_posScale.x, m_posScale.y, m_posScale.z)
		};

		for (auto i = 0; i < NUM_MESH; ++i)
		{
			m_cbBasePass[i].ProjBias = projBias;
			m_cbBasePass[i].WorldViewProjPrev = m_cbBasePass[i].WorldViewProj;
			XMStoreFloat4x4(&m_worlds[i], XMMatrixTranspose(worlds[i]));
			XMStoreFloat4x4(&m_cbBasePass[i].WorldViewProj, XMMatrixTranspose(worlds[i] * viewProj));
			XMStoreFloat3x4(&m_cbBasePass[i].Normal, i ? rot : XMMatrixIdentity());
		}
	}

	m_frameParity = !m_frameParity;
}

void RayTracer::Render(const RayTracing::CommandList* pCommandList, uint32_t frameIndex)
{
	updateAccelerationStructures(pCommandList, frameIndex);

	ResourceBarrier barriers[4];
	auto numBarriers = m_gbuffers[NORMAL]->SetBarrier(barriers, ResourceState::RENDER_TARGET);
	numBarriers = m_gbuffers[VELOCITY]->SetBarrier(barriers, ResourceState::RENDER_TARGET, numBarriers);
	numBarriers = m_depth->SetBarrier(barriers, ResourceState::DEPTH_WRITE, numBarriers);
	pCommandList->Barrier(numBarriers, barriers);
	gbufferPass(pCommandList);

	numBarriers = m_outputViews[UAV_TABLE_RT_OUT]->SetBarrier(barriers, ResourceState::UNORDERED_ACCESS);
	numBarriers = m_gbuffers[NORMAL]->SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE, numBarriers);
	numBarriers = m_gbuffers[VELOCITY]->SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE,
		numBarriers, BARRIER_ALL_SUBRESOURCES, BarrierFlag::BEGIN_ONLY);
	numBarriers = m_depth->SetBarrier(barriers, ResourceState::DEPTH_READ |
		ResourceState::NON_PIXEL_SHADER_RESOURCE, numBarriers);
	pCommandList->Barrier(numBarriers, barriers);
	rayTrace(pCommandList, frameIndex);

	temporalSS(pCommandList);
}

void RayTracer::ToneMap(const RayTracing::CommandList* pCommandList, const Descriptor& rtv,
	uint32_t numBarriers, ResourceBarrier* pBarriers)
{
	numBarriers = m_outputViews[UAV_TABLE_TSAMP + m_frameParity]->SetBarrier(
		pBarriers, ResourceState::NON_PIXEL_SHADER_RESOURCE |
		ResourceState::PIXEL_SHADER_RESOURCE, numBarriers);
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

void RayTracer::ClearHistory(const RayTracing::CommandList* pCommandList)
{
	ResourceBarrier barriers[FrameCount];
	auto numBarriers = 0u;
	for (auto i = 0ui8; i < 2; ++i)
		numBarriers = m_outputViews[UAV_TABLE_TSAMP + i]->SetBarrier(barriers, ResourceState::UNORDERED_ACCESS, numBarriers);
	pCommandList->Barrier(numBarriers, barriers);

	const float clearColor[4] = {};
	for (auto i = 0ui8; i < 2; ++i)
	{
		const uint8_t j = UAV_TABLE_TSAMP + i;
		pCommandList->ClearUnorderedAccessViewFloat(m_uavTables[j], m_outputViews[j]->GetUAV(),
			m_outputViews[j]->GetResource(), clearColor);
	}
}

bool RayTracer::createVB(RayTracing::CommandList* pCommandList, uint32_t numVert,
	uint32_t stride, const uint8_t* pData, vector<Resource>& uploaders)
{
	auto& vertexBuffer = m_vertexBuffers[MODEL_OBJ];
	vertexBuffer = VertexBuffer::MakeUnique();
	N_RETURN(vertexBuffer->Create(m_device.Common, numVert, stride,
		ResourceFlag::NONE, MemoryType::DEFAULT, 1, nullptr, 1,
		nullptr, 1, nullptr, L"MeshVB"), false);
	uploaders.push_back(nullptr);

	return vertexBuffer->Upload(pCommandList, uploaders.back(), pData,
		stride * numVert, 0, ResourceState::NON_PIXEL_SHADER_RESOURCE);
}

bool RayTracer::createIB(RayTracing::CommandList* pCommandList, uint32_t numIndices,
	const uint32_t* pData, vector<Resource>& uploaders)
{
	m_numIndices[MODEL_OBJ] = numIndices;

	auto& indexBuffers = m_indexBuffers[MODEL_OBJ];
	const uint32_t byteWidth = sizeof(uint32_t) * numIndices;
	indexBuffers = IndexBuffer::MakeUnique();
	N_RETURN(indexBuffers->Create(m_device.Common, byteWidth, Format::R32_UINT, ResourceFlag::NONE,
		MemoryType::DEFAULT, 1, nullptr, 1, nullptr, 1, nullptr, L"MeshIB"), false);
	uploaders.push_back(nullptr);

	return indexBuffers->Upload(pCommandList, uploaders.back(), pData,
		byteWidth, 0, ResourceState::NON_PIXEL_SHADER_RESOURCE);
}

bool RayTracer::createGroundMesh(RayTracing::CommandList* pCommandList, vector<Resource>& uploaders)
{
	// Vertex buffer
	{
		// Cube vertices positions and corresponding triangle normals.
		XMFLOAT3 vertices[][2] =
		{
			{ XMFLOAT3(-1.0f, 1.0f, -1.0f), XMFLOAT3(0.0f, 1.0f, 0.0f) },
			{ XMFLOAT3(1.0f, 1.0f, -1.0f), XMFLOAT3(0.0f, 1.0f, 0.0f) },
			{ XMFLOAT3(1.0f, 1.0f, 1.0f), XMFLOAT3(0.0f, 1.0f, 0.0f) },
			{ XMFLOAT3(-1.0f, 1.0f, 1.0f), XMFLOAT3(0.0f, 1.0f, 0.0f) },

			{ XMFLOAT3(-1.0f, -1.0f, -1.0f), XMFLOAT3(0.0f, -1.0f, 0.0f) },
			{ XMFLOAT3(1.0f, -1.0f, -1.0f), XMFLOAT3(0.0f, -1.0f, 0.0f) },
			{ XMFLOAT3(1.0f, -1.0f, 1.0f), XMFLOAT3(0.0f, -1.0f, 0.0f) },
			{ XMFLOAT3(-1.0f, -1.0f, 1.0f), XMFLOAT3(0.0f, -1.0f, 0.0f) },

			{ XMFLOAT3(-1.0f, -1.0f, 1.0f), XMFLOAT3(-1.0f, 0.0f, 0.0f) },
			{ XMFLOAT3(-1.0f, -1.0f, -1.0f), XMFLOAT3(-1.0f, 0.0f, 0.0f) },
			{ XMFLOAT3(-1.0f, 1.0f, -1.0f), XMFLOAT3(-1.0f, 0.0f, 0.0f) },
			{ XMFLOAT3(-1.0f, 1.0f, 1.0f), XMFLOAT3(-1.0f, 0.0f, 0.0f) },

			{ XMFLOAT3(1.0f, -1.0f, 1.0f), XMFLOAT3(1.0f, 0.0f, 0.0f) },
			{ XMFLOAT3(1.0f, -1.0f, -1.0f), XMFLOAT3(1.0f, 0.0f, 0.0f) },
			{ XMFLOAT3(1.0f, 1.0f, -1.0f), XMFLOAT3(1.0f, 0.0f, 0.0f) },
			{ XMFLOAT3(1.0f, 1.0f, 1.0f), XMFLOAT3(1.0f, 0.0f, 0.0f) },

			{ XMFLOAT3(-1.0f, -1.0f, -1.0f), XMFLOAT3(0.0f, 0.0f, -1.0f) },
			{ XMFLOAT3(1.0f, -1.0f, -1.0f), XMFLOAT3(0.0f, 0.0f, -1.0f) },
			{ XMFLOAT3(1.0f, 1.0f, -1.0f), XMFLOAT3(0.0f, 0.0f, -1.0f) },
			{ XMFLOAT3(-1.0f, 1.0f, -1.0f), XMFLOAT3(0.0f, 0.0f, -1.0f) },

			{ XMFLOAT3(-1.0f, -1.0f, 1.0f), XMFLOAT3(0.0f, 0.0f, 1.0f) },
			{ XMFLOAT3(1.0f, -1.0f, 1.0f), XMFLOAT3(0.0f, 0.0f, 1.0f) },
			{ XMFLOAT3(1.0f, 1.0f, 1.0f), XMFLOAT3(0.0f, 0.0f, 1.0f) },
			{ XMFLOAT3(-1.0f, 1.0f, 1.0f), XMFLOAT3(0.0f, 0.0f, 1.0f) },
		};

		auto& vertexBuffer = m_vertexBuffers[GROUND];
		vertexBuffer = VertexBuffer::MakeUnique();
		N_RETURN(vertexBuffer->Create(m_device.Common, static_cast<uint32_t>(size(vertices)), sizeof(XMFLOAT3[2]),
			ResourceFlag::NONE, MemoryType::DEFAULT, 1, nullptr, 1, nullptr, 1, nullptr, L"GroundVB"), false);
		uploaders.push_back(nullptr);

		N_RETURN(vertexBuffer->Upload(pCommandList, uploaders.back(), vertices,
			sizeof(vertices), 0, ResourceState::NON_PIXEL_SHADER_RESOURCE), false);
	}

	// Index Buffer
	{
		// Cube indices.
		uint32_t indices[] =
		{
			3,1,0,
			2,1,3,

			6,4,5,
			7,4,6,

			11,9,8,
			10,9,11,

			14,12,13,
			15,12,14,

			19,17,16,
			18,17,19,

			22,20,21,
			23,20,22
		};

		m_numIndices[GROUND] = static_cast<uint32_t>(size(indices));

		auto& indexBuffers = m_indexBuffers[GROUND];
		indexBuffers = IndexBuffer::MakeUnique();
		N_RETURN(indexBuffers->Create(m_device.Common, sizeof(indices), Format::R32_UINT, ResourceFlag::NONE,
			MemoryType::DEFAULT, 1, nullptr, 1, nullptr, 1, nullptr, L"GroundIB"), false);
		uploaders.push_back(nullptr);

		N_RETURN(indexBuffers->Upload(pCommandList, uploaders.back(), indices,
			sizeof(indices), 0, ResourceState::NON_PIXEL_SHADER_RESOURCE), false);
	}

	return true;
}

bool RayTracer::createInputLayout()
{
	// Define the vertex input layout.
	InputElementTable inputElementDescs =
	{
		{ "POSITION",	0, Format::R32G32B32_FLOAT, 0, 0,								InputClassification::PER_VERTEX_DATA, 0 },
		{ "NORMAL",		0, Format::R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT,	InputClassification::PER_VERTEX_DATA, 0 }
	};

	X_RETURN(m_inputLayout, m_graphicsPipelineCache->CreateInputLayout(inputElementDescs), false);

	return true;
}

bool RayTracer::createPipelineLayouts()
{
	// Global pipeline layout
	// This is a pipeline layout that is shared across all raytracing shaders invoked during a DispatchRays() call.
	{
		const auto pipelineLayout = RayTracing::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(OUTPUT_VIEW, DescriptorType::UAV, 1, 0);
		pipelineLayout->SetRootSRV(ACCELERATION_STRUCTURE, 0);
		pipelineLayout->SetRange(SAMPLER, DescriptorType::SAMPLER, 1, 0);
		pipelineLayout->SetRange(INDEX_BUFFERS, DescriptorType::SRV, NUM_MESH, 0, 1);
		pipelineLayout->SetRange(VERTEX_BUFFERS, DescriptorType::SRV, NUM_MESH, 0, 2);
		pipelineLayout->SetConstants(CONSTANTS, SizeOfInUint32(GlobalConstants), 0);
		pipelineLayout->SetRange(G_BUFFERS, DescriptorType::SRV, 2, 1);
		X_RETURN(m_pipelineLayouts[GLOBAL_LAYOUT], pipelineLayout->GetPipelineLayout(
			m_device, *m_pipelineLayoutCache, PipelineLayoutFlag::NONE,
			L"RayTracerGlobalPipelineLayout"), false);
	}

	// Local pipeline layout for RayGen shader
	// This is a pipeline layout that enables a shader to have unique arguments that come from shader tables.
	{
		const auto pipelineLayout = RayTracing::PipelineLayout::MakeUnique();
		pipelineLayout->SetConstants(0, SizeOfInUint32(RayGenConstants), 1);
		X_RETURN(m_pipelineLayouts[RAY_GEN_LAYOUT], pipelineLayout->GetPipelineLayout(
			m_device, *m_pipelineLayoutCache, PipelineLayoutFlag::LOCAL_PIPELINE_LAYOUT,
			L"RayTracerRayGenPipelineLayout"), false);
	}

	// This is a pipeline layout for g-buffer pass
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetConstants(0, SizeOfInUint32(BasePassConstants), 0, 0, Shader::Stage::VS);
		pipelineLayout->SetConstants(1, SizeOfInUint32(uint32_t), 0, 0, Shader::Stage::PS);
		X_RETURN(m_pipelineLayouts[GBUFFER_PASS_LAYOUT], pipelineLayout->GetPipelineLayout(*m_pipelineLayoutCache,
			PipelineLayoutFlag::ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT, L"GBufferPipelineLayout"), false);
	}

	// This is a pipeline layout for resampling
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(OUTPUT_VIEW, DescriptorType::UAV, 1, 0, 0,
			DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		pipelineLayout->SetShaderStage(OUTPUT_VIEW, Shader::Stage::CS);
		pipelineLayout->SetRange(SHADER_RESOURCES, DescriptorType::SRV, 1, 0);
		pipelineLayout->SetShaderStage(SHADER_RESOURCES, Shader::Stage::CS);
		pipelineLayout->SetRange(SAMPLER, DescriptorType::SAMPLER, 1, 0);
		pipelineLayout->SetShaderStage(SAMPLER, Shader::Stage::CS);
		X_RETURN(m_pipelineLayouts[RESAMPLE_LAYOUT], pipelineLayout->GetPipelineLayout(*m_pipelineLayoutCache,
			PipelineLayoutFlag::NONE, L"ResamplingPipelineLayout"), false);
	}

	// This is a pipeline layout for temporal SS
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(OUTPUT_VIEW, DescriptorType::UAV, 1, 0, 0,
			DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		pipelineLayout->SetShaderStage(OUTPUT_VIEW, Shader::Stage::CS);
		pipelineLayout->SetRange(SHADER_RESOURCES, DescriptorType::SRV, 4, 0);
		pipelineLayout->SetShaderStage(SHADER_RESOURCES, Shader::Stage::CS);
		pipelineLayout->SetRange(SAMPLER, DescriptorType::SAMPLER, 1, 0);
		pipelineLayout->SetShaderStage(SAMPLER, Shader::Stage::CS);
		X_RETURN(m_pipelineLayouts[TEMPORAL_SS_LAYOUT], pipelineLayout->GetPipelineLayout(*m_pipelineLayoutCache,
			PipelineLayoutFlag::NONE, L"TemporalSSPipelineLayout"), false);
	}

	// This is a pipeline layout for tone mapping
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::SRV, 1, 0);
		pipelineLayout->SetShaderStage(0, Shader::Stage::PS);
		X_RETURN(m_pipelineLayouts[TONE_MAP_LAYOUT], pipelineLayout->GetPipelineLayout(*m_pipelineLayoutCache,
			PipelineLayoutFlag::NONE, L"ToneMappingPipelineLayout"), false);
	}

	return true;
}

bool RayTracer::createPipelines(Format rtFormat)
{
	auto vsIndex = 0u;
	auto psIndex = 0u;
	auto csIndex = 0u;

	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"RayTracedTest.cso"), false);
		
		const auto state = RayTracing::State::MakeUnique();
		state->SetShaderLibrary(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		state->SetHitGroup(0, HitGroupName, ClosestHitShaderName);
		state->SetShaderConfig(sizeof(XMFLOAT4), sizeof(XMFLOAT2));
		state->SetLocalPipelineLayout(0, m_pipelineLayouts[RAY_GEN_LAYOUT],
			1, reinterpret_cast<const void**>(&RaygenShaderName));
		state->SetGlobalPipelineLayout(m_pipelineLayouts[GLOBAL_LAYOUT]);
		state->SetMaxRecursionDepth(1);
		m_rayTracingPipelines[TEST] = state->GetPipeline(*m_rayTracingPipelineCache, L"RaytracingTest");

		N_RETURN(m_rayTracingPipelines[TEST].Native || m_rayTracingPipelines[TEST].Fallback, false);
	}

	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"RayTracedGGX.cso"), false);
		
		const auto state = RayTracing::State::MakeUnique();
		state->SetShaderLibrary(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		state->SetHitGroup(0, HitGroupName, ClosestHitShaderName);
		state->SetShaderConfig(sizeof(XMFLOAT4), sizeof(XMFLOAT2));
		state->SetLocalPipelineLayout(0, m_pipelineLayouts[RAY_GEN_LAYOUT],
			1, reinterpret_cast<const void**>(&RaygenShaderName));
		state->SetGlobalPipelineLayout(m_pipelineLayouts[GLOBAL_LAYOUT]);
		state->SetMaxRecursionDepth(1);
		m_rayTracingPipelines[GGX] = state->GetPipeline(*m_rayTracingPipelineCache, L"RaytracingGGX");

		N_RETURN(m_rayTracingPipelines[GGX].Native || m_rayTracingPipelines[GGX].Fallback, false);
	}

	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::VS, vsIndex, L"VSBasePass.cso"), false);
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, psIndex, L"PSGBuffer.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->IASetInputLayout(m_inputLayout);
		state->SetPipelineLayout(m_pipelineLayouts[GBUFFER_PASS_LAYOUT]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, vsIndex++));
		state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, psIndex++));
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->OMSetNumRenderTargets(2);
		state->OMSetRTVFormat(0, Format::R10G10B10A2_UNORM);
		state->OMSetRTVFormat(1, Format::R16G16_FLOAT);
		state->OMSetDSVFormat(Format::D24_UNORM_S8_UINT);
		X_RETURN(m_pipelines[GBUFFER_PASS], state->GetPipeline(*m_graphicsPipelineCache, L"GBufferPass"), false);
	}

	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"CSTemporalSS.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[TEMPORAL_SS_LAYOUT]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[TEMPORAL_SS], state->GetPipeline(*m_computePipelineCache, L"TemporalSS"), false);
	}

	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"CS_SpatialPass.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[RESAMPLE_LAYOUT]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[SPATIAL_PASS], state->GetPipeline(*m_computePipelineCache, L"SpatialPass"), false);
	}

	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::VS, vsIndex, L"VSScreenQuad.cso"), false);
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, psIndex, L"PSToneMap.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[TONE_MAP_LAYOUT]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, vsIndex++));
		state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, psIndex++));
		state->DSSetState(Graphics::DEPTH_STENCIL_NONE, *m_graphicsPipelineCache);
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->OMSetNumRenderTargets(1);
		state->OMSetRTVFormat(0, rtFormat);
		X_RETURN(m_pipelines[TONE_MAP], state->GetPipeline(*m_graphicsPipelineCache, L"ToneMapping"), false);
	}

	return true;
}

bool RayTracer::createDescriptorTables()
{
	//m_descriptorTableCache.AllocateDescriptorPool(CBV_SRV_UAV_POOL, NumUAVs + NUM_MESH * 2);

	// Acceleration structure UAVs
	{
		Descriptor descriptors[NUM_MESH + 1];
		for (auto i = 0u; i < NUM_MESH; ++i) descriptors[i] = m_bottomLevelASs[i]->GetResult()->GetUAV();
		descriptors[NUM_MESH] = m_topLevelAS->GetResult()->GetUAV();
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		const auto asTable = descriptorTable->GetCbvSrvUavTable(*m_descriptorTableCache);
		N_RETURN(asTable, false);
	}

	// Output UAV
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_outputViews[UAV_TABLE_RT_OUT]->GetUAV());
		X_RETURN(m_uavTables[UAV_TABLE_RT_OUT], descriptorTable->GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	// Spatially resolved UAVs
	for (auto i = 0u; i < 2; ++i)
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_outputViews[UAV_TABLE_SPATIAL + i]->GetUAV());
		X_RETURN(m_uavTables[UAV_TABLE_SPATIAL + i], descriptorTable->GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	// Temporal SS output UAVs
	for (auto i = 0u; i < 2; ++i)
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_outputViews[UAV_TABLE_TSAMP + i]->GetUAV());
		X_RETURN(m_uavTables[UAV_TABLE_TSAMP + i], descriptorTable->GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	// Index buffer SRVs
	{
		Descriptor descriptors[NUM_MESH];
		for (auto i = 0u; i < NUM_MESH; ++i) descriptors[i] = m_indexBuffers[i]->GetSRV();
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		X_RETURN(m_srvTables[SRV_TABLE_IB], descriptorTable->GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	// Vertex buffer SRVs
	{
		Descriptor descriptors[NUM_MESH];
		for (auto i = 0u; i < NUM_MESH; ++i) descriptors[i] = m_vertexBuffers[i]->GetSRV();
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		X_RETURN(m_srvTables[SRV_TABLE_VB], descriptorTable->GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	// G-buffer SRVs
	{
		const Descriptor descriptors[] =
		{
			m_gbuffers[NORMAL]->GetSRV(),
			m_depth->GetSRV()
		};
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		X_RETURN(m_srvTables[SRV_TABLE_GB], descriptorTable->GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	// Spatially resolving input SRVs
	for (auto i = 0u; i < 2; ++i)
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_outputViews[UAV_TABLE_SPATIAL + i]->GetSRV());
		X_RETURN(m_srvTables[SRV_TABLE_SPATIAL + i], descriptorTable->GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	// Temporal SS input SRVs
	for (auto i = 0u; i < 2; ++i)
	{
		const Descriptor descriptors[] =
		{
			m_outputViews[UAV_TABLE_RT_OUT]->GetSRV(),
			m_outputViews[UAV_TABLE_TSAMP + !i]->GetSRV(),
			m_gbuffers[VELOCITY]->GetSRV(),
			m_outputViews[UAV_TABLE_SPATIAL]->GetSRV()
		};
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		X_RETURN(m_srvTables[SRV_TABLE_TS + i], descriptorTable->GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	// Tone mapping SRVs
	for (auto i = 0u; i < 2; ++i)
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_outputViews[UAV_TABLE_TSAMP + i]->GetSRV());
		X_RETURN(m_srvTables[SRV_TABLE_TM + i], descriptorTable->GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	// RTV table
	{
		const Descriptor descriptors[] =
		{
			m_gbuffers[NORMAL]->GetRTV(),
			m_gbuffers[VELOCITY]->GetRTV()
		};
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		m_framebuffer = descriptorTable->GetFramebuffer(*m_descriptorTableCache, & m_depth->GetDSV());
	}

	// Create the sampler
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		const auto samplerAnisoWrap = SamplerPreset::ANISOTROPIC_WRAP;
		descriptorTable->SetSamplers(0, 1, &samplerAnisoWrap, *m_descriptorTableCache);
		X_RETURN(m_samplerTable, descriptorTable->GetSamplerTable(*m_descriptorTableCache), false);
	}

	return true;
}

bool RayTracer::buildAccelerationStructures(const RayTracing::CommandList* pCommandList, Geometry* geometries)
{
	// Set geometries
	VertexBufferView vertexBufferViews[NUM_MESH];
	IndexBufferView indexBufferViews[NUM_MESH];
	for (auto i = 0; i < NUM_MESH; ++i)
	{
		vertexBufferViews[i] = m_vertexBuffers[i]->GetVBV();
		indexBufferViews[i] = m_indexBuffers[i]->GetIBV();
	}
	BottomLevelAS::SetTriangleGeometries(geometries, NUM_MESH, Format::R32G32B32_FLOAT,
		vertexBufferViews, indexBufferViews);

	// Descriptor index in descriptor pool
	const auto bottomLevelASIndex = 0u;
	const auto topLevelASIndex = bottomLevelASIndex + NUM_MESH;

	// Prebuild
	for (auto i = 0; i < NUM_MESH; ++i)
	{
		m_bottomLevelASs[i] = BottomLevelAS::MakeUnique();
		N_RETURN(m_bottomLevelASs[i]->PreBuild(m_device, 1, &geometries[i], bottomLevelASIndex + i), false);
	}
	m_topLevelAS = TopLevelAS::MakeUnique();
	N_RETURN(m_topLevelAS->PreBuild(m_device, NUM_MESH, topLevelASIndex,
		BuildFlags::ALLOW_UPDATE | BuildFlags::PREFER_FAST_TRACE), false);

	// Create scratch buffer
	auto scratchSize = m_topLevelAS->GetScratchDataMaxSize();
	for (const auto& bottomLevelAS : m_bottomLevelASs)
		scratchSize = (max)(bottomLevelAS->GetScratchDataMaxSize(), scratchSize);
	N_RETURN(AccelerationStructure::AllocateUAVBuffer(m_device, m_scratch, scratchSize), false);

	// Get descriptor pool and create descriptor tables
	N_RETURN(createDescriptorTables(), false);
	const auto& descriptorPool = m_descriptorTableCache->GetDescriptorPool(CBV_SRV_UAV_POOL);

	// Set instance
	XMFLOAT4X4 matrices[NUM_MESH];
	XMStoreFloat4x4(&matrices[GROUND], XMMatrixTranspose((XMMatrixScaling(8.0f, 0.5f, 8.0f) * XMMatrixTranslation(0.0f, -0.5f, 0.0f))));
	XMStoreFloat4x4(&matrices[MODEL_OBJ], XMMatrixTranspose((XMMatrixScaling(m_posScale.w, m_posScale.w, m_posScale.w) *
		XMMatrixTranslation(m_posScale.x, m_posScale.y, m_posScale.z))));
	float* const transforms[] =
	{
		reinterpret_cast<float*>(&matrices[GROUND]),
		reinterpret_cast<float*>(&matrices[MODEL_OBJ])
	};
	auto& instances = m_instances[FrameCount - 1];
	const BottomLevelAS* ppBottomLevelASs[NUM_MESH];
	for (auto i = 0u; i < NUM_MESH; ++i) ppBottomLevelASs[i] = m_bottomLevelASs[i].get();
	TopLevelAS::SetInstances(m_device, instances, NUM_MESH, ppBottomLevelASs, transforms);

	// Build bottom level ASs
	for (auto& bottomLevelAS : m_bottomLevelASs)
		bottomLevelAS->Build(pCommandList, m_scratch, descriptorPool);

	// Build top level AS
	m_topLevelAS->Build(pCommandList, m_scratch, instances, descriptorPool);

	return true;
}

bool RayTracer::buildShaderTables()
{
	// Get shader identifiers.
	const auto shaderIDSize = ShaderRecord::GetShaderIDSize(m_device);

	for (auto k = 0ui8; k < NUM_RAYTRACE_PIPELINE; ++k)
	{
		for (auto i = 0ui8; i < FrameCount; ++i)
		{
			// Ray gen shader table
			m_rayGenShaderTables[i][k] = ShaderTable::MakeUnique();
			N_RETURN(m_rayGenShaderTables[i][k]->Create(m_device, 1, shaderIDSize + sizeof(RayGenConstants),
				(L"RayGenShaderTable" + to_wstring(i)).c_str()), false);
			N_RETURN(m_rayGenShaderTables[i][k]->AddShaderRecord(*ShaderRecord::MakeUnique(m_device,
				m_rayTracingPipelines[k], RaygenShaderName, &RayGenConstants(), sizeof(RayGenConstants))), false);

			// Hit group shader table
			m_hitGroupShaderTables[i][k] = ShaderTable::MakeUnique();
			N_RETURN(m_hitGroupShaderTables[i][k]->Create(m_device, 1, shaderIDSize, L"HitGroupShaderTable"), false);
			N_RETURN(m_hitGroupShaderTables[i][k]->AddShaderRecord(*ShaderRecord::MakeUnique(m_device,
				m_rayTracingPipelines[k], HitGroupName)), false);
		}

		// Miss shader table
		m_missShaderTables[k] = ShaderTable::MakeUnique();
		N_RETURN(m_missShaderTables[k]->Create(m_device, 1, shaderIDSize, L"MissShaderTable"), false);
		N_RETURN(m_missShaderTables[k]->AddShaderRecord(*ShaderRecord::MakeUnique(m_device,
			m_rayTracingPipelines[k], MissShaderName)), false);
	}

	return true;
}

void RayTracer::updateAccelerationStructures(const RayTracing::CommandList* pCommandList, uint32_t frameIndex)
{
	// Set instance
	float* const transforms[] =
	{
		reinterpret_cast<float*>(&m_worlds[GROUND]),
		reinterpret_cast<float*>(&m_worlds[MODEL_OBJ])
	};
	const BottomLevelAS* pBottomLevelASs[NUM_MESH];
	for (auto i = 0u; i < NUM_MESH; ++i) pBottomLevelASs[i] = m_bottomLevelASs[i].get();
	TopLevelAS::SetInstances(m_device, m_instances[frameIndex], NUM_MESH, pBottomLevelASs, transforms);

	// Update top level AS
	const auto& descriptorPool = m_descriptorTableCache->GetDescriptorPool(CBV_SRV_UAV_POOL);
	m_topLevelAS->Build(pCommandList, m_scratch, m_instances[frameIndex], descriptorPool, true);
}

void RayTracer::rayTrace(const RayTracing::CommandList* pCommandList, uint32_t frameIndex)
{
	pCommandList->SetComputePipelineLayout(m_pipelineLayouts[GLOBAL_LAYOUT]);

	// Bind the heaps, acceleration structure and dispatch rays.
	const DescriptorPool descriptorPools[] =
	{
		m_descriptorTableCache->GetDescriptorPool(CBV_SRV_UAV_POOL),
		m_descriptorTableCache->GetDescriptorPool(SAMPLER_POOL)
	};
	pCommandList->SetDescriptorPools(static_cast<uint32_t>(size(descriptorPools)), descriptorPools);

	pCommandList->SetComputeDescriptorTable(OUTPUT_VIEW, m_uavTables[UAV_TABLE_RT_OUT]);
	pCommandList->SetTopLevelAccelerationStructure(ACCELERATION_STRUCTURE, *m_topLevelAS);
	pCommandList->SetComputeDescriptorTable(SAMPLER, m_samplerTable);
	pCommandList->SetComputeDescriptorTable(INDEX_BUFFERS, m_srvTables[SRV_TABLE_IB]);
	pCommandList->SetComputeDescriptorTable(VERTEX_BUFFERS, m_srvTables[SRV_TABLE_VB]);
	pCommandList->SetCompute32BitConstants(CONSTANTS, SizeOfInUint32(GlobalConstants), &m_cbRaytracing);
	pCommandList->SetComputeDescriptorTable(G_BUFFERS, m_srvTables[SRV_TABLE_GB]);

	// Fallback layer has no depth
	pCommandList->DispatchRays(m_rayTracingPipelines[m_pipeIndex], m_viewport.x,
		m_viewport.y, 1, *m_hitGroupShaderTables[frameIndex][m_pipeIndex],
		*m_missShaderTables[m_pipeIndex], *m_rayGenShaderTables[frameIndex][m_pipeIndex]);
}

void RayTracer::gbufferPass(const RayTracing::CommandList* pCommandList)
{
	// Set framebuffer
	pCommandList->OMSetFramebuffer(m_framebuffer);

	// Clear render target
	const float clearColor[4] = {};
	pCommandList->ClearRenderTargetView(m_gbuffers[NORMAL]->GetRTV(), clearColor);
	pCommandList->ClearRenderTargetView(m_gbuffers[VELOCITY]->GetRTV(), clearColor);
	pCommandList->ClearDepthStencilView(m_depth->GetDSV(), ClearFlag::DEPTH, 1.0f);

	// Set pipeline state
	pCommandList->SetGraphicsPipelineLayout(m_pipelineLayouts[GBUFFER_PASS_LAYOUT]);
	pCommandList->SetPipelineState(m_pipelines[GBUFFER_PASS]);

	// Set viewport
	Viewport viewport(0.0f, 0.0f, static_cast<float>(m_viewport.x), static_cast<float>(m_viewport.y));
	RectRange scissorRect(0, 0, m_viewport.x, m_viewport.y);
	pCommandList->RSSetViewports(1, &viewport);
	pCommandList->RSSetScissorRects(1, &scissorRect);

	pCommandList->IASetPrimitiveTopology(PrimitiveTopology::TRIANGLELIST);

	for (auto i = 0ui8; i < NUM_MESH; ++i)
	{
		// Set descriptor tables
		pCommandList->SetGraphics32BitConstants(0, SizeOfInUint32(BasePassConstants), &m_cbBasePass[i]);
		pCommandList->SetGraphics32BitConstant(1, i);

		pCommandList->IASetVertexBuffers(0, 1, &m_vertexBuffers[i]->GetVBV());
		pCommandList->IASetIndexBuffer(m_indexBuffers[i]->GetIBV());

		pCommandList->DrawIndexed(m_numIndices[i], 1, 0, 0, 0);
	}
}

void RayTracer::spatialPass(const RayTracing::CommandList* pCommandList, uint8_t dst, uint8_t src, uint8_t srcSRV)
{
	pCommandList->SetComputePipelineLayout(m_pipelineLayouts[RESAMPLE_LAYOUT]);

	// Bind the heaps, acceleration structure and dispatch rays.
	const DescriptorPool descriptorPools[] =
	{
		m_descriptorTableCache->GetDescriptorPool(CBV_SRV_UAV_POOL),
		m_descriptorTableCache->GetDescriptorPool(SAMPLER_POOL)
	};
	pCommandList->SetDescriptorPools(static_cast<uint32_t>(size(descriptorPools)), descriptorPools);

	ResourceBarrier barriers[2];
	auto numBarriers = m_outputViews[dst]->SetBarrier(barriers, ResourceState::UNORDERED_ACCESS);
	numBarriers = m_outputViews[src]->SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE, numBarriers);
	pCommandList->Barrier(numBarriers, barriers);

	pCommandList->SetComputeDescriptorTable(OUTPUT_VIEW, m_uavTables[dst]);
	pCommandList->SetComputeDescriptorTable(SHADER_RESOURCES, m_srvTables[srcSRV]);
	pCommandList->SetComputeDescriptorTable(SAMPLER, m_samplerTable);

	if (srcSRV != SRV_TABLE_SPATIAL && srcSRV != SRV_TABLE_SPATIAL1)
		pCommandList->SetPipelineState(m_pipelines[SPATIAL_PASS]);
	pCommandList->Dispatch(DIV_UP(m_viewport.x, 8), DIV_UP(m_viewport.y, 8), 1);
}

void RayTracer::temporalSS(const RayTracing::CommandList* pCommandList)
{
	pCommandList->SetComputePipelineLayout(m_pipelineLayouts[TEMPORAL_SS_LAYOUT]);

	// Bind the heaps, acceleration structure and dispatch rays.
	const DescriptorPool descriptorPools[] =
	{
		m_descriptorTableCache->GetDescriptorPool(CBV_SRV_UAV_POOL),
		m_descriptorTableCache->GetDescriptorPool(SAMPLER_POOL)
	};
	pCommandList->SetDescriptorPools(static_cast<uint32_t>(size(descriptorPools)), descriptorPools);

	ResourceBarrier barriers[5];
	auto numBarriers = m_outputViews[UAV_TABLE_TSAMP + m_frameParity]->SetBarrier(barriers, ResourceState::UNORDERED_ACCESS);
	numBarriers = m_outputViews[UAV_TABLE_RT_OUT]->SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE, numBarriers);
	numBarriers = m_outputViews[UAV_TABLE_TSAMP + !m_frameParity]->SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE |
		ResourceState::PIXEL_SHADER_RESOURCE, numBarriers);
	numBarriers = m_gbuffers[VELOCITY]->SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE,
		numBarriers, BARRIER_ALL_SUBRESOURCES, BarrierFlag::END_ONLY);
	pCommandList->Barrier(numBarriers, barriers);

	pCommandList->SetComputeDescriptorTable(OUTPUT_VIEW, m_uavTables[UAV_TABLE_TSAMP + m_frameParity]);
	pCommandList->SetComputeDescriptorTable(SHADER_RESOURCES, m_srvTables[SRV_TABLE_TS + m_frameParity]);
	pCommandList->SetComputeDescriptorTable(SAMPLER, m_samplerTable);

	pCommandList->SetPipelineState(m_pipelines[TEMPORAL_SS]);
	pCommandList->Dispatch(DIV_UP(m_viewport.x, 8), DIV_UP(m_viewport.y, 8), 1);
}
