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

const wchar_t* RayTracer::HitGroupName = L"hitGroup";
const wchar_t* RayTracer::RaygenShaderName = L"raygenMain";
const wchar_t* RayTracer::ClosestHitShaderName = L"closestHitMain";
const wchar_t* RayTracer::MissShaderName = L"missMain";

RayTracer::RayTracer(const RayTracing::Device &device, const RayTracing::CommandList &commandList) :
	m_device(device),
	m_commandList(commandList),
	m_vertexStride(0),
	m_numIndices(0)
{
	m_pipelineCache.SetDevice(device);
	m_descriptorTableCache.SetDevice(device.Common);
	m_pipelineLayoutCache.SetDevice(device.Common);
}

RayTracer::~RayTracer()
{
}

bool RayTracer::Init(uint32_t width, uint32_t height, Resource &vbUpload, Resource &ibUpload,
	Resource &scratch, Resource &instances, const char *fileName)
{
	m_viewport.x = static_cast<float>(width);
	m_viewport.y = static_cast<float>(height);

	// Load inputs
	ObjLoader objLoader;
	if (!objLoader.Import(fileName, true, true)) return false;
	N_RETURN(createVB(objLoader.GetNumVertices(), objLoader.GetVertexStride(), objLoader.GetVertices(), vbUpload), false);
	N_RETURN(createIB(objLoader.GetNumIndices(), objLoader.GetIndices(), ibUpload), false);

	// Create raytracing pipeline
	createPipelineLayouts();
	createPipeline();

	// Create output view and build acceleration structures
	m_outputView.Create(m_device.Common, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	buildAccelerationStructures(scratch, instances);
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
	XMStoreFloat4x4(&m_cbRayGens[frameIndex].ProjToWorld, XMMatrixInverse(nullptr, viewProj));
	XMStoreFloat4(&m_cbRayGens[frameIndex].EyePt, eyePt);

	m_rayGenShaderTables[frameIndex].Reset();
	m_rayGenShaderTables[frameIndex].AddShaderRecord(ShaderRecord(m_device, m_pipelines[PHONG],
		RaygenShaderName, &m_cbRayGens[frameIndex], sizeof(RayGenConstants)));
}

void RayTracer::Render(uint32_t frameIndex, const Descriptor &dsv)
{
	m_outputView.Barrier(m_commandList.Common, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	rayTrace(frameIndex);
}

const Texture2D &RayTracer::GetOutputView() const
{
	return m_outputView;
}

bool RayTracer::createVB(uint32_t numVert, uint32_t stride, const uint8_t *pData, Resource &vbUpload)
{
	m_vertexStride = stride;
	N_RETURN(m_vertexBuffer.Create(m_device.Common, numVert, stride, D3D12_RESOURCE_FLAG_NONE,
		D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST), false);

	return m_vertexBuffer.Upload(m_commandList.Common, vbUpload, pData,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

bool RayTracer::createIB(uint32_t numIndices, const uint32_t *pData, Resource &ibUpload)
{
	m_numIndices = numIndices;
	N_RETURN(m_indexBuffer.Create(m_device.Common, sizeof(uint32_t) * numIndices, DXGI_FORMAT_R32_UINT,
		D3D12_RESOURCE_FLAG_NONE, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST), false);

	return m_indexBuffer.Upload(m_commandList.Common, ibUpload, pData,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
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
		pipelineLayout.SetRange(GEOMETRY_BUFFERS, DescriptorType::SRV, 2, 1);
		m_pipelineLayouts[GLOBAL_LAYOUT] = pipelineLayout.GetPipelineLayout(m_device, m_pipelineLayoutCache,
			D3D12_ROOT_SIGNATURE_FLAG_NONE, NumUAVs);
	}

	// Local Root Signature for RayGen shader
	// This is a root signature that enables a shader to have unique arguments that come from shader tables.
	{
		RayTracing::PipelineLayout pipelineLayout;
		pipelineLayout.SetConstants(0, SizeOfInUint32(RayGenConstants), 0);
		m_pipelineLayouts[RAY_GEN_LAYOUT] = pipelineLayout.GetPipelineLayout(m_device, m_pipelineLayoutCache,
			D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE, NumUAVs);
	}
}

void RayTracer::createPipeline()
{
	m_pipelineCache.SetDevice(m_device);

	{
		ThrowIfFailed(D3DReadFileToBlob(L"RayTracedPhong.cso", &m_shaderLib));

		RayTracing::State state;
		state.SetShaderLibrary(m_shaderLib);
		state.SetHitGroup(0, HitGroupName, ClosestHitShaderName);
		state.SetShaderConfig(sizeof(XMFLOAT4), sizeof(XMFLOAT2));
		state.SetLocalPipelineLayout(0, m_pipelineLayouts[RAY_GEN_LAYOUT],
			1, reinterpret_cast<const void**>(&RaygenShaderName));
		state.SetGlobalPipelineLayout(m_pipelineLayouts[GLOBAL_LAYOUT]);
		state.SetMaxRecursionDepth(1);
		m_pipelines[PHONG] = state.GetPipeline(m_pipelineCache);
	}
}

void RayTracer::createDescriptorTables(vector<Descriptor> &descriptors)
{
	{
		descriptors.push_back(m_bottomLevelAS.GetResult().GetUAV());
		descriptors.push_back(m_topLevelAS.GetResult().GetUAV());
		// 3 UAVs, so NumUAVs = 3.
		Util::DescriptorTable descriptorTable;
		descriptorTable.SetDescriptors(0, static_cast<uint32_t>(descriptors.size()), descriptors.data());
		m_uavTables[UAV_TABLE_OUTPUT] = descriptorTable.GetCbvSrvUavTable(m_descriptorTableCache);
	}

	{
		Util::DescriptorTable descriptorTable;
		descriptorTable.SetDescriptors(0, 1, &m_topLevelAS.GetResult().GetSRV());
		m_srvTables[SRV_TABLE_AS] = descriptorTable.GetCbvSrvUavTable(m_descriptorTableCache);
	}

	{
		Descriptor descriptors[] = { m_indexBuffer.GetSRV(), m_vertexBuffer.GetSRV() };
		Util::DescriptorTable descriptorTable;
		descriptorTable.SetDescriptors(0, _countof(descriptors), descriptors);
		m_srvTables[SRV_TABLE_IB_VB] = descriptorTable.GetCbvSrvUavTable(m_descriptorTableCache);
	}
}

bool RayTracer::buildAccelerationStructures(XUSG::Resource &scratch, XUSG::Resource &instances)
{
	// Set geometries
	const auto numDescs = 1u;
	vector<Geometry> geometries(numDescs);
	BottomLevelAS::SetGeometries(geometries.data(), numDescs, DXGI_FORMAT_R32G32B32_FLOAT,
		&m_vertexBuffer.GetVBV(), &m_indexBuffer.GetIBV());

	// Descriptors
	vector<Descriptor> descriptors = { m_outputView.GetUAV() };
	const uint32_t bottomLevelASIndex = static_cast<uint32_t>(descriptors.size());
	const uint32_t topLevelASIndex = bottomLevelASIndex + 1;

	// Prebuild
	N_RETURN(m_bottomLevelAS.PreBuild(m_device, numDescs, geometries.data(), bottomLevelASIndex, NumUAVs), false);
	N_RETURN(m_topLevelAS.PreBuild(m_device, numDescs, topLevelASIndex, NumUAVs), false);

	// Create scratch buffer
	const auto scratchSize = (max)(m_bottomLevelAS.GetScratchDataMaxSize(), m_topLevelAS.GetScratchDataMaxSize());
	N_RETURN(AccelerationStructure::AllocateUAVBuffer(m_device.Common, scratch, scratchSize), false);

	// Get descriptor pool and create descriptor tables
	createDescriptorTables(descriptors);

	// Set instance
	const auto numInstances = 1u;
	float *transforms[] = { XMMatrixIdentity().r[0].m128_f32 };
	TopLevelAS::SetInstances(m_device, instances, 1, &m_bottomLevelAS, transforms);

	// Build bottom level AS
	N_RETURN(m_bottomLevelAS.Build(m_device, m_commandList, scratch,
		m_descriptorTableCache.GetCbvSrvUavPool(), NumUAVs), false);

	// Barrier
	AccelerationStructure::Barrier(m_commandList, numInstances, &m_bottomLevelAS);

	// Build top level AS
	return m_topLevelAS.Build(m_device, m_commandList, scratch, instances,
		m_descriptorTableCache.GetCbvSrvUavPool(), NumUAVs);
}

void RayTracer::buildShaderTables()
{
	// Get shader identifiers.
	const auto shaderIDSize = ShaderRecord::GetShaderIDSize(m_device);

	// Ray gen shader table
	for (auto i = 0ui8; i < FrameCount; ++i)
	{
		m_rayGenShaderTables[i].Create(m_device, 1, shaderIDSize + sizeof(RayGenConstants), L"RayGenShaderTable");
		m_rayGenShaderTables[i].AddShaderRecord(ShaderRecord(m_device, m_pipelines[PHONG],
			RaygenShaderName, &m_cbRayGens[i], sizeof(RayGenConstants)));
	}

	// Miss shader table
	m_missShaderTable.Create(m_device, 1, shaderIDSize, L"MissShaderTable");
	m_missShaderTable.AddShaderRecord(ShaderRecord(m_device, m_pipelines[PHONG], MissShaderName));

	// Hit group shader table
	m_hitGroupShaderTable.Create(m_device, 1, shaderIDSize, L"HitGroupShaderTable");
	m_hitGroupShaderTable.AddShaderRecord(ShaderRecord(m_device, m_pipelines[PHONG], HitGroupName));
}

void RayTracer::rayTrace(uint32_t frameIndex)
{
	const auto &commandList = m_commandList.Common;
	commandList->SetComputeRootSignature(m_pipelineLayouts[GLOBAL_LAYOUT].Get());

	// Bind the heaps, acceleration structure and dispatch rays.
	const DescriptorPool descriptorPools[] =
	{
		m_descriptorTableCache.GetCbvSrvUavPool(),
		m_descriptorTableCache.GetSamplerPool()
	};
	SetDescriptorPool(m_device, m_commandList, 2, descriptorPools);

	commandList->SetComputeRootDescriptorTable(OUTPUT_VIEW, *m_uavTables[UAV_TABLE_OUTPUT]);
	SetTopLevelAccelerationStructure(m_device, m_commandList, ACCELERATION_STRUCTURE, m_topLevelAS, m_srvTables[SRV_TABLE_AS]);
	commandList->SetComputeRootDescriptorTable(SAMPLER, *m_samplerTable);
	commandList->SetComputeRootDescriptorTable(GEOMETRY_BUFFERS, *m_srvTables[SRV_TABLE_IB_VB]);

	const auto width = static_cast<uint32_t>(m_viewport.x);
	const auto height = static_cast<uint32_t>(m_viewport.y);
	DispatchRays(m_device, m_commandList, m_pipelines[PHONG], width, height, 1,
		m_hitGroupShaderTable, m_missShaderTable, m_rayGenShaderTables[frameIndex]);
}
