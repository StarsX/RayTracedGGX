//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

#include "ObjLoader.h"
#include "RayTracer.h"

#define SizeOfInUint32(obj) ((sizeof(obj) - 1) / sizeof(uint32_t) + 1)

using namespace std;
using namespace DirectX;
using namespace XUSG;
using namespace XUSG::RayTracing;

const wchar_t *RayTracer::HitGroupName = L"hitGroup";
const wchar_t *RayTracer::RaygenShaderName = L"raygenMain";
const wchar_t *RayTracer::ClosestHitShaderName = L"closestHitMain";
const wchar_t *RayTracer::MissShaderName = L"missMain";

RayTracer::RayTracer(const RayTracing::Device &device, const RayTracing::CommandList &commandList) :
	m_device(device),
	m_commandList(commandList)
{
	m_pipelineCache.SetDevice(device);
	m_descriptorTableCache.SetDevice(device.Common);
	m_pipelineLayoutCache.SetDevice(device.Common);

	m_descriptorTableCache.SetName(L"RayTracerDescriptorTableCache");
}

RayTracer::~RayTracer()
{
}

bool RayTracer::Init(uint32_t width, uint32_t height, Resource *vbUploads, Resource *ibUploads,
	Resource &scratch, Resource &instances, const char *fileName, const XMFLOAT4 &posScale)
{
	m_viewport = XMUINT2(width, height);
	m_posScale = posScale;

	// Load inputs
	ObjLoader objLoader;
	if (!objLoader.Import(fileName, true, true)) return false;
	N_RETURN(createVB(objLoader.GetNumVertices(), objLoader.GetVertexStride(), objLoader.GetVertices(), vbUploads[MODEL_OBJ]), false);
	N_RETURN(createIB(objLoader.GetNumIndices(), objLoader.GetIndices(), ibUploads[MODEL_OBJ]), false);

	N_RETURN(createGroundMesh(vbUploads[GROUND], ibUploads[GROUND]), false);

	// Create raytracing pipeline
	createPipelineLayouts();
	createPipeline();

	// Create output view and build acceleration structures
	for (auto &outputView : m_outputViews)
		outputView.Create(m_device.Common, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	N_RETURN(buildAccelerationStructures(scratch, instances), false);
	buildShaderTables();

	// Create the sampler
	{
		Util::DescriptorTable samplerTable;
		const auto samplerAnisoWrap = SamplerPreset::ANISOTROPIC_WRAP;
		samplerTable.SetSamplers(0, 1, &samplerAnisoWrap, m_descriptorTableCache);
		m_samplerTable = samplerTable.GetSamplerTable(m_descriptorTableCache);
	}

	return true;
}

void RayTracer::UpdateFrame(uint32_t frameIndex, CXMVECTOR eyePt, CXMMATRIX viewProj)
{
	const auto projToWorld = XMMatrixInverse(nullptr, viewProj);
	XMStoreFloat4x4(&m_cbRayGens[frameIndex].ProjToWorld, XMMatrixTranspose(projToWorld));
	XMStoreFloat3(&m_cbRayGens[frameIndex].EyePt, eyePt);

	m_rayGenShaderTables[frameIndex].Reset();
	m_rayGenShaderTables[frameIndex].AddShaderRecord(ShaderRecord(m_device, m_pipelines[TEST],
		RaygenShaderName, &m_cbRayGens[frameIndex], sizeof(RayGenConstants)));
}

void RayTracer::Render(uint32_t frameIndex, const Descriptor &dsv)
{
	m_outputViews[frameIndex].Barrier(m_commandList.Common, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	rayTrace(frameIndex);
}

const Texture2D &RayTracer::GetOutputView(uint32_t frameIndex, ResourceState dstState)
{
	if (dstState) m_outputViews[frameIndex].Barrier(m_commandList.Common, dstState);

	return m_outputViews[frameIndex];
}

bool RayTracer::createVB(uint32_t numVert, uint32_t stride, const uint8_t *pData, Resource &vbUpload)
{
	auto &vertexBuffer = m_vertexBuffers[MODEL_OBJ];
	N_RETURN(vertexBuffer.Create(m_device.Common, numVert, stride, D3D12_RESOURCE_FLAG_NONE,
		D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST), false);

	return vertexBuffer.Upload(m_commandList.Common, vbUpload, pData,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

bool RayTracer::createIB(uint32_t numIndices, const uint32_t *pData, Resource &ibUpload)
{
	auto &indexBuffers = m_indexBuffers[MODEL_OBJ];
	N_RETURN(indexBuffers.Create(m_device.Common, sizeof(uint32_t) * numIndices, DXGI_FORMAT_R32_UINT,
		D3D12_RESOURCE_FLAG_NONE, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST), false);

	return indexBuffers.Upload(m_commandList.Common, ibUpload, pData,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

bool RayTracer::createGroundMesh(Resource &vbUpload,Resource &ibUpload)
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

		auto &vertexBuffer = m_vertexBuffers[GROUND];
		N_RETURN(vertexBuffer.Create(m_device.Common, ARRAYSIZE(vertices), sizeof(XMFLOAT3[2]),
			D3D12_RESOURCE_FLAG_NONE, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST), false);

		N_RETURN(vertexBuffer.Upload(m_commandList.Common, vbUpload, vertices,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE), false);
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

		auto &indexBuffers = m_indexBuffers[GROUND];
		N_RETURN(indexBuffers.Create(m_device.Common, sizeof(uint32_t) * ARRAYSIZE(indices), DXGI_FORMAT_R32_UINT,
			D3D12_RESOURCE_FLAG_NONE, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST), false);

		N_RETURN(indexBuffers.Upload(m_commandList.Common, ibUpload, indices,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE), false);
	}

	return true;
}

void RayTracer::createPipelineLayouts()
{
	// Global Root Signature
	// This is a root signature that is shared across all raytracing shaders invoked during a DispatchRays() call.
	{
		RayTracing::PipelineLayout pipelineLayout;
		pipelineLayout.SetRange(OUTPUT_VIEW, DescriptorType::UAV, 1, 0);
		pipelineLayout.SetRange(ACCELERATION_STRUCTURE, DescriptorType::SRV, 1, 0);
		pipelineLayout.SetRange(SAMPLER, DescriptorType::SAMPLER, 1, 0);
		pipelineLayout.SetRange(INDEX_BUFFERS, DescriptorType::SRV, NUM_MESH, 0, 1);
		pipelineLayout.SetRange(VERTEX_BUFFERS, DescriptorType::SRV, NUM_MESH, 0, 2);
		m_pipelineLayouts[GLOBAL_LAYOUT] = pipelineLayout.GetPipelineLayout(m_device, m_pipelineLayoutCache,
			D3D12_ROOT_SIGNATURE_FLAG_NONE, NumUAVs, L"RayTracerGlobalPipelineLayout");
	}

	// Local Root Signature for RayGen shader
	// This is a root signature that enables a shader to have unique arguments that come from shader tables.
	{
		RayTracing::PipelineLayout pipelineLayout;
		pipelineLayout.SetConstants(0, SizeOfInUint32(RayGenConstants), 0);
		m_pipelineLayouts[RAY_GEN_LAYOUT] = pipelineLayout.GetPipelineLayout(m_device, m_pipelineLayoutCache,
			D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE, NumUAVs, L"RayTracerRayGenPipelineLayout");
	}
}

void RayTracer::createPipeline()
{
	{
		ThrowIfFailed(D3DReadFileToBlob(L"RayTracedTest.cso", &m_shaderLib));

		RayTracing::State state;
		state.SetShaderLibrary(m_shaderLib);
		state.SetHitGroup(0, HitGroupName, ClosestHitShaderName);
		state.SetShaderConfig(sizeof(XMFLOAT4), sizeof(XMFLOAT2));
		state.SetLocalPipelineLayout(0, m_pipelineLayouts[RAY_GEN_LAYOUT],
			1, reinterpret_cast<const void**>(&RaygenShaderName));
		state.SetGlobalPipelineLayout(m_pipelineLayouts[GLOBAL_LAYOUT]);
		state.SetMaxRecursionDepth(3);
		m_pipelines[TEST] = state.GetPipeline(m_pipelineCache);
	}
}

void RayTracer::createDescriptorTables()
{
	//m_descriptorTableCache.AllocateDescriptorPool(CBV_SRV_UAV_POOL, NumUAVs + NUM_MESH * 2 + 1);

	for (auto i = 0u; i < FrameCount; ++i)
	{
		Util::DescriptorTable descriptorTable;
		descriptorTable.SetDescriptors(0, 1, &m_outputViews[i].GetUAV());
		m_uavTables[i][UAV_TABLE_OUTPUT] = descriptorTable.GetCbvSrvUavTable(m_descriptorTableCache);
	}

	{
		Descriptor descriptors[NUM_MESH + 1];
		for (auto i = 0u; i < NUM_MESH; ++i) descriptors[i] = m_bottomLevelASs[i].GetResult().GetUAV();
		descriptors[NUM_MESH] = m_topLevelAS.GetResult().GetUAV();
		Util::DescriptorTable descriptorTable;
		descriptorTable.SetDescriptors(0, ARRAYSIZE(descriptors), descriptors);
		descriptorTable.GetCbvSrvUavTable(m_descriptorTableCache);
	}

	if (m_device.RaytracingAPI == RayTracing::API::DirectXRaytracing)
	{
		Util::DescriptorTable descriptorTable;
		descriptorTable.SetDescriptors(0, 1, &m_topLevelAS.GetResult().GetSRV());
		m_srvTables[SRV_TABLE_AS] = descriptorTable.GetCbvSrvUavTable(m_descriptorTableCache);
	}

	{
		Descriptor descriptors[NUM_MESH];
		for (auto i = 0u; i < NUM_MESH; ++i) descriptors[i] = m_indexBuffers[i].GetSRV();
		Util::DescriptorTable descriptorTable;
		descriptorTable.SetDescriptors(0, ARRAYSIZE(descriptors), descriptors);
		m_srvTables[SRV_TABLE_IB] = descriptorTable.GetCbvSrvUavTable(m_descriptorTableCache);
	}

	{
		Descriptor descriptors[NUM_MESH];
		for (auto i = 0u; i < NUM_MESH; ++i) descriptors[i] = m_vertexBuffers[i].GetSRV();
		Util::DescriptorTable descriptorTable;
		descriptorTable.SetDescriptors(0, ARRAYSIZE(descriptors), descriptors);
		m_srvTables[SRV_TABLE_VB] = descriptorTable.GetCbvSrvUavTable(m_descriptorTableCache);
	}
}

bool RayTracer::buildAccelerationStructures(XUSG::Resource &scratch, XUSG::Resource &instances)
{
	// Set geometries
	VertexBufferView vertexBufferViews[NUM_MESH];
	IndexBufferView indexBufferViews[NUM_MESH];
	for (auto i = 0; i < NUM_MESH; ++i)
	{
		vertexBufferViews[i] = m_vertexBuffers[i].GetVBV();
		indexBufferViews[i] = m_indexBuffers[i].GetIBV();
	}
	vector<Geometry> geometries(NUM_MESH);
	BottomLevelAS::SetGeometries(geometries.data(), NUM_MESH, DXGI_FORMAT_R32G32B32_FLOAT,
		vertexBufferViews, indexBufferViews);

	// Descriptor index in descriptor pool
	const uint32_t bottomLevelASIndex = FrameCount;
	const uint32_t topLevelASIndex = bottomLevelASIndex + NUM_MESH;

	// Prebuild
	for (auto i = 0; i < NUM_MESH; ++i)
		N_RETURN(m_bottomLevelASs[i].PreBuild(m_device, 1, &geometries[i],
			bottomLevelASIndex + i, NumUAVs), false);
	N_RETURN(m_topLevelAS.PreBuild(m_device, NUM_MESH, topLevelASIndex, NumUAVs), false);

	// Create scratch buffer
	auto scratchSize = m_topLevelAS.GetScratchDataMaxSize();
	for (const auto &bottomLevelAS : m_bottomLevelASs)
		scratchSize = (max)(bottomLevelAS.GetScratchDataMaxSize(), scratchSize);
	N_RETURN(AccelerationStructure::AllocateUAVBuffer(m_device.Common, scratch, scratchSize), false);

	// Get descriptor pool and create descriptor tables
	createDescriptorTables();
	const auto &descriptorPool = m_descriptorTableCache.GetDescriptorPool(CBV_SRV_UAV_POOL);

	// Set instance
	const auto numInstances = NUM_MESH;
	float *transforms[] =
	{
		XMMatrixTranspose((XMMatrixScaling(m_posScale.w, m_posScale.w, m_posScale.w) * XMMatrixTranslation(m_posScale.x, m_posScale.y, m_posScale.z))).r[0].m128_f32,
		XMMatrixTranspose((XMMatrixScaling(8.0f, 0.5f, 8.0f) * XMMatrixTranslation(0.0f, -0.5f, 0.0f))).r[0].m128_f32
	};
	TopLevelAS::SetInstances(m_device, instances, NUM_MESH, m_bottomLevelASs, transforms);

	// Build bottom level ASs
	for (auto &bottomLevelAS : m_bottomLevelASs)
		N_RETURN(bottomLevelAS.Build(m_device, m_commandList, scratch, descriptorPool, NumUAVs), false);

	// Barrier
	AccelerationStructure::Barrier(m_commandList, numInstances, m_bottomLevelASs);

	// Build top level AS
	return m_topLevelAS.Build(m_device, m_commandList, scratch, instances, descriptorPool, NumUAVs);
}

void RayTracer::buildShaderTables()
{
	// Get shader identifiers.
	const auto shaderIDSize = ShaderRecord::GetShaderIDSize(m_device);

	// Ray gen shader table
	for (auto i = 0ui8; i < FrameCount; ++i)
	{
		m_rayGenShaderTables[i].Create(m_device, 1, shaderIDSize + sizeof(RayGenConstants),
			(L"RayGenShaderTable" + to_wstring(i)).c_str());
		m_rayGenShaderTables[i].AddShaderRecord(ShaderRecord(m_device, m_pipelines[TEST],
			RaygenShaderName, &m_cbRayGens[i], sizeof(RayGenConstants)));
	}

	// Miss shader table
	m_missShaderTable.Create(m_device, 1, shaderIDSize, L"MissShaderTable");
	m_missShaderTable.AddShaderRecord(ShaderRecord(m_device, m_pipelines[TEST], MissShaderName));

	// Hit group shader table
	m_hitGroupShaderTable.Create(m_device, 1, shaderIDSize, L"HitGroupShaderTable");
	m_hitGroupShaderTable.AddShaderRecord(ShaderRecord(m_device, m_pipelines[TEST], HitGroupName));
}

void RayTracer::rayTrace(uint32_t frameIndex)
{
	const auto &commandList = m_commandList.Common;
	commandList->SetComputeRootSignature(m_pipelineLayouts[GLOBAL_LAYOUT].Get());

	// Bind the heaps, acceleration structure and dispatch rays.
	const DescriptorPool descriptorPools[] =
	{
		m_descriptorTableCache.GetDescriptorPool(CBV_SRV_UAV_POOL),
		m_descriptorTableCache.GetDescriptorPool(SAMPLER_POOL)
	};
	SetDescriptorPool(m_device, m_commandList, ARRAYSIZE(descriptorPools), descriptorPools);

	commandList->SetComputeRootDescriptorTable(OUTPUT_VIEW, *m_uavTables[frameIndex][UAV_TABLE_OUTPUT]);
	SetTopLevelAccelerationStructure(m_device, m_commandList, ACCELERATION_STRUCTURE, m_topLevelAS, m_srvTables[SRV_TABLE_AS]);
	commandList->SetComputeRootDescriptorTable(SAMPLER, *m_samplerTable);
	commandList->SetComputeRootDescriptorTable(INDEX_BUFFERS, *m_srvTables[SRV_TABLE_IB]);
	commandList->SetComputeRootDescriptorTable(VERTEX_BUFFERS, *m_srvTables[SRV_TABLE_VB]);

	DispatchRays(m_device, m_commandList, m_pipelines[TEST], m_viewport.x, m_viewport.y, 1,
		m_hitGroupShaderTable, m_missShaderTable, m_rayGenShaderTables[frameIndex]);
}
