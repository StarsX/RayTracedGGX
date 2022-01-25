//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "RayTracer.h"
#include "Optional/XUSGObjLoader.h"
#define _INDEPENDENT_DDS_LOADER_
#include "Advanced/XUSGDDSLoader.h"
#undef _INDEPENDENT_DDS_LOADER_
#define _INDEPENDENT_HALTON_
#include "Advanced/XUSGHalton.h"
#undef _INDEPENDENT_HALTON_
#include "DirectXPackedVector.h"

using namespace std;
using namespace DirectX;
using namespace XUSG;
using namespace XUSG::RayTracing;

struct Vertex
{
	XMFLOAT3 Pos;
	XMFLOAT3 Nrm;
};

struct RayGenConstants
{
	XMMATRIX	ProjToWorld;
	XMVECTOR	EyePt;
	XMFLOAT2	ProjBias;
};

struct CBGlobal
{
	XMFLOAT4X4	WorldViewProjs[RayTracer::NUM_MESH];
	XMFLOAT4X4	WorldViewProjsPrev[RayTracer::NUM_MESH];
	XMFLOAT3X4	Worlds[RayTracer::NUM_MESH];
	XMFLOAT3X4	WorldITs[RayTracer::NUM_MESH - 1];
	float		WorldIT[11];
	uint32_t	FrameIndex;
};

struct CBPerObject
{
	XMFLOAT4X4	WorldViewProj;
	XMFLOAT2	ProjBias;
};

struct CBMaterial
{
	XMFLOAT4	BaseColors[RayTracer::NUM_MESH];
	XMFLOAT4	RoughMetals[RayTracer::NUM_MESH];
};

const wchar_t* RayTracer::HitGroupNames[] = { L"hitGroupReflection", L"hitGroupDiffuse" };
const wchar_t* RayTracer::RaygenShaderName = L"raygenMain";
const wchar_t* RayTracer::ClosestHitShaderNames[] = { L"closestHitReflection", L"closestHitDiffuse" };
const wchar_t* RayTracer::MissShaderName = L"missMain";

RayTracer::RayTracer() :
	m_instances()
{
	m_shaderPool = ShaderPool::MakeUnique();
	AccelerationStructure::SetUAVCount(NUM_HIT_GROUP + NUM_GBUFFER + NUM_MESH + 1);
}

RayTracer::~RayTracer()
{
}

bool RayTracer::Init(RayTracing::CommandList* pCommandList, uint32_t width, uint32_t height,
	vector<Resource::uptr>& uploaders, GeometryBuffer* pGeometries, const char* fileName,
	const wchar_t* envFileName, Format rtFormat, const XMFLOAT4& posScale,
	uint8_t maxGBufferMips)
{
	const auto pDevice = pCommandList->GetRTDevice();
	m_rayTracingPipelineCache = RayTracing::PipelineCache::MakeUnique(pDevice);
	m_graphicsPipelineCache = Graphics::PipelineCache::MakeUnique(pDevice);
	m_computePipelineCache = Compute::PipelineCache::MakeUnique(pDevice);
	m_pipelineLayoutCache = PipelineLayoutCache::MakeUnique(pDevice);
	m_descriptorTableCache = DescriptorTableCache::MakeUnique(pDevice, L"RayTracerDescriptorTableCache");

	m_viewport = XMUINT2(width, height);
	m_posScale = posScale;

	// Load inputs
	ObjLoader objLoader;
	if (!objLoader.Import(fileName, true, true)) return false;
	N_RETURN(createVB(pCommandList, objLoader.GetNumVertices(), objLoader.GetVertexStride(), objLoader.GetVertices(), uploaders), false);
	N_RETURN(createIB(pCommandList, objLoader.GetNumIndices(), objLoader.GetIndices(), uploaders), false);

	N_RETURN(createGroundMesh(pCommandList, uploaders), false);

	// Create output views
	for (uint8_t i = 0; i < NUM_HIT_GROUP; ++i)
	{
		auto& outputView = m_outputViews[i];
		outputView = Texture2D::MakeUnique();
		N_RETURN(outputView->Create(pDevice, width, height, Format::R11G11B10_FLOAT, 1,
			ResourceFlag::ALLOW_UNORDERED_ACCESS, 1, 1, false, MemoryFlag::NONE,
			(L"RayTracingOut" + to_wstring(i)).c_str()), false);
	}

	m_visBuffer = RenderTarget::MakeUnique();
	N_RETURN(m_visBuffer->Create(pDevice, width, height, Format::R32_UINT,
		1, ResourceFlag::NONE, 1, 1, nullptr, false, MemoryFlag::NONE, L"VisibilityBuffer"), false);

	uint8_t mipCount = max<uint8_t>(Log2((max)(width, height)), 0) + 1;
	mipCount = (min)(mipCount, maxGBufferMips);
	for (auto& renderTarget : m_gbuffers) renderTarget = RenderTarget::MakeUnique();
	N_RETURN(m_gbuffers[NORMAL]->Create(pDevice, width, height, Format::R10G10B10A2_UNORM, 1,
		ResourceFlag::ALLOW_UNORDERED_ACCESS, mipCount, 1, nullptr, false, MemoryFlag::NONE, L"Normal"), false);
	N_RETURN(m_gbuffers[ROUGH_METAL]->Create(pDevice, width, height, Format::R8G8_UNORM, 1,
		ResourceFlag::ALLOW_UNORDERED_ACCESS, mipCount, 1, nullptr, false, MemoryFlag::NONE, L"RoughnessMetallic"), false);
	N_RETURN(m_gbuffers[VELOCITY]->Create(pDevice, width, height, Format::R16G16_FLOAT, 1,
		ResourceFlag::ALLOW_UNORDERED_ACCESS, 1, 1, nullptr, false, MemoryFlag::NONE, L"Velocity"), false);

	const auto dsFormat = Format::D24_UNORM_S8_UINT;
	m_depth = DepthStencil::MakeShared();
	N_RETURN(m_depth->Create(pDevice, width, height, dsFormat, ResourceFlag::NONE,
		1, 1, 1, 1.0f, 0, false, MemoryFlag::NONE, L"Depth"), false);

	// Constant buffers
	for (auto i = 0u; i < NUM_MESH; ++i)
	{
		auto& cbBasePass = m_cbPerOjects[i];
		cbBasePass = ConstantBuffer::MakeUnique();
		N_RETURN(cbBasePass->Create(pDevice, sizeof(CBPerObject[FrameCount]), FrameCount, nullptr,
			MemoryType::UPLOAD, MemoryFlag::NONE, (L"CBBasePass" + to_wstring(i)).c_str()), false);
	}

	m_cbRaytracing = ConstantBuffer::MakeUnique();
	N_RETURN(m_cbRaytracing->Create(pDevice, sizeof(CBGlobal[FrameCount]), FrameCount,
		nullptr, MemoryType::UPLOAD, MemoryFlag::NONE, L"CBGlobal"), false);

	m_cbMaterials = ConstantBuffer::MakeUnique();
	N_RETURN(m_cbMaterials->Create(pDevice, sizeof(CBMaterial), 1,
		nullptr, MemoryType::UPLOAD, MemoryFlag::NONE, L"CBMaterial"), false);
	{
		const auto pCbData = reinterpret_cast<CBMaterial*>(m_cbMaterials->Map());
		pCbData->BaseColors[0] = XMFLOAT4(0.95f, 0.93f, 0.88f, 1.0f);	// Silver
		//pCbData->BaseColors[0] = XMFLOAT4(powf(0.92549f, 2.2f), powf(0.69412f, 2.2f), powf(0.6745f, 2.2f), 1.0f); // Rose gold
		//pCbData->BaseColors[0] = XMFLOAT4(0.72f, 0.43f, 0.47f, 1.0f);	// Rose gold
		pCbData->BaseColors[1] = XMFLOAT4(1.0f, 0.71f, 0.29f, 1.0f);	// Gold
		pCbData->RoughMetals[0] = XMFLOAT4(0.5f, 1.0f, 0.0f, 0.0f);
		pCbData->RoughMetals[1] = XMFLOAT4(0.16f, 1.0f, 0.0f, 0.0f);
	}

	// Load input image
	{
		DDS::Loader textureLoader;
		DDS::AlphaMode alphaMode;

		uploaders.emplace_back(Resource::MakeUnique());
		N_RETURN(textureLoader.CreateTextureFromFile(pCommandList, envFileName,
			8192, false, m_lightProbe, uploaders.back().get(), &alphaMode), false);
	}

	// Create raytracing pipelines
	N_RETURN(createInputLayout(), false);
	N_RETURN(createPipelineLayouts(pDevice), false);
	N_RETURN(createPipelines(rtFormat, dsFormat), false);

	// Build acceleration structures
	N_RETURN(buildAccelerationStructures(pCommandList, pGeometries), false);
	N_RETURN(buildShaderTables(pDevice), false);

	return true;
}

void RayTracer::SetMetallic(uint32_t meshIdx, float metallic)
{
	const auto pCbData = reinterpret_cast<CBMaterial*>(m_cbMaterials->Map());
	pCbData->RoughMetals[meshIdx].y = metallic;
}

void RayTracer::UpdateFrame(const RayTracing::Device* pDevice, uint8_t frameIndex,
	CXMVECTOR eyePt, CXMMATRIX viewProj, float timeStep)
{
	const auto halton = IncrementalHalton();
	XMFLOAT2 projBias =
	{
		(halton.x * 2.0f - 1.0f) / m_viewport.x,
		(halton.y * 2.0f - 1.0f) / m_viewport.y
	};

	{
		const auto projToWorld = XMMatrixInverse(nullptr, viewProj);
		RayGenConstants cbRayGen = { XMMatrixTranspose(projToWorld), eyePt, projBias };

		m_rayGenShaderTables[frameIndex]->Reset();
		m_rayGenShaderTables[frameIndex]->AddShaderRecord(ShaderRecord::MakeUnique(pDevice,
			m_pipelines[RAY_TRACING], RaygenShaderName, &cbRayGen, sizeof(cbRayGen)).get());
	}

	{
		static auto angle = 0.0f;
		angle += 16.0f * timeStep * XM_PI / 180.0f;
		const auto rot = XMMatrixRotationY(angle);

		XMMATRIX worlds[NUM_MESH] =
		{
			XMMatrixScaling(10.0f, 0.5f, 10.0f) * XMMatrixTranslation(0.0f, -0.5f, 0.0f),
			XMMatrixScaling(m_posScale.w, m_posScale.w, m_posScale.w) * rot *
			XMMatrixTranslation(m_posScale.x, m_posScale.y, m_posScale.z)
		};

		{
			static auto s_frameIndex = 0u;
			const auto pCbData = reinterpret_cast<CBGlobal*>(m_cbRaytracing->Map(frameIndex));
			const auto n = 256u;
			for (auto i = 0u; i < NUM_MESH; ++i)
			{
				pCbData->WorldViewProjsPrev[i] = m_worldViewProjs[i];
				XMStoreFloat4x4(&m_worlds[i], XMMatrixTranspose(worlds[i]));
				XMStoreFloat4x4(&pCbData->WorldViewProjs[i], XMMatrixTranspose(worlds[i] * viewProj));
				XMStoreFloat3x4(&pCbData->Worlds[i], worlds[i]);
				XMStoreFloat3x4(&pCbData->WorldITs[i], i ? rot : XMMatrixIdentity());
				m_worldViewProjs[i] = pCbData->WorldViewProjs[i];
			}
			pCbData->FrameIndex = s_frameIndex++;
			s_frameIndex %= n;
		}

		for (auto i = 0u; i < NUM_MESH; ++i)
		{
			const auto pCbData = reinterpret_cast<CBPerObject*>(m_cbPerOjects[i]->Map(frameIndex));
			XMStoreFloat4x4(&pCbData->WorldViewProj, XMMatrixTranspose(worlds[i] * viewProj));
			pCbData->ProjBias = projBias;
		}
	}
}

void RayTracer::Render(RayTracing::CommandList* pCommandList, uint8_t frameIndex)
{
	RenderVisibility(pCommandList, frameIndex);
	rayTrace(pCommandList, frameIndex);

	ResourceBarrier barriers[5];
	auto numBarriers = m_depth->SetBarrier(barriers, ResourceState::DEPTH_READ);
	for (uint8_t i = 0; i < VELOCITY; ++i)
		numBarriers = m_gbuffers[i]->SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE, numBarriers, 0);
	numBarriers = m_gbuffers[VELOCITY]->SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE,
		numBarriers, BARRIER_ALL_SUBRESOURCES, BarrierFlag::BEGIN_ONLY);
	pCommandList->Barrier(numBarriers, barriers);
}

void RayTracer::UpdateAccelerationStructures(const RayTracing::CommandList* pCommandList, uint8_t frameIndex)
{
	// Set instance
	float* const transforms[] =
	{
		reinterpret_cast<float*>(&m_worlds[GROUND]),
		reinterpret_cast<float*>(&m_worlds[MODEL_OBJ])
	};
	const BottomLevelAS* pBottomLevelASs[NUM_MESH];
	for (auto i = 0u; i < NUM_MESH; ++i) pBottomLevelASs[i] = m_bottomLevelASs[i].get();
	TopLevelAS::SetInstances(pCommandList->GetRTDevice(), m_instances[frameIndex].get(), NUM_MESH, pBottomLevelASs, transforms);

	// Update top level AS
	const auto& descriptorPool = m_descriptorTableCache->GetDescriptorPool(CBV_SRV_UAV_POOL);
	m_topLevelAS->Build(pCommandList, m_scratch.get(), m_instances[frameIndex].get(), descriptorPool, true);
}

void RayTracer::RenderVisibility(RayTracing::CommandList* pCommandList, uint8_t frameIndex)
{
	// Bind the heaps
	const DescriptorPool descriptorPools[] =
	{
		m_descriptorTableCache->GetDescriptorPool(CBV_SRV_UAV_POOL),
		m_descriptorTableCache->GetDescriptorPool(SAMPLER_POOL)
	};
	pCommandList->SetDescriptorPools(static_cast<uint32_t>(size(descriptorPools)), descriptorPools);

	visibility(pCommandList, frameIndex);

	// Set barriers
	ResourceBarrier barriers[7];
	auto numBarriers = m_visBuffer->SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE);
	for (auto& outputView : m_outputViews)
		numBarriers = outputView->SetBarrier(barriers, ResourceState::UNORDERED_ACCESS, numBarriers);
	for (auto& gbuffer : m_gbuffers)
		numBarriers = gbuffer->SetBarrier(barriers, ResourceState::UNORDERED_ACCESS, numBarriers, 0);
	// numBarriers = m_depth->SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE, numBarriers);
	pCommandList->Barrier(numBarriers, barriers);
}

void RayTracer::RayTrace(RayTracing::CommandList* pCommandList, uint8_t frameIndex)
{
	// Bind the heaps
	const DescriptorPool descriptorPools[] =
	{
		m_descriptorTableCache->GetDescriptorPool(CBV_SRV_UAV_POOL),
		m_descriptorTableCache->GetDescriptorPool(SAMPLER_POOL)
	};
	pCommandList->SetDescriptorPools(static_cast<uint32_t>(size(descriptorPools)), descriptorPools);

	rayTrace(pCommandList, frameIndex);

	ResourceBarrier barriers[4];
	auto numBarriers = m_gbuffers[VELOCITY]->SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE,
		0, BARRIER_ALL_SUBRESOURCES, BarrierFlag::BEGIN_ONLY);
	for (uint8_t i = 0; i < VELOCITY; ++i)
		numBarriers = m_gbuffers[i]->SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE, numBarriers, 0);
	pCommandList->Barrier(numBarriers, barriers);
}

const Texture2D::uptr* RayTracer::GetRayTracingOutputs() const
{
	return m_outputViews;
}

const RenderTarget::uptr* RayTracer::GetGBuffers() const
{
	return m_gbuffers;
}

const DepthStencil::sptr RayTracer::GetDepth() const
{
	return m_depth;
}

bool RayTracer::createVB(XUSG::CommandList* pCommandList, uint32_t numVert,
	uint32_t stride, const uint8_t* pData, vector<Resource::uptr>& uploaders)
{
	auto& vertexBuffer = m_vertexBuffers[MODEL_OBJ];
	vertexBuffer = VertexBuffer::MakeUnique();
	N_RETURN(vertexBuffer->Create(pCommandList->GetDevice(), numVert, sizeof(Vertex),
		ResourceFlag::NONE, MemoryType::DEFAULT, 1, nullptr, 1,
		nullptr, 1, nullptr, MemoryFlag::NONE, L"MeshVB"), false);
	uploaders.emplace_back(Resource::MakeUnique());

	return vertexBuffer->Upload(pCommandList, uploaders.back().get(), pData,
		sizeof(Vertex) * numVert, 0, ResourceState::NON_PIXEL_SHADER_RESOURCE);
}

bool RayTracer::createIB(XUSG::CommandList* pCommandList, uint32_t numIndices,
	const uint32_t* pData, vector<Resource::uptr>& uploaders)
{
	m_numIndices[MODEL_OBJ] = numIndices;

	auto& indexBuffers = m_indexBuffers[MODEL_OBJ];
	const uint32_t byteWidth = sizeof(uint32_t) * numIndices;
	indexBuffers = IndexBuffer::MakeUnique();
	N_RETURN(indexBuffers->Create(pCommandList->GetDevice(), byteWidth, Format::R32_UINT, ResourceFlag::NONE,
		MemoryType::DEFAULT, 1, nullptr, 1, nullptr, 1, nullptr, MemoryFlag::NONE, L"MeshIB"), false);
	uploaders.emplace_back(Resource::MakeUnique());

	return indexBuffers->Upload(pCommandList, uploaders.back().get(), pData,
		byteWidth, 0, ResourceState::NON_PIXEL_SHADER_RESOURCE);
}

bool RayTracer::createGroundMesh(XUSG::CommandList* pCommandList, vector<Resource::uptr>& uploaders)
{
	const auto pDevice = pCommandList->GetDevice();

	// Vertex buffer
	{
		// Cube vertices positions and corresponding triangle normals.
		Vertex vertices[] =
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
		N_RETURN(vertexBuffer->Create(pDevice, static_cast<uint32_t>(size(vertices)),
			sizeof(Vertex), ResourceFlag::NONE, MemoryType::DEFAULT, 1, nullptr, 1, nullptr,
			1, nullptr, MemoryFlag::NONE, L"GroundVB"), false);
		uploaders.push_back(Resource::MakeUnique());

		N_RETURN(vertexBuffer->Upload(pCommandList, uploaders.back().get(), vertices,
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
		N_RETURN(indexBuffers->Create(pDevice, sizeof(indices), Format::R32_UINT, ResourceFlag::NONE,
			MemoryType::DEFAULT, 1, nullptr, 1, nullptr, 1, nullptr, MemoryFlag::NONE, L"GroundIB"), false);
		uploaders.push_back(Resource::MakeUnique());

		N_RETURN(indexBuffers->Upload(pCommandList, uploaders.back().get(), indices,
			sizeof(indices), 0, ResourceState::NON_PIXEL_SHADER_RESOURCE), false);
	}

	return true;
}

bool RayTracer::createInputLayout()
{
	// Define the vertex input layout.
	const InputElement inputElements[] =
	{
		{ "POSITION",	0, Format::R32G32B32_FLOAT,	0, 0,								InputClassification::PER_VERTEX_DATA, 0 },
		{ "NORMAL",		0, Format::R32G32B32_FLOAT,	0, D3D12_APPEND_ALIGNED_ELEMENT,	InputClassification::PER_VERTEX_DATA, 0 }
	};

	X_RETURN(m_pInputLayout, m_graphicsPipelineCache->CreateInputLayout(inputElements, static_cast<uint32_t>(size(inputElements))), false);

	return true;
}

bool RayTracer::createPipelineLayouts(const RayTracing::Device* pDevice)
{
	// This is a pipeline layout for visibility-buffer pass
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRootCBV(0, 0, 0, Shader::Stage::VS);
		pipelineLayout->SetConstants(1, SizeOfInUint32(uint32_t), 0, 0, Shader::Stage::PS);
		X_RETURN(m_pipelineLayouts[VISIBILITY_LAYOUT], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT, L"VisibilityPipelineLayout"), false);
	}

	// Global pipeline layout
	// This is a pipeline layout that is shared across all raytracing shaders invoked during a DispatchRays() call.
	{
		const auto pipelineLayout = RayTracing::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(OUTPUT_VIEW, DescriptorType::UAV, 5, 0);
		pipelineLayout->SetRootSRV(ACCELERATION_STRUCTURE, 0, 0, DescriptorFlag::DATA_STATIC);
		pipelineLayout->SetRange(SAMPLER, DescriptorType::SAMPLER, 1, 0);
		pipelineLayout->SetRange(INDEX_BUFFERS, DescriptorType::SRV, NUM_MESH, 0, 1);
		pipelineLayout->SetRange(VERTEX_BUFFERS, DescriptorType::SRV, NUM_MESH, 0, 2);
		pipelineLayout->SetRootCBV(MATERIALS, 0);
		pipelineLayout->SetRootCBV(CONSTANTS, 1);
		pipelineLayout->SetRange(SHADER_RESOURCES, DescriptorType::SRV, 2, 1);
		X_RETURN(m_pipelineLayouts[RT_GLOBAL_LAYOUT], pipelineLayout->GetPipelineLayout(
			pDevice, m_pipelineLayoutCache.get(), PipelineLayoutFlag::NONE,
			L"RayTracerVGlobalPipelineLayout"), false);
	}

	// Local pipeline layout for RayGen shader
	// This is a pipeline layout that enables a shader to have unique arguments that come from shader tables.
	{
		const auto pipelineLayout = RayTracing::PipelineLayout::MakeUnique();
		pipelineLayout->SetConstants(0, SizeOfInUint32(RayGenConstants), 2);
		X_RETURN(m_pipelineLayouts[RAY_GEN_LAYOUT], pipelineLayout->GetPipelineLayout(
			pDevice, m_pipelineLayoutCache.get(), PipelineLayoutFlag::LOCAL_PIPELINE_LAYOUT,
			L"RayTracerRayGenPipelineLayout"), false);
	}

	return true;
}

bool RayTracer::createPipelines(Format rtFormat, Format dsFormat)
{
	auto vsIndex = 0u;
	auto psIndex = 0u;
	auto csIndex = 0u;

	// Visibility-buffer pass
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::VS, vsIndex, L"VSVisibility.cso"), false);
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, psIndex, L"PSVisibility.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->IASetInputLayout(m_pInputLayout);
		state->SetPipelineLayout(m_pipelineLayouts[VISIBILITY_LAYOUT]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, vsIndex++));
		state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, psIndex++));
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->OMSetNumRenderTargets(1);
		state->OMSetRTVFormat(0, Format::R32_UINT);
		state->OMSetDSVFormat(dsFormat);
		X_RETURN(m_pipelines[VISIBILITY], state->GetPipeline(m_graphicsPipelineCache.get(), L"VisibilityPass"), false);
	}

	// Ray tracing pass
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"RayTracing.cso"), false);

		const auto state = RayTracing::State::MakeUnique();
		state->SetShaderLibrary(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		state->SetHitGroup(HIT_GROUP_REFLECTION, HitGroupNames[HIT_GROUP_REFLECTION], ClosestHitShaderNames[HIT_GROUP_REFLECTION]);
		state->SetHitGroup(HIT_GROUP_DIFFUSE, HitGroupNames[HIT_GROUP_DIFFUSE], ClosestHitShaderNames[HIT_GROUP_DIFFUSE]);
		state->SetShaderConfig(sizeof(XMFLOAT4), sizeof(XMFLOAT2));
		state->SetLocalPipelineLayout(0, m_pipelineLayouts[RAY_GEN_LAYOUT],
			1, reinterpret_cast<const void**>(&RaygenShaderName));
		state->SetGlobalPipelineLayout(m_pipelineLayouts[RT_GLOBAL_LAYOUT]);
		state->SetMaxRecursionDepth(1);
		X_RETURN(m_pipelines[RAY_TRACING], state->GetPipeline(m_rayTracingPipelineCache.get(), L"Raytracing"), false);
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
		const auto asTable = descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get());
		N_RETURN(asTable, false);
	}

	// Output UAVs
	{
		const Descriptor descriptors[] =
		{
			m_outputViews[HIT_GROUP_REFLECTION]->GetUAV(),
			m_outputViews[HIT_GROUP_DIFFUSE]->GetUAV(),
			m_gbuffers[NORMAL]->GetUAV(),
			m_gbuffers[ROUGH_METAL]->GetUAV(),
			m_gbuffers[VELOCITY]->GetUAV()
		};
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		X_RETURN(m_uavTable, descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	// Index buffer SRVs
	{
		Descriptor descriptors[NUM_MESH];
		for (auto i = 0u; i < NUM_MESH; ++i) descriptors[i] = m_indexBuffers[i]->GetSRV();
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		X_RETURN(m_srvTables[SRV_TABLE_IB], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	// Vertex buffer SRVs
	{
		Descriptor descriptors[NUM_MESH];
		for (auto i = 0u; i < NUM_MESH; ++i) descriptors[i] = m_vertexBuffers[i]->GetSRV();
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		X_RETURN(m_srvTables[SRV_TABLE_VB], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	// Ray-tracing SRVs
	{
		const Descriptor descriptors[] =
		{
			m_visBuffer->GetSRV(),
			m_lightProbe->GetSRV()
		};
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		X_RETURN(m_srvTables[SRV_TABLE_RO], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	// RTV table and framebuffer
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_visBuffer->GetRTV());
		m_framebuffer = descriptorTable->GetFramebuffer(m_descriptorTableCache.get(), & m_depth->GetDSV());
	}

	// Create the sampler
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		const auto samplerAnisoWrap = SamplerPreset::ANISOTROPIC_WRAP;
		descriptorTable->SetSamplers(0, 1, &samplerAnisoWrap, m_descriptorTableCache.get());
		X_RETURN(m_samplerTable, descriptorTable->GetSamplerTable(m_descriptorTableCache.get()), false);
	}

	return true;
}

bool RayTracer::buildAccelerationStructures(RayTracing::CommandList* pCommandList, GeometryBuffer* pGeometries)
{
	const auto pDevice = pCommandList->GetRTDevice();

	// Set geometries
	VertexBufferView vertexBufferViews[NUM_MESH];
	IndexBufferView indexBufferViews[NUM_MESH];
	for (auto i = 0u; i < NUM_MESH; ++i)
	{
		vertexBufferViews[i] = m_vertexBuffers[i]->GetVBV();
		indexBufferViews[i] = m_indexBuffers[i]->GetIBV();
		BottomLevelAS::SetTriangleGeometries(pGeometries[i], 1, Format::R32G32B32_FLOAT,
			&vertexBufferViews[i], &indexBufferViews[i]);
	}

	// Descriptor index in descriptor pool
	const auto bottomLevelASIndex = 0u;
	const auto topLevelASIndex = bottomLevelASIndex + NUM_MESH;

	// Prebuild
	for (auto i = 0u; i < NUM_MESH; ++i)
	{
		m_bottomLevelASs[i] = BottomLevelAS::MakeUnique();
		N_RETURN(m_bottomLevelASs[i]->PreBuild(pDevice, 1, pGeometries[i], bottomLevelASIndex + i), false);
	}
	m_topLevelAS = TopLevelAS::MakeUnique();
	N_RETURN(m_topLevelAS->PreBuild(pDevice, NUM_MESH, topLevelASIndex,
		BuildFlag::ALLOW_UPDATE | BuildFlag::PREFER_FAST_TRACE), false);

	// Create scratch buffer
	auto scratchSize = m_topLevelAS->GetScratchDataMaxSize();
	for (const auto& bottomLevelAS : m_bottomLevelASs)
		scratchSize = (max)(bottomLevelAS->GetScratchDataMaxSize(), scratchSize);
	m_scratch = Resource::MakeUnique();
	N_RETURN(AccelerationStructure::AllocateUAVBuffer(pDevice, m_scratch.get(), scratchSize), false);

	// Get descriptor pool and create descriptor tables
	N_RETURN(createDescriptorTables(), false);
	const auto& descriptorPool = m_descriptorTableCache->GetDescriptorPool(CBV_SRV_UAV_POOL);

	// Set instance
	XMFLOAT3X4 matrices[NUM_MESH];
	XMStoreFloat3x4(&matrices[GROUND], (XMMatrixScaling(8.0f, 0.5f, 8.0f) * XMMatrixTranslation(0.0f, -0.5f, 0.0f)));
	XMStoreFloat3x4(&matrices[MODEL_OBJ], (XMMatrixScaling(m_posScale.w, m_posScale.w, m_posScale.w) *
		XMMatrixTranslation(m_posScale.x, m_posScale.y, m_posScale.z)));
	float* const transforms[] =
	{
		reinterpret_cast<float*>(&matrices[GROUND]),
		reinterpret_cast<float*>(&matrices[MODEL_OBJ])
	};
	for (auto& instances : m_instances) instances = Resource::MakeUnique();
	auto& instances = m_instances[FrameCount - 1];
	const BottomLevelAS* ppBottomLevelASs[NUM_MESH];
	for (auto i = 0u; i < NUM_MESH; ++i) ppBottomLevelASs[i] = m_bottomLevelASs[i].get();
	TopLevelAS::SetInstances(pDevice, instances.get(), NUM_MESH, ppBottomLevelASs, transforms);

	// Build bottom level ASs
	for (auto& bottomLevelAS : m_bottomLevelASs)
		bottomLevelAS->Build(pCommandList, m_scratch.get(), descriptorPool);

	// Build top level AS
	m_topLevelAS->Build(pCommandList, m_scratch.get(), instances.get(), descriptorPool);

	return true;
}

bool RayTracer::buildShaderTables(const RayTracing::Device* pDevice)
{
	// Get shader identifiers.
	const auto shaderIDSize = ShaderRecord::GetShaderIDSize(pDevice);
	const auto cbRayGen = RayGenConstants();

	for (uint8_t i = 0; i < FrameCount; ++i)
	{
		// Ray gen shader table
		m_rayGenShaderTables[i] = ShaderTable::MakeUnique();
		N_RETURN(m_rayGenShaderTables[i]->Create(pDevice, 1, shaderIDSize + sizeof(cbRayGen),
			(L"RayGenShaderTable" + to_wstring(i)).c_str()), false);
		N_RETURN(m_rayGenShaderTables[i]->AddShaderRecord(ShaderRecord::MakeUnique(pDevice,
			m_pipelines[RAY_TRACING], RaygenShaderName, &cbRayGen, sizeof(cbRayGen)).get()), false);
	}

	// Hit group shader table
	m_hitGroupShaderTable = ShaderTable::MakeUnique();
	N_RETURN(m_hitGroupShaderTable->Create(pDevice, 2, shaderIDSize, L"HitGroupShaderTable"), false);
	N_RETURN(m_hitGroupShaderTable->AddShaderRecord(ShaderRecord::MakeUnique(pDevice,
		m_pipelines[RAY_TRACING], HitGroupNames[HIT_GROUP_REFLECTION]).get()), false);
	N_RETURN(m_hitGroupShaderTable->AddShaderRecord(ShaderRecord::MakeUnique(pDevice,
		m_pipelines[RAY_TRACING], HitGroupNames[HIT_GROUP_DIFFUSE]).get()), false);

	// Miss shader table
	m_missShaderTable = ShaderTable::MakeUnique();
	N_RETURN(m_missShaderTable->Create(pDevice, 1, shaderIDSize, L"MissShaderTable"), false);
	N_RETURN(m_missShaderTable->AddShaderRecord(ShaderRecord::MakeUnique(pDevice,
		m_pipelines[RAY_TRACING], MissShaderName).get()), false);

	return true;
}

void RayTracer::visibility(XUSG::CommandList* pCommandList, uint8_t frameIndex)
{
	// Set barriers
	ResourceBarrier barriers[2];
	auto numBarriers = m_depth->SetBarrier(barriers, ResourceState::DEPTH_WRITE);
	numBarriers = m_visBuffer->SetBarrier(barriers, ResourceState::RENDER_TARGET, numBarriers, 0);
	pCommandList->Barrier(numBarriers, barriers);

	// Set framebuffer
	pCommandList->OMSetFramebuffer(m_framebuffer);

	// Clear render target
	const float clearColor[4] = {};
	pCommandList->ClearRenderTargetView(m_visBuffer->GetRTV(), clearColor);
	pCommandList->ClearDepthStencilView(m_depth->GetDSV(), ClearFlag::DEPTH, 1.0f);

	// Set pipeline state
	pCommandList->SetGraphicsPipelineLayout(m_pipelineLayouts[VISIBILITY_LAYOUT]);
	pCommandList->SetPipelineState(m_pipelines[VISIBILITY]);
	pCommandList->SetGraphicsRootConstantBufferView(0, m_cbMaterials.get());

	// Set viewport
	Viewport viewport(0.0f, 0.0f, static_cast<float>(m_viewport.x), static_cast<float>(m_viewport.y));
	RectRange scissorRect(0, 0, m_viewport.x, m_viewport.y);
	pCommandList->RSSetViewports(1, &viewport);
	pCommandList->RSSetScissorRects(1, &scissorRect);

	pCommandList->IASetPrimitiveTopology(PrimitiveTopology::TRIANGLELIST);

	for (auto i = 0u; i < NUM_MESH; ++i)
	{
		// Set descriptor tables
		pCommandList->SetGraphicsRootConstantBufferView(0, m_cbPerOjects[i].get(), m_cbPerOjects[i]->GetCBVOffset(frameIndex));
		pCommandList->SetGraphics32BitConstant(1, i);

		pCommandList->IASetVertexBuffers(0, 1, &m_vertexBuffers[i]->GetVBV());
		pCommandList->IASetIndexBuffer(m_indexBuffers[i]->GetIBV());

		pCommandList->DrawIndexed(m_numIndices[i], 1, 0, 0, 0);
	}
}

void RayTracer::rayTrace(const RayTracing::CommandList* pCommandList, uint8_t frameIndex)
{
	// Bind the acceleration structure and dispatch rays.
	pCommandList->SetComputePipelineLayout(m_pipelineLayouts[RT_GLOBAL_LAYOUT]);
	pCommandList->SetComputeDescriptorTable(OUTPUT_VIEW, m_uavTable);
	pCommandList->SetTopLevelAccelerationStructure(ACCELERATION_STRUCTURE, m_topLevelAS.get());
	pCommandList->SetComputeDescriptorTable(SAMPLER, m_samplerTable);
	pCommandList->SetComputeDescriptorTable(INDEX_BUFFERS, m_srvTables[SRV_TABLE_IB]);
	pCommandList->SetComputeDescriptorTable(VERTEX_BUFFERS, m_srvTables[SRV_TABLE_VB]);
	pCommandList->SetComputeRootConstantBufferView(MATERIALS, m_cbMaterials.get());
	pCommandList->SetComputeRootConstantBufferView(CONSTANTS, m_cbRaytracing.get(), m_cbRaytracing->GetCBVOffset(frameIndex));
	pCommandList->SetComputeDescriptorTable(SHADER_RESOURCES, m_srvTables[SRV_TABLE_RO]);

	// Fallback layer has no depth
	pCommandList->DispatchRays(m_pipelines[RAY_TRACING], m_viewport.x, m_viewport.y, 1,
		m_hitGroupShaderTable.get(), m_missShaderTable.get(), m_rayGenShaderTables[frameIndex].get());
}
