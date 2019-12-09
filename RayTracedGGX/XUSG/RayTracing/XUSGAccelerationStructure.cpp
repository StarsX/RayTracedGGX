//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "XUSGRayTracing.h"

using namespace std;
using namespace XUSG;
using namespace XUSG::RayTracing;

//--------------------------------------------------------------------------------------
// Acceleration structure
//--------------------------------------------------------------------------------------

uint32_t AccelerationStructure::FrameCount = 1;

AccelerationStructure::AccelerationStructure() :
	m_currentFrame(0)
{
}

AccelerationStructure::~AccelerationStructure()
{
}

RawBuffer& AccelerationStructure::GetResult()
{
	return m_results[m_currentFrame];
}

uint32_t AccelerationStructure::GetResultDataMaxSize() const
{
	return static_cast<uint32_t>(m_prebuildInfo.ResultDataMaxSizeInBytes);
}

uint32_t AccelerationStructure::GetScratchDataMaxSize() const
{
	return static_cast<uint32_t>(m_prebuildInfo.ScratchDataSizeInBytes);
}

uint32_t AccelerationStructure::GetUpdateScratchDataSize() const
{
	return static_cast<uint32_t>(m_prebuildInfo.UpdateScratchDataSizeInBytes);
}

const WRAPPED_GPU_POINTER& AccelerationStructure::GetResultPointer() const
{
	return m_pointers[m_currentFrame];
}

void AccelerationStructure::SetFrameCount(uint32_t frameCount)
{
	FrameCount = frameCount;
}

bool AccelerationStructure::AllocateUAVBuffer(const RayTracing::Device& device, Resource& resource,
	uint64_t byteWidth, ResourceState dstState)
{
	const auto heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	const auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(byteWidth, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

	V_RETURN(device.Common->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &bufferDesc,
		static_cast<D3D12_RESOURCE_STATES>(dstState), nullptr, IID_PPV_ARGS(&resource)), cerr, false);

	return true;
}

bool AccelerationStructure::AllocateUploadBuffer(const RayTracing::Device& device, Resource& resource,
	uint64_t byteWidth, void* pData)
{
	const auto heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
	const auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(byteWidth);

	V_RETURN(device.Common->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE,
		&bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
		IID_PPV_ARGS(&resource)), cerr, false);

	void* pMappedData;
	resource->Map(0, nullptr, &pMappedData);
	memcpy(pMappedData, pData, byteWidth);
	resource->Unmap(0, nullptr);

	return true;
}

bool AccelerationStructure::preBuild(const RayTracing::Device& device, uint32_t descriptorIndex,
	uint32_t numUAVs, uint32_t numSRVs)
{
	const auto& inputs = m_buildDesc.Inputs;

	m_prebuildInfo = {};
	if (device.RaytracingAPI == API::FallbackLayer)
		device.Fallback->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &m_prebuildInfo, numUAVs);
	else // DirectX Raytracing
		device.Native->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &m_prebuildInfo);

	N_RETURN(m_prebuildInfo.ResultDataMaxSizeInBytes > 0, false);

	// Allocate resources for acceleration structures.
	// Acceleration structures can only be placed in resources that are created in the default heap (or custom heap equivalent). 
	// Default heap is OK since the application doesn’t need CPU read/write access to them. 
	// The resources that will contain acceleration structures must be created in the state D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, 
	// and must have resource flag D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS. The ALLOW_UNORDERED_ACCESS requirement simply acknowledges both: 
	//  - the system will be doing this type of access in its implementation of acceleration structure builds behind the scenes.
	//  - from the app point of view, synchronization of writes/reads to acceleration structures is accomplished using UAV barriers.
	const auto initialState = device.RaytracingAPI == API::FallbackLayer ?
		static_cast<ResourceState>(device.Fallback->GetAccelerationStructureResourceState()) :
		ResourceState::RAYTRACING_ACCELERATION_STRUCTURE;

	const auto bufferCount = (inputs.Flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE)
		== D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE ? FrameCount : 1;

	m_results.resize(bufferCount);
	for (auto& result : m_results)
		N_RETURN(result.Create(device.Common, GetResultDataMaxSize(), ResourceFlag::ALLOW_UNORDERED_ACCESS,
			MemoryType::DEFAULT, initialState, numSRVs), false);

	// The Fallback Layer interface uses WRAPPED_GPU_POINTER to encapsulate the underlying pointer
	// which will either be an emulated GPU pointer for the compute - based path or a GPU_VIRTUAL_ADDRESS for the DXR path.
	if (device.RaytracingAPI == API::FallbackLayer)
	{
		m_pointers.resize(bufferCount);
		for (auto i = 0u; i < bufferCount; ++i)
			m_pointers[i] = device.Fallback->GetWrappedPointerSimple(descriptorIndex,
				m_results[i].GetResource()->GetGPUVirtualAddress());
	}

	return true;
}

//--------------------------------------------------------------------------------------
// Bottom level acceleration structure
//--------------------------------------------------------------------------------------

BottomLevelAS::BottomLevelAS() :
	AccelerationStructure()
{
}

BottomLevelAS::~BottomLevelAS()
{
}

bool BottomLevelAS::PreBuild(const RayTracing::Device& device, uint32_t numDescs,
	Geometry* geometries, uint32_t descriptorIndex, uint32_t numUAVs, BuildFlags flags)
{
	m_buildDesc = {};
	auto& inputs = m_buildDesc.Inputs;
	inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	inputs.Flags = flags;
	inputs.NumDescs = numDescs;
	inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
	inputs.pGeometryDescs = geometries;

	// Get required sizes for an acceleration structure.
	return preBuild(device, descriptorIndex, numUAVs);
}

void BottomLevelAS::Build(const RayTracing::CommandList& commandList, const Resource& scratch,
	const DescriptorPool& descriptorPool, uint32_t numUAVs, bool update)
{
	// Complete Acceleration Structure desc
	{
		if (update && (m_buildDesc.Inputs.Flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE)
			== D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE)
		{
			m_buildDesc.SourceAccelerationStructureData = m_results[m_currentFrame].GetResource()->GetGPUVirtualAddress();
			m_buildDesc.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
			m_currentFrame = (m_currentFrame + 1) % FrameCount;
		}

		m_buildDesc.DestAccelerationStructureData = m_results[m_currentFrame].GetResource()->GetGPUVirtualAddress();
		m_buildDesc.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
	}

	// Build acceleration structure.
	commandList.BuildRaytracingAccelerationStructure(&m_buildDesc, 0, nullptr, descriptorPool, numUAVs);

	// Resource barrier
	commandList.Barrier(1, &ResourceBarrier::UAV(m_results[m_currentFrame].GetResource().get()));
}

void BottomLevelAS::SetGeometries(Geometry* geometries, uint32_t numGeometries, Format vertexFormat,
	const VertexBufferView* pVBs, const IndexBufferView* pIBs, const GeometryFlags* geometryFlags,
	const ResourceView* pTransforms)
{
	for (auto i = 0u; i < numGeometries; ++i)
	{
		auto& geometryDesc = geometries[i];

		assert(pIBs[i].Format == DXGI_FORMAT_R32_UINT || pIBs[i].Format == DXGI_FORMAT_R16_UINT);
		const uint32_t strideIB = pIBs[i].Format == DXGI_FORMAT_R32_UINT ? sizeof(uint32_t) : sizeof(uint16_t);

		geometryDesc = {};
		geometryDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
		geometryDesc.Triangles.Transform3x4 = pTransforms ? pTransforms[i].resource->GetGPUVirtualAddress() + pTransforms[i].offset: 0;
		geometryDesc.Triangles.IndexFormat = pIBs[i].Format;
		geometryDesc.Triangles.VertexFormat = static_cast<decltype(geometryDesc.Triangles.VertexFormat)>(vertexFormat);
		geometryDesc.Triangles.IndexCount = pIBs[i].SizeInBytes / strideIB;
		geometryDesc.Triangles.VertexCount = pVBs[i].SizeInBytes / pVBs[i].StrideInBytes;
		geometryDesc.Triangles.IndexBuffer = pIBs ? pIBs[i].BufferLocation : 0;
		geometryDesc.Triangles.VertexBuffer.StartAddress = pVBs ? pVBs[i].BufferLocation : 0;
		geometryDesc.Triangles.VertexBuffer.StrideInBytes = pVBs ? pVBs[i].StrideInBytes : 0;

		// Mark the geometry as opaque. 
		// PERFORMANCE TIP: mark geometry as opaque whenever applicable as it can enable important ray processing optimizations.
		// Note: When rays encounter opaque geometry an any hit shader will not be executed whether it is present or not.
		geometryDesc.Flags = geometryFlags ? geometryFlags[i] : D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
	}
}

//--------------------------------------------------------------------------------------
// Top level acceleration structure
//--------------------------------------------------------------------------------------

TopLevelAS::TopLevelAS() :
	AccelerationStructure()
{
}

TopLevelAS::~TopLevelAS()
{
}

bool TopLevelAS::PreBuild(const RayTracing::Device& device, uint32_t numDescs,
	uint32_t descriptorIndex, uint32_t numUAVs, BuildFlags flags)
{
	m_buildDesc = {};
	auto& inputs = m_buildDesc.Inputs;
	inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
	inputs.Flags = flags;
	inputs.NumDescs = numDescs;
	inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;

	// Get required sizes for an acceleration structure.
	return preBuild(device, descriptorIndex, numUAVs, 1);
}

void TopLevelAS::Build(const RayTracing::CommandList& commandList, const Resource& scratch,
	const Resource& instanceDescs, const DescriptorPool& descriptorPool,
	uint32_t numUAVs, bool update)
{
	// Complete Acceleration Structure desc
	{
		if (update && (m_buildDesc.Inputs.Flags & D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE)
			== D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE)
		{
			m_buildDesc.SourceAccelerationStructureData = m_results[m_currentFrame].GetResource()->GetGPUVirtualAddress();
			m_buildDesc.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
			m_currentFrame = (m_currentFrame + 1) % FrameCount;
		}

		m_buildDesc.DestAccelerationStructureData = m_results[m_currentFrame].GetResource()->GetGPUVirtualAddress();
		m_buildDesc.Inputs.InstanceDescs = instanceDescs->GetGPUVirtualAddress();
		m_buildDesc.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
	}

	// Build acceleration structure.
	commandList.BuildRaytracingAccelerationStructure(&m_buildDesc, 0, nullptr, descriptorPool, numUAVs);
}

void TopLevelAS::SetInstances(const RayTracing::Device& device, Resource& instances,
	uint32_t numInstances, BottomLevelAS* bottomLevelASs, float* const* transforms)
{
	// Note on Emulated GPU pointers (AKA Wrapped pointers) requirement in Fallback Layer:
	// The primary point of divergence between the DXR API and the compute-based Fallback layer is the handling of GPU pointers. 
	// DXR fundamentally requires that GPUs be able to dynamically read from arbitrary addresses in GPU memory. 
	// The existing Direct Compute API today is more rigid than DXR and requires apps to explicitly inform the GPU what blocks of memory it will access with SRVs/UAVs.
	// In order to handle the requirements of DXR, the Fallback Layer uses the concept of Emulated GPU pointers, 
	// which requires apps to create views around all memory they will access for raytracing, 
	// but retains the DXR-like flexibility of only needing to bind the top level acceleration structure at DispatchRays.
	//
	// The Fallback Layer interface uses WRAPPED_GPU_POINTER to encapsulate the underlying pointer
	// which will either be an emulated GPU pointer for the compute - based path or a GPU_VIRTUAL_ADDRESS for the DXR path.
	if (device.RaytracingAPI == API::FallbackLayer)
	{
		vector<D3D12_RAYTRACING_FALLBACK_INSTANCE_DESC> instanceDescs(numInstances);
		for (auto i = 0u; i < numInstances; ++i)
		{
			memcpy(instanceDescs[i].Transform, transforms[i], sizeof(instanceDescs[i].Transform));
			instanceDescs[i].InstanceMask = 1;
			instanceDescs[i].AccelerationStructure = bottomLevelASs[i].GetResultPointer();
		}

		if (instances)
		{
			void* pMappedData;
			instances->Map(0, nullptr, &pMappedData);
			memcpy(pMappedData, instanceDescs.data(), sizeof(D3D12_RAYTRACING_FALLBACK_INSTANCE_DESC) * numInstances);
			instances->Unmap(0, nullptr);
		}
		else AllocateUploadBuffer(device, instances, sizeof(D3D12_RAYTRACING_FALLBACK_INSTANCE_DESC) * numInstances, instanceDescs.data());
	}
	else // DirectX Raytracing
	{
		vector<D3D12_RAYTRACING_INSTANCE_DESC> instanceDescs(numInstances);
		for (auto i = 0u; i < numInstances; ++i)
		{
			memcpy(instanceDescs[i].Transform, transforms[i], sizeof(instanceDescs[i].Transform));
			instanceDescs[i].InstanceMask = 1;
			instanceDescs[i].AccelerationStructure = bottomLevelASs[i].GetResult().GetResource()->GetGPUVirtualAddress();
		}

		if (instances)
		{
			void* pMappedData;
			instances->Map(0, nullptr, &pMappedData);
			memcpy(pMappedData, instanceDescs.data(), sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * numInstances);
			instances->Unmap(0, nullptr);
		}
		else AllocateUploadBuffer(device, instances, sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * numInstances, instanceDescs.data());
	}
}
