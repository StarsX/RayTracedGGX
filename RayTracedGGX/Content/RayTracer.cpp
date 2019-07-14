//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

#include "DXFrameworkHelper.h"
#include "ObjLoader.h"
#include "RayTracer.h"

#define SAMPLE_COUNT 2
#define SizeOfInUint32(obj)	DIV_UP(sizeof(obj), sizeof(uint32_t))

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

bool RayTracer::Init(const RayTracing::CommandList& commandList, uint32_t width, uint32_t height,
	vector<Resource>& uploaders, Geometry* geometries, const char* fileName,
	Format rtFormat, const XMFLOAT4& posScale)
{
	m_viewport = XMUINT2(width, height);
	m_posScale = posScale;

	// Load inputs
	ObjLoader objLoader;
	if (!objLoader.Import(fileName, true, true)) return false;
	N_RETURN(createVB(commandList, objLoader.GetNumVertices(), objLoader.GetVertexStride(), objLoader.GetVertices(), uploaders), false);
	N_RETURN(createIB(commandList, objLoader.GetNumIndices(), objLoader.GetIndices(), uploaders), false);

	N_RETURN(createGroundMesh(commandList, uploaders), false);

	// Create raytracing pipelines
	N_RETURN(createInputLayout(), false);
	N_RETURN(createPipelineLayouts(), false);
	N_RETURN(createPipelines(rtFormat), false);

	// Create output view and build acceleration structures
	m_outputViews[UAV_TABLE_OUTPUT].Create(m_device.Common, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT,
		SAMPLE_COUNT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, 1, 1, D3D12_HEAP_TYPE_DEFAULT,
		D3D12_RESOURCE_STATE_COMMON, false, L"RayTracingOut");
	m_outputViews[UAV_TABLE_SPATIAL].Create(m_device.Common, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT,
		1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, 1, 1, D3D12_HEAP_TYPE_DEFAULT,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, false, L"SpatialOut0");
	m_outputViews[UAV_TABLE_SPATIAL1].Create(m_device.Common, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT,
		1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, 1, 1, D3D12_HEAP_TYPE_DEFAULT,
		D3D12_RESOURCE_STATE_COMMON, false, L"SpatialOut1");
	m_outputViews[UAV_TABLE_TSAMP].Create(m_device.Common, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT,
		1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, 1, 1, D3D12_HEAP_TYPE_DEFAULT,
		D3D12_RESOURCE_STATE_COMMON, false, L"TemporalSSOut0");
	m_outputViews[UAV_TABLE_TSAMP1].Create(m_device.Common, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT,
		1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, 1, 1, D3D12_HEAP_TYPE_DEFAULT,
		D3D12_RESOURCE_STATE_COMMON, false, L"TemporalSSOut1");
	m_velocity.Create(m_device.Common, width, height, DXGI_FORMAT_R16G16_FLOAT, 1, D3D12_RESOURCE_FLAG_NONE,
		1, 1, D3D12_RESOURCE_STATE_COMMON, nullptr, false, L"Velocity");
	m_depth.Create(m_device.Common, width, height, DXGI_FORMAT_D24_UNORM_S8_UINT, D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE,
		1, 1, 1, D3D12_RESOURCE_STATE_COMMON, 1.0f, 0, false, L"Depth");
	N_RETURN(buildAccelerationStructures(commandList, geometries), false);
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
		RayGenConstants cbRayGen = { XMMatrixTranspose(projToWorld), eyePt, XMFLOAT2(-halton.x, halton.y) };

		m_rayGenShaderTables[frameIndex][m_pipeIndex].Reset();
		m_rayGenShaderTables[frameIndex][m_pipeIndex].AddShaderRecord(ShaderRecord(m_device, m_rayTracingPipelines[m_pipeIndex],
			RaygenShaderName, &cbRayGen, sizeof(RayGenConstants)));
	}

	{
		static auto angle = 0.0f;
		angle += !isPaused && m_pipeIndex == TEST ? 0.1f * XM_PI / 180.0f : 0.0f;
		const auto rot = XMMatrixRotationY(angle);

		const auto n = 256u;
		static auto i = 0u;
		HitGroupConstants cbHitGroup = { XMMatrixTranspose(rot), i };
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
			m_cbBasePass[i].ProjBias = projBias;
			m_cbBasePass[i].WorldViewProjPrev = m_cbBasePass[i].WorldViewProj;
			XMStoreFloat4x4(&m_worlds[i], XMMatrixTranspose(worlds[i]));
			XMStoreFloat4x4(&m_cbBasePass[i].WorldViewProj, XMMatrixTranspose(worlds[i] * viewProj));
		}
	}
}

void RayTracer::Render(const RayTracing::CommandList& commandList, uint32_t frameIndex)
{
	updateAccelerationStructures(commandList, frameIndex);

	ResourceBarrier barriers[2];
	auto numBarriers = m_velocity.SetBarrier(barriers, D3D12_RESOURCE_STATE_RENDER_TARGET);
	commandList.Barrier(numBarriers, barriers);
	gbufferPass(commandList);

	numBarriers = m_outputViews[UAV_TABLE_OUTPUT].SetBarrier(barriers, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	numBarriers = m_velocity.SetBarrier(barriers, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		numBarriers, 0xffffffff, D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY);
	commandList.Barrier(numBarriers, barriers);
	rayTrace(commandList, frameIndex);

	if (m_pipeIndex == GGX)
	{
		spatialPass(commandList, UAV_TABLE_SPATIAL1, UAV_TABLE_OUTPUT, SRV_TABLE_TS);
		spatialPass(commandList, UAV_TABLE_SPATIAL, UAV_TABLE_SPATIAL1, SRV_TABLE_SPATIAL1);
		spatialPass(commandList, UAV_TABLE_SPATIAL1, UAV_TABLE_SPATIAL, SRV_TABLE_SPATIAL);
		spatialPass(commandList, UAV_TABLE_SPATIAL, UAV_TABLE_SPATIAL1, SRV_TABLE_SPATIAL1);
	}
	temporalSS(commandList);

	m_frameParity = !m_frameParity;
}

void RayTracer::ToneMap(const RayTracing::CommandList& commandList, const RenderTargetTable& rtvTable,
	uint32_t numBarriers, ResourceBarrier* pBarriers)
{
	numBarriers = m_outputViews[UAV_TABLE_TSAMP + m_frameParity].SetBarrier(
		pBarriers, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, numBarriers);
	commandList.Barrier(numBarriers, pBarriers);

	// Set render target
	commandList.OMSetRenderTargets(1, rtvTable, nullptr);

	// Set descriptor tables
	commandList.SetGraphicsPipelineLayout(m_pipelineLayouts[TONE_MAP_LAYOUT]);
	commandList.SetGraphicsDescriptorTable(0, m_srvTables[SRV_TABLE_TM + m_frameParity]);

	// Set pipeline state
	commandList.SetPipelineState(m_pipelines[TONE_MAP]);

	// Set viewport
	Viewport viewport(0.0f, 0.0f, static_cast<float>(m_viewport.x), static_cast<float>(m_viewport.y));
	RectRange scissorRect(0, 0, m_viewport.x, m_viewport.y);
	commandList.RSSetViewports(1, &viewport);
	commandList.RSSetScissorRects(1, &scissorRect);

	commandList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	commandList.DrawIndexed(3, 1, 0, 0, 0);
}

void RayTracer::ClearHistory(const RayTracing::CommandList& commandList)
{
	ResourceBarrier barriers[FrameCount];
	auto numBarriers = 0u;
	for (auto i = 0ui8; i < 2; ++i)
		numBarriers = m_outputViews[UAV_TABLE_TSAMP + i].SetBarrier(barriers, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, numBarriers);
	commandList.Barrier(numBarriers, barriers);

	const float clearColor[4] = {};
	for (auto i = 0ui8; i < 2; ++i)
	{
		const uint8_t j = UAV_TABLE_TSAMP + i;
		commandList.ClearUnorderedAccessViewFloat(*m_uavTables[j], m_outputViews[j].GetUAV(),
			m_outputViews[j].GetResource(), clearColor);
	}
}

bool RayTracer::createVB(const RayTracing::CommandList& commandList, uint32_t numVert,
	uint32_t stride, const uint8_t* pData, vector<Resource>& uploaders)
{
	auto& vertexBuffer = m_vertexBuffers[MODEL_OBJ];
	N_RETURN(vertexBuffer.Create(m_device.Common, numVert, stride, D3D12_RESOURCE_FLAG_NONE,
		D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, 1, nullptr, 1, nullptr,
		1, nullptr, L"MeshVB"), false);
	uploaders.push_back(nullptr);

	return vertexBuffer.Upload(commandList, uploaders.back(), pData, stride * numVert,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

bool RayTracer::createIB(const RayTracing::CommandList& commandList, uint32_t numIndices,
	const uint32_t* pData, vector<Resource>& uploaders)
{
	m_numIndices[MODEL_OBJ] = numIndices;

	auto& indexBuffers = m_indexBuffers[MODEL_OBJ];
	const uint32_t byteWidth = sizeof(uint32_t) * numIndices;
	N_RETURN(indexBuffers.Create(m_device.Common, byteWidth, DXGI_FORMAT_R32_UINT,
		D3D12_RESOURCE_FLAG_NONE, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST,
		1, nullptr, 1, nullptr, 1, nullptr, L"MeshIB"), false);
	uploaders.push_back(nullptr);

	return indexBuffers.Upload(commandList, uploaders.back(), pData,
		byteWidth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

bool RayTracer::createGroundMesh(const RayTracing::CommandList& commandList, vector<Resource>& uploaders)
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
		N_RETURN(vertexBuffer.Create(m_device.Common, static_cast<uint32_t>(size(vertices)), sizeof(XMFLOAT3[2]),
			D3D12_RESOURCE_FLAG_NONE, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, 1, nullptr,
			1, nullptr, 1, nullptr, L"GroundVB"), false);
		uploaders.push_back(nullptr);

		N_RETURN(vertexBuffer.Upload(commandList, uploaders.back(), vertices, sizeof(vertices),
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

		auto& indexBuffers = m_indexBuffers[GROUND];
		N_RETURN(indexBuffers.Create(m_device.Common, sizeof(indices), DXGI_FORMAT_R32_UINT,
			D3D12_RESOURCE_FLAG_NONE, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST,
			1, nullptr, 1, nullptr, 1, nullptr, L"GroundIB"), false);
		uploaders.push_back(nullptr);

		N_RETURN(indexBuffers.Upload(commandList, uploaders.back(), indices, sizeof(indices),
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE), false);
	}

	return true;
}

bool RayTracer::createInputLayout()
{
	const auto offset = D3D12_APPEND_ALIGNED_ELEMENT;

	// Define the vertex input layout.
	InputElementTable inputElementDescs =
	{
		{ "POSITION",	0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,		D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL",		0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offset,	D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};

	X_RETURN(m_inputLayout, m_graphicsPipelineCache.CreateInputLayout(inputElementDescs), false);

	return true;
}

bool RayTracer::createPipelineLayouts()
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
		X_RETURN(m_pipelineLayouts[GLOBAL_LAYOUT], pipelineLayout.GetPipelineLayout(m_device, m_pipelineLayoutCache,
			D3D12_ROOT_SIGNATURE_FLAG_NONE, NumUAVs, L"RayTracerGlobalPipelineLayout"), false);
	}

	// Local pipeline layout for RayGen shader
	// This is a pipeline layout that enables a shader to have unique arguments that come from shader tables.
	{
		RayTracing::PipelineLayout pipelineLayout;
		pipelineLayout.SetConstants(0, SizeOfInUint32(RayGenConstants), 0);
		X_RETURN(m_pipelineLayouts[RAY_GEN_LAYOUT], pipelineLayout.GetPipelineLayout(m_device, m_pipelineLayoutCache,
			D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE, NumUAVs, L"RayTracerRayGenPipelineLayout"), false);
	}

	// Local pipeline layout for Hit group
	// This is a pipeline layout that enables a shader to have unique arguments that come from shader tables.
	{
		RayTracing::PipelineLayout pipelineLayout;
		pipelineLayout.SetConstants(0, SizeOfInUint32(HitGroupConstants), 1);
		X_RETURN(m_pipelineLayouts[HIT_GROUP_LAYOUT], pipelineLayout.GetPipelineLayout(m_device, m_pipelineLayoutCache,
			D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE, NumUAVs, L"RayTracerHitGroupPipelineLayout"), false);
	}

	// This is a pipeline layout for g-buffer pass
	{
		Util::PipelineLayout pipelineLayout;
		pipelineLayout.SetConstants(0, SizeOfInUint32(BasePassConstants), 0, 0, Shader::Stage::VS);
		X_RETURN(m_pipelineLayouts[GBUFFER_PASS_LAYOUT], pipelineLayout.GetPipelineLayout(m_pipelineLayoutCache,
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT, L"GBufferPipelineLayout"), false);
	}

	// This is a pipeline layout for resampling
	{
		Util::PipelineLayout pipelineLayout;
		pipelineLayout.SetRange(OUTPUT_VIEW, DescriptorType::UAV, 1, 0, 0,
			D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE);
		pipelineLayout.SetShaderStage(OUTPUT_VIEW, Shader::Stage::CS);
		pipelineLayout.SetRange(SHADER_RESOURCES, DescriptorType::SRV, 1, 0);
		pipelineLayout.SetShaderStage(SHADER_RESOURCES, Shader::Stage::CS);
		pipelineLayout.SetRange(SAMPLER, DescriptorType::SAMPLER, 1, 0);
		pipelineLayout.SetShaderStage(SAMPLER, Shader::Stage::CS);
		X_RETURN(m_pipelineLayouts[RESAMPLE_LAYOUT], pipelineLayout.GetPipelineLayout(m_pipelineLayoutCache,
			D3D12_ROOT_SIGNATURE_FLAG_NONE, L"ResamplingPipelineLayout"), false);
	}

	// This is a pipeline layout for temporal SS
	{
		Util::PipelineLayout pipelineLayout;
		pipelineLayout.SetRange(OUTPUT_VIEW, DescriptorType::UAV, 1, 0, 0,
			D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE);
		pipelineLayout.SetShaderStage(OUTPUT_VIEW, Shader::Stage::CS);
		pipelineLayout.SetRange(SHADER_RESOURCES, DescriptorType::SRV, 4, 0);
		pipelineLayout.SetShaderStage(SHADER_RESOURCES, Shader::Stage::CS);
		pipelineLayout.SetRange(SAMPLER, DescriptorType::SAMPLER, 1, 0);
		pipelineLayout.SetShaderStage(SAMPLER, Shader::Stage::CS);
		X_RETURN(m_pipelineLayouts[TEMPORAL_SS_LAYOUT], pipelineLayout.GetPipelineLayout(m_pipelineLayoutCache,
			D3D12_ROOT_SIGNATURE_FLAG_NONE, L"TemporalSSPipelineLayout"), false);
	}

	// This is a pipeline layout for tone mapping
	{
		Util::PipelineLayout pipelineLayout;
		pipelineLayout.SetRange(0, DescriptorType::SRV, 1, 0);
		pipelineLayout.SetShaderStage(0, Shader::Stage::PS);
		X_RETURN(m_pipelineLayouts[TONE_MAP_LAYOUT], pipelineLayout.GetPipelineLayout(m_pipelineLayoutCache,
			D3D12_ROOT_SIGNATURE_FLAG_NONE, L"ToneMappingPipelineLayout"), false);
	}

	return true;
}

bool RayTracer::createPipelines(Format rtFormat)
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
		state.SetMaxRecursionDepth(2);
		m_rayTracingPipelines[TEST] = state.GetPipeline(m_rayTracingPipelineCache, L"RaytracingTest");

		N_RETURN(m_rayTracingPipelines[TEST].Native || m_rayTracingPipelines[TEST].Fallback, false);
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
		state.SetMaxRecursionDepth(2);
		m_rayTracingPipelines[GGX] = state.GetPipeline(m_rayTracingPipelineCache, L"RaytracingGGX");

		N_RETURN(m_rayTracingPipelines[GGX].Native || m_rayTracingPipelines[GGX].Fallback, false);
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
		X_RETURN(m_pipelines[GBUFFER_PASS], state.GetPipeline(m_graphicsPipelineCache, L"GBufferPass"), false);
	}

	{
		const auto shader = m_shaderPool.CreateShader(Shader::Stage::CS, 0, L"TemporalSS.cso");

		Compute::State state;
		state.SetPipelineLayout(m_pipelineLayouts[TEMPORAL_SS_LAYOUT]);
		state.SetShader(shader);
		X_RETURN(m_pipelines[TEMPORAL_SS], state.GetPipeline(m_computePipelineCache, L"TemporalSS"), false);
	}

	{
		const auto shader = m_shaderPool.CreateShader(Shader::Stage::CS, 1, L"SpatialPass.cso");

		Compute::State state;
		state.SetPipelineLayout(m_pipelineLayouts[RESAMPLE_LAYOUT]);
		state.SetShader(shader);
		X_RETURN(m_pipelines[SPATIAL_PASS], state.GetPipeline(m_computePipelineCache, L"SpatialPass"), false);
	}

	{
		const auto shader = m_shaderPool.CreateShader(Shader::Stage::CS, 2, L"TemporalAA.cso");

		Compute::State state;
		state.SetPipelineLayout(m_pipelineLayouts[TEMPORAL_SS_LAYOUT]);
		state.SetShader(shader);
		X_RETURN(m_pipelines[TEMPORAL_AA], state.GetPipeline(m_computePipelineCache, L"TemporalAA"), false);
	}

	{
		const auto vs = m_shaderPool.CreateShader(Shader::Stage::VS, 1, L"VSScreenQuad.cso");
		const auto ps = m_shaderPool.CreateShader(Shader::Stage::PS, 1, L"PSToneMap.cso");

		Graphics::State state;
		state.SetPipelineLayout(m_pipelineLayouts[TONE_MAP_LAYOUT]);
		state.SetShader(Shader::Stage::VS, m_shaderPool.GetShader(Shader::Stage::VS, 1));
		state.SetShader(Shader::Stage::PS, m_shaderPool.GetShader(Shader::Stage::PS, 1));
		state.DSSetState(Graphics::DEPTH_STENCIL_NONE, m_graphicsPipelineCache);
		state.IASetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
		state.OMSetNumRenderTargets(1);
		state.OMSetRTVFormat(0, rtFormat);
		X_RETURN(m_pipelines[TONE_MAP], state.GetPipeline(m_graphicsPipelineCache, L"ToneMapping"), false);
	}

	return true;
}

bool RayTracer::createDescriptorTables()
{
	//m_descriptorTableCache.AllocateDescriptorPool(CBV_SRV_UAV_POOL, NumUAVs + NUM_MESH * 2);

	// Acceleration structure UAVs
	{
		Descriptor descriptors[NUM_MESH + 1];
		for (auto i = 0u; i < NUM_MESH; ++i) descriptors[i] = m_bottomLevelASs[i].GetResult().GetUAV();
		descriptors[NUM_MESH] = m_topLevelAS.GetResult().GetUAV();
		Util::DescriptorTable descriptorTable;
		descriptorTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		const auto asTable = descriptorTable.GetCbvSrvUavTable(m_descriptorTableCache);
		N_RETURN(asTable, false);
	}

	// Output UAV
	{
		Util::DescriptorTable descriptorTable;
		descriptorTable.SetDescriptors(0, 1, &m_outputViews[UAV_TABLE_OUTPUT].GetUAV());
		X_RETURN(m_uavTables[UAV_TABLE_OUTPUT], descriptorTable.GetCbvSrvUavTable(m_descriptorTableCache), false);
	}

	// Spatially resolved UAVs
	for (auto i = 0u; i < 2; ++i)
	{
		Util::DescriptorTable descriptorTable;
		descriptorTable.SetDescriptors(0, 1, &m_outputViews[UAV_TABLE_SPATIAL + i].GetUAV());
		X_RETURN(m_uavTables[UAV_TABLE_SPATIAL + i], descriptorTable.GetCbvSrvUavTable(m_descriptorTableCache), false);
	}

	// Temporal SS output UAVs
	for (auto i = 0u; i < 2; ++i)
	{
		Util::DescriptorTable descriptorTable;
		descriptorTable.SetDescriptors(0, 1, &m_outputViews[UAV_TABLE_TSAMP + i].GetUAV());
		X_RETURN(m_uavTables[UAV_TABLE_TSAMP + i], descriptorTable.GetCbvSrvUavTable(m_descriptorTableCache), false);
	}

	// Index buffer SRVs
	{
		Descriptor descriptors[NUM_MESH];
		for (auto i = 0u; i < NUM_MESH; ++i) descriptors[i] = m_indexBuffers[i].GetSRV();
		Util::DescriptorTable descriptorTable;
		descriptorTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		X_RETURN(m_srvTables[SRV_TABLE_IB], descriptorTable.GetCbvSrvUavTable(m_descriptorTableCache), false);
	}

	// Vertex buffer SRVs
	{
		Descriptor descriptors[NUM_MESH];
		for (auto i = 0u; i < NUM_MESH; ++i) descriptors[i] = m_vertexBuffers[i].GetSRV();
		Util::DescriptorTable descriptorTable;
		descriptorTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		X_RETURN(m_srvTables[SRV_TABLE_VB], descriptorTable.GetCbvSrvUavTable(m_descriptorTableCache), false);
	}

	// Spatially resolving input SRVs
	for (auto i = 0u; i < 2; ++i)
	{
		Util::DescriptorTable descriptorTable;
		descriptorTable.SetDescriptors(0, 1, &m_outputViews[UAV_TABLE_SPATIAL + i].GetSRV());
		X_RETURN(m_srvTables[SRV_TABLE_SPATIAL + i], descriptorTable.GetCbvSrvUavTable(m_descriptorTableCache), false);
	}

	// Temporal SS input SRVs
	for (auto i = 0u; i < 2; ++i)
	{
		Descriptor descriptors[] =
		{
			m_outputViews[UAV_TABLE_OUTPUT].GetSRV(),
			m_outputViews[UAV_TABLE_TSAMP + !i].GetSRV(),
			m_velocity.GetSRV(),
			m_outputViews[UAV_TABLE_SPATIAL].GetSRV()
		};
		Util::DescriptorTable descriptorTable;
		descriptorTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		X_RETURN(m_srvTables[SRV_TABLE_TS + i], descriptorTable.GetCbvSrvUavTable(m_descriptorTableCache), false);
	}

	// Tone mapping SRVs
	for (auto i = 0u; i < 2; ++i)
	{
		Util::DescriptorTable descriptorTable;
		descriptorTable.SetDescriptors(0, 1, &m_outputViews[UAV_TABLE_TSAMP + i].GetSRV());
		X_RETURN(m_srvTables[SRV_TABLE_TM + i], descriptorTable.GetCbvSrvUavTable(m_descriptorTableCache), false);
	}

	// RTV table
	{
		Util::DescriptorTable rtvTable;
		rtvTable.SetDescriptors(0, 1, &m_velocity.GetRTV());
		X_RETURN(m_rtvTable, rtvTable.GetRtvTable(m_descriptorTableCache), false);
	}

	// Create the sampler
	{
		Util::DescriptorTable samplerTable;
		const auto samplerAnisoWrap = SamplerPreset::ANISOTROPIC_WRAP;
		samplerTable.SetSamplers(0, 1, &samplerAnisoWrap, m_descriptorTableCache);
		X_RETURN(m_samplerTable, samplerTable.GetSamplerTable(m_descriptorTableCache), false);
	}

	return true;
}

bool RayTracer::buildAccelerationStructures(const RayTracing::CommandList& commandList, Geometry* geometries)
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
	for (const auto& bottomLevelAS : m_bottomLevelASs)
		scratchSize = (max)(bottomLevelAS.GetScratchDataMaxSize(), scratchSize);
	N_RETURN(AccelerationStructure::AllocateUAVBuffer(m_device, m_scratch, scratchSize), false);

	// Get descriptor pool and create descriptor tables
	N_RETURN(createDescriptorTables(), false);
	const auto& descriptorPool = m_descriptorTableCache.GetDescriptorPool(CBV_SRV_UAV_POOL);

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
	TopLevelAS::SetInstances(m_device, instances, NUM_MESH, m_bottomLevelASs, transforms);

	// Build bottom level ASs
	for (auto& bottomLevelAS : m_bottomLevelASs)
		bottomLevelAS.Build(commandList, m_scratch, descriptorPool, NumUAVs);

	// Build top level AS
	m_topLevelAS.Build(commandList, m_scratch, instances, descriptorPool, NumUAVs);

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
			N_RETURN(m_rayGenShaderTables[i][k].Create(m_device, 1, shaderIDSize + sizeof(RayGenConstants),
				(L"RayGenShaderTable" + to_wstring(i)).c_str()), false);
			N_RETURN(m_rayGenShaderTables[i][k].AddShaderRecord(ShaderRecord(m_device, m_rayTracingPipelines[k],
				RaygenShaderName, &RayGenConstants(), sizeof(RayGenConstants))), false);

			// Hit group shader table
			N_RETURN(m_hitGroupShaderTables[i][k].Create(m_device, 1, shaderIDSize + sizeof(HitGroupConstants),
				L"HitGroupShaderTable"), false);
			N_RETURN(m_hitGroupShaderTables[i][k].AddShaderRecord(ShaderRecord(m_device, m_rayTracingPipelines[k],
				HitGroupName, &HitGroupConstants(), sizeof(HitGroupConstants))), false);
		}

		// Miss shader table
		N_RETURN(m_missShaderTables[k].Create(m_device, 1, shaderIDSize, L"MissShaderTable"), false);
		N_RETURN(m_missShaderTables[k].AddShaderRecord(ShaderRecord(m_device, m_rayTracingPipelines[k],
			MissShaderName)), false);
	}

	return true;
}

void RayTracer::updateAccelerationStructures(const RayTracing::CommandList& commandList, uint32_t frameIndex)
{
	// Set instance
	float* const transforms[] =
	{
		reinterpret_cast<float*>(&m_worlds[GROUND]),
		reinterpret_cast<float*>(&m_worlds[MODEL_OBJ])
	};
	TopLevelAS::SetInstances(m_device, m_instances[frameIndex], NUM_MESH, m_bottomLevelASs, transforms);

	// Update top level AS
	const auto& descriptorPool = m_descriptorTableCache.GetDescriptorPool(CBV_SRV_UAV_POOL);
	m_topLevelAS.Build(commandList, m_scratch, m_instances[frameIndex], descriptorPool, NumUAVs, true);
}

void RayTracer::rayTrace(const RayTracing::CommandList& commandList, uint32_t frameIndex)
{
	commandList.SetComputePipelineLayout(m_pipelineLayouts[GLOBAL_LAYOUT]);

	// Bind the heaps, acceleration structure and dispatch rays.
	const DescriptorPool descriptorPools[] =
	{
		m_descriptorTableCache.GetDescriptorPool(CBV_SRV_UAV_POOL),
		m_descriptorTableCache.GetDescriptorPool(SAMPLER_POOL)
	};
	commandList.SetDescriptorPools(static_cast<uint32_t>(size(descriptorPools)), descriptorPools);

	commandList.SetComputeDescriptorTable(OUTPUT_VIEW, m_uavTables[UAV_TABLE_OUTPUT]);
	commandList.SetTopLevelAccelerationStructure(ACCELERATION_STRUCTURE, m_topLevelAS);
	commandList.SetComputeDescriptorTable(SAMPLER, m_samplerTable);
	commandList.SetComputeDescriptorTable(INDEX_BUFFERS, m_srvTables[SRV_TABLE_IB]);
	commandList.SetComputeDescriptorTable(VERTEX_BUFFERS, m_srvTables[SRV_TABLE_VB]);

	// Fallback layer has no depth
	commandList.DispatchRays(m_rayTracingPipelines[m_pipeIndex], m_viewport.x, m_pipeIndex == GGX ?
		m_viewport.y << 1 : m_viewport.y, 1, m_hitGroupShaderTables[frameIndex][m_pipeIndex],
		m_missShaderTables[m_pipeIndex], m_rayGenShaderTables[frameIndex][m_pipeIndex]);
}

void RayTracer::gbufferPass(const RayTracing::CommandList& commandList)
{
	// Set render target
	commandList.OMSetRenderTargets(1, m_rtvTable, &m_depth.GetDSV());

	// Clear render target
	const float clearColor[4] = {};
	commandList.ClearRenderTargetView(m_velocity.GetRTV(), clearColor);
	commandList.ClearDepthStencilView(m_depth.GetDSV(), D3D12_CLEAR_FLAG_DEPTH, 1.0f);

	// Set pipeline state
	commandList.SetGraphicsPipelineLayout(m_pipelineLayouts[GBUFFER_PASS_LAYOUT]);
	commandList.SetPipelineState(m_pipelines[GBUFFER_PASS]);

	// Set viewport
	Viewport viewport(0.0f, 0.0f, static_cast<float>(m_viewport.x), static_cast<float>(m_viewport.y));
	RectRange scissorRect(0, 0, m_viewport.x, m_viewport.y);
	commandList.RSSetViewports(1, &viewport);
	commandList.RSSetScissorRects(1, &scissorRect);

	commandList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	for (auto i = 0ui8; i < NUM_MESH; ++i)
	{
		// Set descriptor tables
		commandList.SetGraphics32BitConstants(0, SizeOfInUint32(BasePassConstants), &m_cbBasePass[i]);

		commandList.IASetVertexBuffers(0, 1, &m_vertexBuffers[i].GetVBV());
		commandList.IASetIndexBuffer(m_indexBuffers[i].GetIBV());

		commandList.DrawIndexed(m_numIndices[i], 1, 0, 0, 0);
	}
}

void RayTracer::spatialPass(const RayTracing::CommandList& commandList, uint8_t dst, uint8_t src, uint8_t srcSRV)
{
	commandList.SetComputePipelineLayout(m_pipelineLayouts[RESAMPLE_LAYOUT]);

	// Bind the heaps, acceleration structure and dispatch rays.
	const DescriptorPool descriptorPools[] =
	{
		m_descriptorTableCache.GetDescriptorPool(CBV_SRV_UAV_POOL),
		m_descriptorTableCache.GetDescriptorPool(SAMPLER_POOL)
	};
	commandList.SetDescriptorPools(static_cast<uint32_t>(size(descriptorPools)), descriptorPools);

	ResourceBarrier barriers[2];
	auto numBarriers = m_outputViews[dst].SetBarrier(barriers, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	numBarriers = m_outputViews[src].SetBarrier(barriers, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, numBarriers);
	commandList.Barrier(numBarriers, barriers);

	commandList.SetComputeDescriptorTable(OUTPUT_VIEW, m_uavTables[dst]);
	commandList.SetComputeDescriptorTable(SHADER_RESOURCES, m_srvTables[srcSRV]);
	commandList.SetComputeDescriptorTable(SAMPLER, m_samplerTable);

	if (srcSRV != SRV_TABLE_SPATIAL && srcSRV != SRV_TABLE_SPATIAL1)
		commandList.SetPipelineState(m_pipelines[SPATIAL_PASS]);
	commandList.Dispatch(DIV_UP(m_viewport.x, 8), DIV_UP(m_viewport.y, 8), 1);
}

void RayTracer::temporalSS(const RayTracing::CommandList& commandList)
{
	commandList.SetComputePipelineLayout(m_pipelineLayouts[TEMPORAL_SS_LAYOUT]);

	// Bind the heaps, acceleration structure and dispatch rays.
	const DescriptorPool descriptorPools[] =
	{
		m_descriptorTableCache.GetDescriptorPool(CBV_SRV_UAV_POOL),
		m_descriptorTableCache.GetDescriptorPool(SAMPLER_POOL)
	};
	commandList.SetDescriptorPools(static_cast<uint32_t>(size(descriptorPools)), descriptorPools);

	ResourceBarrier barriers[5];
	auto numBarriers = m_outputViews[UAV_TABLE_TSAMP + m_frameParity].SetBarrier(barriers, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	numBarriers = m_outputViews[UAV_TABLE_OUTPUT].SetBarrier(barriers, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, numBarriers);
	numBarriers = m_outputViews[UAV_TABLE_TSAMP + !m_frameParity].SetBarrier(barriers, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, numBarriers);
	numBarriers = m_velocity.SetBarrier(barriers, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		numBarriers, 0xffffffff, D3D12_RESOURCE_BARRIER_FLAG_END_ONLY);
	if (m_pipeIndex == GGX)
		numBarriers = m_outputViews[UAV_TABLE_SPATIAL].SetBarrier(barriers, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, numBarriers);
	commandList.Barrier(numBarriers, barriers);

	commandList.SetComputeDescriptorTable(OUTPUT_VIEW, m_uavTables[UAV_TABLE_TSAMP + m_frameParity]);
	commandList.SetComputeDescriptorTable(SHADER_RESOURCES, m_srvTables[SRV_TABLE_TS + m_frameParity]);
	commandList.SetComputeDescriptorTable(SAMPLER, m_samplerTable);

	commandList.SetPipelineState(m_pipelines[m_pipeIndex == GGX ? TEMPORAL_SS : TEMPORAL_AA]);
	commandList.Dispatch(DIV_UP(m_viewport.x, 8), DIV_UP(m_viewport.y, 8), 1);
}
