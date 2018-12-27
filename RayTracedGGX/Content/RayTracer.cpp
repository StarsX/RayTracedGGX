//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

#include "DXFrameworkHelper.h"
#include "ObjLoader.h"
#include "RayTracer.h"

#define GGX_GI	0

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
	m_commandList(commandList),
	m_instances(nullptr)
{
	m_rayTracingPipelineCache.SetDevice(device);
	m_computePipelineCache.SetDevice(device.Common);
	m_descriptorTableCache.SetDevice(device.Common);
	m_pipelineLayoutCache.SetDevice(device.Common);

	m_descriptorTableCache.SetName(L"RayTracerDescriptorTableCache");
}

RayTracer::~RayTracer()
{
}

bool RayTracer::Init(uint32_t width, uint32_t height, Resource *vbUploads, Resource *ibUploads,
	Geometry *geometries, const char *fileName, const XMFLOAT4 &posScale)
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
	N_RETURN(createPipelines(), false);

	// Create output view and build acceleration structures
	for (auto &outputViews : m_outputViews)
		for (auto &outputView : outputViews)
			outputView.Create(m_device.Common, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	N_RETURN(buildAccelerationStructures(geometries), false);
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

static const XMFLOAT2 &IncrementalHalton()
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

// Quasirandom low-discrepancy sequences
XMFLOAT2 Hammersley(uint32_t i, uint32_t num)
{
	auto bits = i;
	bits = (bits << 16) | (bits >> 16);
	bits = ((bits & 0x55555555) << 1) | ((bits & 0xAAAAAAAA) >> 1);
	bits = ((bits & 0x33333333) << 2) | ((bits & 0xCCCCCCCC) >> 2);
	bits = ((bits & 0x0F0F0F0F) << 4) | ((bits & 0xF0F0F0F0) >> 4);
	bits = ((bits & 0x00FF00FF) << 8) | ((bits & 0xFF00FF00) >> 8);

	return XMFLOAT2(i / static_cast<float>(num), static_cast<float>(bits) * 2.3283064365386963e-10f); // / 0x100000000
}

void RayTracer::UpdateFrame(uint32_t frameIndex, CXMVECTOR eyePt, CXMMATRIX viewProj)
{
	{
		const auto projToWorld = XMMatrixInverse(nullptr, viewProj);
		XMStoreFloat4x4(&m_cbRayGens[frameIndex].ProjToWorld, XMMatrixTranspose(projToWorld));
		XMStoreFloat4(&m_cbRayGens[frameIndex].EyePt, eyePt);
#if GGX_GI
		m_cbRayGens[frameIndex].Jitter = IncrementalHalton();
#else
		m_cbRayGens[frameIndex].Jitter = XMFLOAT2(0.5f, 0.5f);
#endif

		m_rayGenShaderTables[frameIndex].Reset();
		m_rayGenShaderTables[frameIndex].AddShaderRecord(ShaderRecord(m_device, m_pipelines[TEST],
			RaygenShaderName, &m_cbRayGens[frameIndex], sizeof(RayGenConstants)));
	}

	{
		static auto angle = 0.0f;
#if !GGX_GI
		angle += 0.1f * XM_PI / 180.0f;
#endif
		const auto rot = XMMatrixRotationY(angle);
		XMStoreFloat3x3(&m_rot, rot);

		HitGroupConstants cbHitGroup = { XMMatrixTranspose(rot) };
#if GGX_GI
		const auto n = 255u;
		static auto i = 0u;
		cbHitGroup.Hammersley = Hammersley(i, n);
		i = (i + 1) % n;
#endif

		m_hitGroupShaderTables[frameIndex].Reset();
		m_hitGroupShaderTables[frameIndex].AddShaderRecord(ShaderRecord(m_device, m_pipelines[TEST],
			HitGroupName, &cbHitGroup, sizeof(HitGroupConstants)));
	}
}

void RayTracer::Render(uint32_t frameIndex, const Descriptor &dsv)
{
	m_outputViews[frameIndex][UAV_TABLE_OUTPUT].Barrier(m_commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	updateAccelerationStructures();
	rayTrace(frameIndex);
#if GGX_GI
	temporalSS(frameIndex);
#endif
}

const Texture2D &RayTracer::GetOutputView(uint32_t frameIndex, ResourceState dstState)
{
#if GGX_GI
#define UAV_OUTPUT	UAV_TABLE_TSAMP
#else
#define UAV_OUTPUT	UAV_TABLE_OUTPUT
#endif

	if (dstState) m_outputViews[frameIndex][UAV_OUTPUT].Barrier(m_commandList, dstState);

	return m_outputViews[frameIndex][UAV_OUTPUT];
}

bool RayTracer::createVB(uint32_t numVert, uint32_t stride, const uint8_t *pData, Resource &vbUpload)
{
	auto &vertexBuffer = m_vertexBuffers[MODEL_OBJ];
	N_RETURN(vertexBuffer.Create(m_device.Common, numVert, stride, D3D12_RESOURCE_FLAG_NONE,
		D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST), false);

	return vertexBuffer.Upload(m_commandList, vbUpload, pData,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

bool RayTracer::createIB(uint32_t numIndices, const uint32_t *pData, Resource &ibUpload)
{
	auto &indexBuffers = m_indexBuffers[MODEL_OBJ];
	N_RETURN(indexBuffers.Create(m_device.Common, sizeof(uint32_t) * numIndices, DXGI_FORMAT_R32_UINT,
		D3D12_RESOURCE_FLAG_NONE, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST), false);

	return indexBuffers.Upload(m_commandList, ibUpload, pData,
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
		N_RETURN(vertexBuffer.Create(m_device.Common, static_cast<uint32_t>(size(vertices)), sizeof(XMFLOAT3[2]),
			D3D12_RESOURCE_FLAG_NONE, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST), false);

		N_RETURN(vertexBuffer.Upload(m_commandList, vbUpload, vertices,
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
		N_RETURN(indexBuffers.Create(m_device.Common, sizeof(indices), DXGI_FORMAT_R32_UINT,
			D3D12_RESOURCE_FLAG_NONE, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST), false);

		N_RETURN(indexBuffers.Upload(m_commandList, ibUpload, indices,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE), false);
	}

	return true;
}

void RayTracer::createPipelineLayouts()
{
	// Global pipeline layout
	// This is a pipeline layout that is shared across all raytracing shaders invoked during a DispatchRays() call.
	{
		RayTracing::PipelineLayout pipelineLayout;
		pipelineLayout.SetRange(OUTPUT_VIEW, DescriptorType::UAV, 1, 0);
		pipelineLayout.SetRootSRV(ACCELERATION_STRUCTURE, 0);
		pipelineLayout.SetRange(SAMPLER, DescriptorType::SAMPLER, 1, 0);
		pipelineLayout.SetRange(INDEX_BUFFERS, DescriptorType::SRV, NUM_MESH, 0, 1);
		pipelineLayout.SetRange(VERTEX_BUFFERS, DescriptorType::SRV, NUM_MESH, 0, 2);
		m_pipelineLayouts[GLOBAL_LAYOUT] = pipelineLayout.GetPipelineLayout(m_device, m_pipelineLayoutCache,
			D3D12_ROOT_SIGNATURE_FLAG_NONE, NumUAVs, L"RayTracerGlobalPipelineLayout");
	}

	// Local pipeline layout for RayGen shader
	// This is a pipeline layout that enables a shader to have unique arguments that come from shader tables.
	{
		RayTracing::PipelineLayout pipelineLayout;
		pipelineLayout.SetConstants(0, SizeOfInUint32(RayGenConstants), 0);
		m_pipelineLayouts[RAY_GEN_LAYOUT] = pipelineLayout.GetPipelineLayout(m_device, m_pipelineLayoutCache,
			D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE, NumUAVs, L"RayTracerRayGenPipelineLayout");
	}

	// Local pipeline layout for Hit group
	// This is a pipeline layout that enables a shader to have unique arguments that come from shader tables.
	{
		RayTracing::PipelineLayout pipelineLayout;
		pipelineLayout.SetConstants(0, SizeOfInUint32(HitGroupConstants), 1);
		m_pipelineLayouts[HIT_GROUP_LAYOUT] = pipelineLayout.GetPipelineLayout(m_device, m_pipelineLayoutCache,
			D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE, NumUAVs, L"RayTracerHitGroupPipelineLayout");
	}

	// This is a pipeline layout for temporal SS
	{
		Util::PipelineLayout pipelineLayout;
		pipelineLayout.SetRange(OUTPUT_VIEW, DescriptorType::UAV, 1, 0);
		pipelineLayout.SetShaderStage(OUTPUT_VIEW, Shader::Stage::CS);
		pipelineLayout.SetRange(SHADER_RESOURCES, DescriptorType::SRV, 2, 0);
		pipelineLayout.SetShaderStage(SHADER_RESOURCES, Shader::Stage::CS);
		pipelineLayout.SetRange(SAMPLER, DescriptorType::SAMPLER, 1, 0);
		pipelineLayout.SetShaderStage(SAMPLER, Shader::Stage::CS);
		m_pipelineLayouts[TEMPORAL_SS_LAYOUT] = pipelineLayout.GetPipelineLayout(m_pipelineLayoutCache,
			D3D12_ROOT_SIGNATURE_FLAG_NONE, L"TemporalSSPipelineLayout");
	}
}

bool RayTracer::createPipelines()
{
	{
		Blob shaderLib;
		V_RETURN(D3DReadFileToBlob(L"RayTracedTest.cso", &shaderLib), cerr, false);

		RayTracing::State state;
		state.SetShaderLibrary(shaderLib);
		state.SetHitGroup(0, HitGroupName, ClosestHitShaderName);
		state.SetShaderConfig(sizeof(XMFLOAT4), sizeof(XMFLOAT2));
		state.SetLocalPipelineLayout(0, m_pipelineLayouts[RAY_GEN_LAYOUT],
			1, reinterpret_cast<const void**>(&RaygenShaderName));
		state.SetLocalPipelineLayout(1, m_pipelineLayouts[HIT_GROUP_LAYOUT],
			1, reinterpret_cast<const void**>(&HitGroupName));
		state.SetGlobalPipelineLayout(m_pipelineLayouts[GLOBAL_LAYOUT]);
		state.SetMaxRecursionDepth(3);
		m_pipelines[TEST] = state.GetPipeline(m_rayTracingPipelineCache);
	}

	{
		Blob shaderLib;
		V_RETURN(D3DReadFileToBlob(L"RayTracedGGX.cso", &shaderLib), cerr, false);

		RayTracing::State state;
		state.SetShaderLibrary(shaderLib);
		state.SetHitGroup(0, HitGroupName, ClosestHitShaderName);
		state.SetShaderConfig(sizeof(XMFLOAT4), sizeof(XMFLOAT2));
		state.SetLocalPipelineLayout(0, m_pipelineLayouts[RAY_GEN_LAYOUT],
			1, reinterpret_cast<const void**>(&RaygenShaderName));
		state.SetLocalPipelineLayout(1, m_pipelineLayouts[HIT_GROUP_LAYOUT],
			1, reinterpret_cast<const void**>(&HitGroupName));
		state.SetGlobalPipelineLayout(m_pipelineLayouts[GLOBAL_LAYOUT]);
		state.SetMaxRecursionDepth(3);
		m_pipelines[GGX] = state.GetPipeline(m_rayTracingPipelineCache);
	}

	{
		const auto shader = m_shaderPool.CreateShader(Shader::Stage::CS, 0, L"TemporalSS.cso");

		Compute::State state;
		state.SetPipelineLayout(m_pipelineLayouts[TEMPORAL_SS_LAYOUT]);
		state.SetShader(shader);
		m_pipeline = state.GetPipeline(m_computePipelineCache);
	}

	return true;
}

void RayTracer::createDescriptorTables()
{
	//m_descriptorTableCache.AllocateDescriptorPool(CBV_SRV_UAV_POOL, NumUAVs + NUM_MESH * 2);

	// Acceleration structure UAVs
	{
		Descriptor descriptors[NUM_MESH + 1];
		for (auto i = 0u; i < NUM_MESH; ++i) descriptors[i] = m_bottomLevelASs[i].GetResult().GetUAV();
		descriptors[NUM_MESH] = m_topLevelAS.GetResult().GetUAV();
		Util::DescriptorTable descriptorTable;
		descriptorTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		descriptorTable.GetCbvSrvUavTable(m_descriptorTableCache);
	}

	for (auto i = 0u; i < FrameCount; ++i)
	{
		// Output UAV
		{
			Util::DescriptorTable descriptorTable;
			descriptorTable.SetDescriptors(0, 1, &m_outputViews[i][UAV_TABLE_OUTPUT].GetUAV());
			m_uavTables[i][UAV_TABLE_OUTPUT] = descriptorTable.GetCbvSrvUavTable(m_descriptorTableCache);
		}

		// Temporal SS output UAV
		{
			Util::DescriptorTable descriptorTable;
			descriptorTable.SetDescriptors(0, 1, &m_outputViews[i][UAV_TABLE_TSAMP].GetUAV());
			m_uavTables[i][UAV_TABLE_TSAMP] = descriptorTable.GetCbvSrvUavTable(m_descriptorTableCache);
		}
	}

	// Index buffer SRVs
	{
		Descriptor descriptors[NUM_MESH];
		for (auto i = 0u; i < NUM_MESH; ++i) descriptors[i] = m_indexBuffers[i].GetSRV();
		Util::DescriptorTable descriptorTable;
		descriptorTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		m_srvTables[SRV_TABLE_IB] = descriptorTable.GetCbvSrvUavTable(m_descriptorTableCache);
	}

	// Vertex buffer SRVs
	{
		Descriptor descriptors[NUM_MESH];
		for (auto i = 0u; i < NUM_MESH; ++i) descriptors[i] = m_vertexBuffers[i].GetSRV();
		Util::DescriptorTable descriptorTable;
		descriptorTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		m_srvTables[SRV_TABLE_VB] = descriptorTable.GetCbvSrvUavTable(m_descriptorTableCache);
	}

	// Temporal SS input SRVs
	for (auto i = 0u; i < FrameCount; ++i)
	{
		Descriptor descriptors[] =
		{
			m_outputViews[i][UAV_TABLE_OUTPUT].GetSRV(),
			m_outputViews[(i + FrameCount - 1) % FrameCount][UAV_TABLE_TSAMP].GetSRV()
		};
		Util::DescriptorTable descriptorTable;
		descriptorTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		m_srvTables[SRV_TABLE_TS + i] = descriptorTable.GetCbvSrvUavTable(m_descriptorTableCache);
	}
}

bool RayTracer::buildAccelerationStructures(Geometry *geometries)
{
	AccelerationStructure::SetFrameCount(FrameCount);

	// Set geometries
	VertexBufferView vertexBufferViews[NUM_MESH];
	IndexBufferView indexBufferViews[NUM_MESH];
	for (auto i = 0; i < NUM_MESH; ++i)
	{
		vertexBufferViews[i] = m_vertexBuffers[i].GetVBV();
		indexBufferViews[i] = m_indexBuffers[i].GetIBV();
	}
	BottomLevelAS::SetGeometries(geometries, NUM_MESH, DXGI_FORMAT_R32G32B32_FLOAT,
		vertexBufferViews, indexBufferViews);

	// Descriptor index in descriptor pool
	const uint32_t bottomLevelASIndex = 0;
	const uint32_t topLevelASIndex = bottomLevelASIndex + NUM_MESH;

	// Prebuild
	for (auto i = 0; i < NUM_MESH; ++i)
		N_RETURN(m_bottomLevelASs[i].PreBuild(m_device, 1, &geometries[i],
			bottomLevelASIndex + i, NumUAVs), false);
	N_RETURN(m_topLevelAS.PreBuild(m_device, NUM_MESH, topLevelASIndex, NumUAVs,
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE |
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE), false);

	// Create scratch buffer
	auto scratchSize = m_topLevelAS.GetScratchDataMaxSize();
	for (const auto &bottomLevelAS : m_bottomLevelASs)
		scratchSize = (max)(bottomLevelAS.GetScratchDataMaxSize(), scratchSize);
	N_RETURN(AccelerationStructure::AllocateUAVBuffer(m_device, m_scratch, scratchSize), false);

	// Get descriptor pool and create descriptor tables
	createDescriptorTables();
	const auto &descriptorPool = m_descriptorTableCache.GetDescriptorPool(CBV_SRV_UAV_POOL);

	// Set instance
	XMFLOAT4X4 matrices[NUM_MESH];
	XMStoreFloat4x4(&matrices[GROUND], XMMatrixTranspose((XMMatrixScaling(8.0f, 0.5f, 8.0f) * XMMatrixTranslation(0.0f, -0.5f, 0.0f))));
	XMStoreFloat4x4(&matrices[MODEL_OBJ], XMMatrixTranspose((XMMatrixScaling(m_posScale.w, m_posScale.w, m_posScale.w) *
		XMMatrixTranslation(m_posScale.x, m_posScale.y, m_posScale.z))));
	float *const transforms[] =
	{
		reinterpret_cast<float*>(&matrices[GROUND]),
		reinterpret_cast<float*>(&matrices[MODEL_OBJ])
	};
	TopLevelAS::SetInstances(m_device, m_instances, NUM_MESH, m_bottomLevelASs, transforms);

	// Build bottom level ASs
	for (auto &bottomLevelAS : m_bottomLevelASs)
		bottomLevelAS.Build(m_commandList, m_scratch, descriptorPool, NumUAVs);

	// Build top level AS
	m_topLevelAS.Build(m_commandList, m_scratch, m_instances, descriptorPool, NumUAVs);

	return true;
}

void RayTracer::buildShaderTables()
{
	// Get shader identifiers.
	const auto shaderIDSize = ShaderRecord::GetShaderIDSize(m_device);

	for (auto i = 0ui8; i < FrameCount; ++i)
	{
		// Ray gen shader table
		m_rayGenShaderTables[i].Create(m_device, 1, shaderIDSize + sizeof(RayGenConstants),
			(L"RayGenShaderTable" + to_wstring(i)).c_str());
		m_rayGenShaderTables[i].AddShaderRecord(ShaderRecord(m_device, m_pipelines[TEST],
			RaygenShaderName, &m_cbRayGens[i], sizeof(RayGenConstants)));

		// Hit group shader table
		HitGroupConstants cbHitGroup = { XMMatrixTranspose(XMLoadFloat3x3(&m_rot)) };
		m_hitGroupShaderTables[i].Create(m_device, 1, shaderIDSize + sizeof(HitGroupConstants), L"HitGroupShaderTable");
		m_hitGroupShaderTables[i].AddShaderRecord(ShaderRecord(m_device, m_pipelines[TEST],
			HitGroupName, &cbHitGroup, sizeof(HitGroupConstants)));
	}

	// Miss shader table
	m_missShaderTable.Create(m_device, 1, shaderIDSize, L"MissShaderTable");
	m_missShaderTable.AddShaderRecord(ShaderRecord(m_device, m_pipelines[TEST], MissShaderName));
}

void RayTracer::updateAccelerationStructures()
{
	// Set instance
	XMFLOAT4X4 matrices[NUM_MESH];
	XMStoreFloat4x4(&matrices[GROUND], XMMatrixTranspose((XMMatrixScaling(8.0f, 0.5f, 8.0f) * XMMatrixTranslation(0.0f, -0.5f, 0.0f))));
	XMStoreFloat4x4(&matrices[MODEL_OBJ], XMMatrixTranspose((XMMatrixScaling(m_posScale.w, m_posScale.w, m_posScale.w) * XMLoadFloat3x3(&m_rot) *
		XMMatrixTranslation(m_posScale.x, m_posScale.y, m_posScale.z))));
	float *const transforms[] =
	{
		reinterpret_cast<float*>(&matrices[GROUND]),
		reinterpret_cast<float*>(&matrices[MODEL_OBJ])
	};
	TopLevelAS::SetInstances(m_device, m_instances, NUM_MESH, m_bottomLevelASs, transforms);

	// Update top level AS
	const auto &descriptorPool = m_descriptorTableCache.GetDescriptorPool(CBV_SRV_UAV_POOL);
	m_topLevelAS.Build(m_commandList, m_scratch, m_instances, descriptorPool, NumUAVs, true);
}

void RayTracer::rayTrace(uint32_t frameIndex)
{
	m_commandList.SetComputePipelineLayout(m_pipelineLayouts[GLOBAL_LAYOUT]);

	// Bind the heaps, acceleration structure and dispatch rays.
	const DescriptorPool descriptorPools[] =
	{
		m_descriptorTableCache.GetDescriptorPool(CBV_SRV_UAV_POOL),
		m_descriptorTableCache.GetDescriptorPool(SAMPLER_POOL)
	};
	m_commandList.SetDescriptorPools(static_cast<uint32_t>(size(descriptorPools)), descriptorPools);

	m_commandList.SetComputeDescriptorTable(OUTPUT_VIEW, m_uavTables[frameIndex][UAV_TABLE_OUTPUT]);
	m_commandList.SetTopLevelAccelerationStructure(ACCELERATION_STRUCTURE, m_topLevelAS);
	m_commandList.SetComputeDescriptorTable(SAMPLER, m_samplerTable);
	m_commandList.SetComputeDescriptorTable(INDEX_BUFFERS, m_srvTables[SRV_TABLE_IB]);
	m_commandList.SetComputeDescriptorTable(VERTEX_BUFFERS, m_srvTables[SRV_TABLE_VB]);

#if GGX_GI
#define PIPELINE	GGX
#else
#define PIPELINE	TEST
#endif
	m_commandList.DispatchRays(m_pipelines[PIPELINE], m_viewport.x, m_viewport.y, 1,
		m_hitGroupShaderTables[frameIndex], m_missShaderTable, m_rayGenShaderTables[frameIndex]);
}

void RayTracer::temporalSS(uint32_t frameIndex)
{
	m_commandList.SetComputePipelineLayout(m_pipelineLayouts[TEMPORAL_SS_LAYOUT]);

	// Bind the heaps, acceleration structure and dispatch rays.
	const DescriptorPool descriptorPools[] =
	{
		m_descriptorTableCache.GetDescriptorPool(CBV_SRV_UAV_POOL),
		m_descriptorTableCache.GetDescriptorPool(SAMPLER_POOL)
	};
	m_commandList.SetDescriptorPools(static_cast<uint32_t>(size(descriptorPools)), descriptorPools);

	const auto prevFrame = (frameIndex + FrameCount - 1) % FrameCount;
	m_outputViews[frameIndex][UAV_TABLE_TSAMP].Barrier(m_commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	m_outputViews[frameIndex][UAV_TABLE_OUTPUT].Barrier(m_commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	m_outputViews[prevFrame][UAV_TABLE_TSAMP].Barrier(m_commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

	m_commandList.SetComputeDescriptorTable(OUTPUT_VIEW, m_uavTables[frameIndex][UAV_TABLE_TSAMP]);
	m_commandList.SetComputeDescriptorTable(SHADER_RESOURCES, m_srvTables[SRV_TABLE_TS + frameIndex]);
	m_commandList.SetComputeDescriptorTable(SAMPLER, m_samplerTable);

	m_commandList.SetPipelineState(m_pipeline);
	m_commandList.Dispatch((m_viewport.x - 1) / 8 + 1, (m_viewport.y - 1) / 8 + 1, 1);
}
