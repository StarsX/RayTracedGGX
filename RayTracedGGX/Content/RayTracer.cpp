//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

#include "DXFrameworkHelper.h"
#include "ObjLoader.h"
#include "RayTracer.h"

#define SAMPLE_COUNT 2
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
	m_instances(),
	m_pipeIndex(TEST)
{
	m_rayTracingPipelineCache.SetDevice(device);
	m_graphicsPipelineCache.SetDevice(device.Common);
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

	// Create raytracing pipelines
	createInputLayout();
	createPipelineLayouts();
	N_RETURN(createPipelines(), false);

	// Create output view and build acceleration structures
	for (auto &outputViews : m_outputViews)
	{
		outputViews[UAV_TABLE_OUTPUT].Create(m_device.Common, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, SAMPLE_COUNT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		outputViews[UAV_TABLE_TSAMP].Create(m_device.Common, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	}
	for (auto &velocity : m_velocities)
		velocity.Create(m_device.Common, width, height, DXGI_FORMAT_R16G16_FLOAT);
	m_depth.Create(m_device.Common, width, height, DXGI_FORMAT_D24_UNORM_S8_UINT, D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
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

void RayTracer::SetPipeline(RayTracingPipeline pipeline)
{
	m_pipeIndex = pipeline;
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
		RayGenConstants cbRayGen = { XMMatrixTranspose(projToWorld), eyePt, IncrementalHalton() };

		m_rayGenShaderTables[frameIndex][m_pipeIndex].Reset();
		m_rayGenShaderTables[frameIndex][m_pipeIndex].AddShaderRecord(ShaderRecord(m_device, m_rayTracingPipelines[m_pipeIndex],
			RaygenShaderName, &cbRayGen, sizeof(RayGenConstants)));
	}

	{
		static auto angle = 0.0f;
		angle += m_pipeIndex == TEST ? 0.1f * XM_PI / 180.0f : 0.0f;
		const auto rot = XMMatrixRotationY(angle);
		
		const auto n = 255u;
		static auto i = 0u;
		HitGroupConstants cbHitGroup = { XMMatrixTranspose(rot), Hammersley(i, n) };
		i = (i + 1) % n;

		m_hitGroupShaderTables[frameIndex][m_pipeIndex].Reset();
		m_hitGroupShaderTables[frameIndex][m_pipeIndex].AddShaderRecord(ShaderRecord(m_device, m_rayTracingPipelines[m_pipeIndex],
			HitGroupName, &cbHitGroup, sizeof(HitGroupConstants)));

		XMMATRIX worlds[NUM_MESH] =
		{
			XMMatrixScaling(8.0f, 0.5f, 8.0f) * XMMatrixTranslation(0.0f, -0.5f, 0.0f),
			XMMatrixScaling(m_posScale.w, m_posScale.w, m_posScale.w) * rot *
			XMMatrixTranslation(m_posScale.x, m_posScale.y, m_posScale.z)
		};

		for (auto i = 0; i < NUM_MESH; ++i)
		{
			m_cbBasePass[i].WorldViewProjPrev = m_cbBasePass[i].WorldViewProj;
			XMStoreFloat4x4(&m_worlds[i], XMMatrixTranspose(worlds[i]));
			XMStoreFloat4x4(&m_cbBasePass[i].WorldViewProj, XMMatrixTranspose(worlds[i] * viewProj));
		}
	}
}

void RayTracer::Render(uint32_t frameIndex)
{
	m_outputViews[frameIndex][UAV_TABLE_OUTPUT].Barrier(m_commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	updateAccelerationStructures(frameIndex);
	rayTrace(frameIndex);

	if (m_pipeIndex == GGX)
	{
		gbufferPass(frameIndex);
		temporalSS(frameIndex);
	}
}

void RayTracer::ClearHistory()
{
	const float clearColor[4] = {};
	for (auto i = 0ui8; i < FrameCount; ++i)
	{
		m_outputViews[i][UAV_TABLE_TSAMP].Barrier(m_commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		m_commandList.ClearUnorderedAccessViewFloat(*m_uavTables[i][UAV_TABLE_TSAMP], m_outputViews[i][UAV_TABLE_TSAMP].GetUAV(),
			m_outputViews[i][UAV_TABLE_TSAMP].GetResource(), clearColor);
	}
}

const Texture2D &RayTracer::GetOutputView(uint32_t frameIndex, ResourceState dstState)
{
	const auto uavOut = m_pipeIndex == GGX ? UAV_TABLE_TSAMP : UAV_TABLE_OUTPUT;
	if (dstState) m_outputViews[frameIndex][uavOut].Barrier(m_commandList, dstState);

	return m_outputViews[frameIndex][uavOut];
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
	m_numIndices[MODEL_OBJ] = numIndices;

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

		m_numIndices[GROUND] = static_cast<uint32_t>(size(indices));

		auto &indexBuffers = m_indexBuffers[GROUND];
		N_RETURN(indexBuffers.Create(m_device.Common, sizeof(indices), DXGI_FORMAT_R32_UINT,
			D3D12_RESOURCE_FLAG_NONE, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST), false);

		N_RETURN(indexBuffers.Upload(m_commandList, ibUpload, indices,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE), false);
	}

	return true;
}

void RayTracer::createInputLayout()
{
	const auto offset = D3D12_APPEND_ALIGNED_ELEMENT;

	// Define the vertex input layout.
	InputElementTable inputElementDescs =
	{
		{ "POSITION",	0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,		D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL",		0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offset,	D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};

	m_inputLayout = m_graphicsPipelineCache.CreateInputLayout(inputElementDescs);
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

	// This is a pipeline layout for g-buffer pass
	{
		Util::PipelineLayout pipelineLayout;
		pipelineLayout.SetConstants(0, SizeOfInUint32(BasePassConstants), 0, 0, Shader::Stage::VS);
		m_pipelineLayouts[GBUFFER_PASS_LAYOUT] = pipelineLayout.GetPipelineLayout(m_pipelineLayoutCache,
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT, L"GBufferPipelineLayout");
	}

	// This is a pipeline layout for temporal SS
	{
		Util::PipelineLayout pipelineLayout;
		pipelineLayout.SetRange(OUTPUT_VIEW, DescriptorType::UAV, 1, 0);
		pipelineLayout.SetShaderStage(OUTPUT_VIEW, Shader::Stage::CS);
		pipelineLayout.SetRange(SHADER_RESOURCES, DescriptorType::SRV, 3, 0);
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
		m_rayTracingPipelines[TEST] = state.GetPipeline(m_rayTracingPipelineCache);
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
		m_rayTracingPipelines[GGX] = state.GetPipeline(m_rayTracingPipelineCache);
	}

	{
		const auto vs = m_shaderPool.CreateShader(Shader::Stage::VS, 0, L"VSBasePass.cso");
		const auto ps = m_shaderPool.CreateShader(Shader::Stage::PS, 0, L"PSGBuffer.cso");

		Graphics::State state;
		state.IASetInputLayout(m_inputLayout);
		state.SetPipelineLayout(m_pipelineLayouts[GBUFFER_PASS_LAYOUT]);
		state.SetShader(Shader::Stage::VS, m_shaderPool.GetShader(Shader::Stage::VS, 0));
		state.SetShader(Shader::Stage::PS, m_shaderPool.GetShader(Shader::Stage::PS, 0));
		state.IASetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
		state.OMSetNumRenderTargets(1);
		state.OMSetRTVFormat(0, DXGI_FORMAT_R16G16_FLOAT);
		state.OMSetDSVFormat(DXGI_FORMAT_D24_UNORM_S8_UINT);
		m_pipelines[GBUFFER_PASS] = state.GetPipeline(m_graphicsPipelineCache);
	}

	{
		const auto shader = m_shaderPool.CreateShader(Shader::Stage::CS, 0, L"TemporalSS.cso");

		Compute::State state;
		state.SetPipelineLayout(m_pipelineLayouts[TEMPORAL_SS_LAYOUT]);
		state.SetShader(shader);
		m_pipelines[TEMPORAL_SS] = state.GetPipeline(m_computePipelineCache);
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
			m_outputViews[(i + FrameCount - 1) % FrameCount][UAV_TABLE_TSAMP].GetSRV(),
			m_velocities[i].GetSRV()
		};
		Util::DescriptorTable descriptorTable;
		descriptorTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		m_srvTables[SRV_TABLE_TS + i] = descriptorTable.GetCbvSrvUavTable(m_descriptorTableCache);
	}

	// RTV tables
	for (auto i = 0u; i < FrameCount; ++i)
	{
		Util::DescriptorTable rtvTable;
		rtvTable.SetDescriptors(0, 1, &m_velocities[i].GetRTV());
		m_rtvTables[i] = rtvTable.GetRtvTable(m_descriptorTableCache);
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
	auto &instances = m_instances[FrameCount - 1];
	TopLevelAS::SetInstances(m_device, instances, NUM_MESH, m_bottomLevelASs, transforms);

	// Build bottom level ASs
	for (auto &bottomLevelAS : m_bottomLevelASs)
		bottomLevelAS.Build(m_commandList, m_scratch, descriptorPool, NumUAVs);

	// Build top level AS
	m_topLevelAS.Build(m_commandList, m_scratch, instances, descriptorPool, NumUAVs);

	return true;
}

void RayTracer::buildShaderTables()
{
	// Get shader identifiers.
	const auto shaderIDSize = ShaderRecord::GetShaderIDSize(m_device);

	for (auto k = 0ui8; k < NUM_RAYTRACE_PIPELINE; ++k)
	{
		for (auto i = 0ui8; i < FrameCount; ++i)
		{
			// Ray gen shader table
			m_rayGenShaderTables[i][k].Create(m_device, 1, shaderIDSize + sizeof(RayGenConstants),
				(L"RayGenShaderTable" + to_wstring(i)).c_str());
			m_rayGenShaderTables[i][k].AddShaderRecord(ShaderRecord(m_device, m_rayTracingPipelines[k],
				RaygenShaderName, &RayGenConstants(), sizeof(RayGenConstants)));

			// Hit group shader table
			m_hitGroupShaderTables[i][k].Create(m_device, 1, shaderIDSize + sizeof(HitGroupConstants), L"HitGroupShaderTable");
			m_hitGroupShaderTables[i][k].AddShaderRecord(ShaderRecord(m_device, m_rayTracingPipelines[k],
				HitGroupName, &HitGroupConstants(), sizeof(HitGroupConstants)));
		}

		// Miss shader table
		m_missShaderTables[k].Create(m_device, 1, shaderIDSize, L"MissShaderTable");
		m_missShaderTables[k].AddShaderRecord(ShaderRecord(m_device, m_rayTracingPipelines[k], MissShaderName));
	}
}

void RayTracer::updateAccelerationStructures(uint32_t frameIndex)
{
	// Set instance
	float *const transforms[] =
	{
		reinterpret_cast<float*>(&m_worlds[GROUND]),
		reinterpret_cast<float*>(&m_worlds[MODEL_OBJ])
	};
	TopLevelAS::SetInstances(m_device, m_instances[frameIndex], NUM_MESH, m_bottomLevelASs, transforms);

	// Update top level AS
	const auto &descriptorPool = m_descriptorTableCache.GetDescriptorPool(CBV_SRV_UAV_POOL);
	m_topLevelAS.Build(m_commandList, m_scratch, m_instances[frameIndex], descriptorPool, NumUAVs, true);
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

	// Fallback layer has no depth
	m_commandList.DispatchRays(m_rayTracingPipelines[m_pipeIndex], m_viewport.x, m_pipeIndex == GGX ? (m_viewport.y << 1) :
		m_viewport.y, 1, m_hitGroupShaderTables[frameIndex][m_pipeIndex], m_missShaderTables[m_pipeIndex],
		m_rayGenShaderTables[frameIndex][m_pipeIndex]);
}

void RayTracer::gbufferPass(uint32_t frameIndex)
{
	// Set render target
	m_velocities[frameIndex].Barrier(m_commandList, D3D12_RESOURCE_STATE_RENDER_TARGET);
	m_commandList.OMSetRenderTargets(1, m_rtvTables[frameIndex], m_depth.GetDSV());

	// Clear render target
	const float clearColor[4] = {};
	m_commandList.ClearRenderTargetView(m_velocities[frameIndex].GetRTV(), clearColor);
	m_commandList.ClearDepthStencilView(m_depth.GetDSV(), D3D12_CLEAR_FLAG_DEPTH, 1.0f);

	// Set pipeline state
	m_commandList.SetGraphicsPipelineLayout(m_pipelineLayouts[GBUFFER_PASS_LAYOUT]);
	m_commandList.SetPipelineState(m_pipelines[GBUFFER_PASS]);

	// Set viewport
	Viewport viewport(0.0f, 0.0f, static_cast<float>(m_viewport.x), static_cast<float>(m_viewport.y));
	RectRange scissorRect(0, 0, m_viewport.x, m_viewport.y);
	m_commandList.RSSetViewports(1, &viewport);
	m_commandList.RSSetScissorRects(1, &scissorRect);

	m_commandList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	for (auto i = 0ui8; i < NUM_MESH; ++i)
	{
		// Set descriptor tables
		m_commandList.SetGraphics32BitConstants(0, SizeOfInUint32(BasePassConstants), &m_cbBasePass[i]);

		m_commandList.IASetVertexBuffers(0, 1, &m_vertexBuffers[i].GetVBV());
		m_commandList.IASetIndexBuffer(m_indexBuffers[i].GetIBV());

		m_commandList.DrawIndexed(m_numIndices[i], 1, 0, 0, 0);
	}
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
	m_velocities[frameIndex].Barrier(m_commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

	m_commandList.SetComputeDescriptorTable(OUTPUT_VIEW, m_uavTables[frameIndex][UAV_TABLE_TSAMP]);
	m_commandList.SetComputeDescriptorTable(SHADER_RESOURCES, m_srvTables[SRV_TABLE_TS + frameIndex]);
	m_commandList.SetComputeDescriptorTable(SAMPLER, m_samplerTable);

	m_commandList.SetPipelineState(m_pipelines[TEMPORAL_SS]);
	m_commandList.Dispatch((m_viewport.x - 1) / 8 + 1, (m_viewport.y - 1) / 8 + 1, 1);
}
