//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "XUSGRayTracing.h"

using namespace std;
using namespace XUSG;
using namespace XUSG::RayTracing;

RayTracing::CommandList::CommandList() :
	XUSG::CommandList()
{
}

RayTracing::CommandList::~CommandList()
{
}

bool RayTracing::CommandList::CreateRaytracingInterfaces(const RayTracing::Device& device)
{
	m_raytracingAPI = device.RaytracingAPI;

	if (m_raytracingAPI == API::FallbackLayer)
		device.Fallback->QueryRaytracingCommandList(m_commandList.get(), IID_PPV_ARGS(&m_fallback));
	else // DirectX Raytracing
	{
		const auto hr = m_commandList->QueryInterface(IID_PPV_ARGS(&m_native));
		if (FAILED(hr))
		{
			OutputDebugString(L"Couldn't get DirectX Raytracing interface for the command list.\n");

			return false;
		}
	}

	return true;
}

void RayTracing::CommandList::BuildRaytracingAccelerationStructure(const BuildDesc* pDesc, uint32_t numPostbuildInfoDescs,
	const PostbuildInfo* pPostbuildInfoDescs, const DescriptorPool& descriptorPool, uint32_t numUAVs) const
{
	if (m_raytracingAPI == API::FallbackLayer)
	{
		// Set the descriptor heaps to be used during acceleration structure build for the Fallback Layer.
		m_fallback->SetDescriptorHeaps(1, descriptorPool.GetAddressOf());
		m_fallback->BuildRaytracingAccelerationStructure(pDesc, numPostbuildInfoDescs, pPostbuildInfoDescs, numUAVs);
	}
	else // DirectX Raytracing
		m_native->BuildRaytracingAccelerationStructure(pDesc, numPostbuildInfoDescs, pPostbuildInfoDescs);
}

void RayTracing::CommandList::SetDescriptorPools(uint32_t numDescriptorPools, const DescriptorPool* pDescriptorPools) const
{
	vector<DescriptorPool::element_type*> ppDescriptorPools(numDescriptorPools);
	for (auto i = 0u; i < numDescriptorPools; ++i)
		ppDescriptorPools[i] = pDescriptorPools[i].get();

	if (m_raytracingAPI == API::FallbackLayer)
		m_fallback->SetDescriptorHeaps(numDescriptorPools, ppDescriptorPools.data());
	else // DirectX Raytracing
		m_commandList->SetDescriptorHeaps(numDescriptorPools, ppDescriptorPools.data());
}

void RayTracing::CommandList::SetTopLevelAccelerationStructure(uint32_t index, const TopLevelAS& topLevelAS) const
{
	if (m_raytracingAPI == API::FallbackLayer)
		m_fallback->SetTopLevelAccelerationStructure(index, topLevelAS.GetResultPointer());
	else // DirectX Raytracing
		SetComputeRootShaderResourceView(index, const_cast<TopLevelAS&>(topLevelAS).GetResult().GetResource());
}

void RayTracing::CommandList::DispatchRays(const RayTracing::Pipeline& pipeline,
	uint32_t width, uint32_t height, uint32_t depth,
	const ShaderTable& hitGroup,
	const ShaderTable& miss,
	const ShaderTable& rayGen) const
{
	auto dispatchRays = [&](auto* commandList, auto* stateObject, auto* dispatchDesc)
	{
		// Since each shader table has only one shader record, the stride is same as the size.
		dispatchDesc->HitGroupTable.StartAddress = hitGroup.GetResource()->GetGPUVirtualAddress();
		dispatchDesc->HitGroupTable.SizeInBytes = hitGroup.GetResource()->GetDesc().Width;
		dispatchDesc->HitGroupTable.StrideInBytes = hitGroup.GetShaderRecordSize();
		dispatchDesc->MissShaderTable.StartAddress = miss.GetResource()->GetGPUVirtualAddress();
		dispatchDesc->MissShaderTable.SizeInBytes = miss.GetResource()->GetDesc().Width;
		dispatchDesc->MissShaderTable.StrideInBytes = miss.GetShaderRecordSize();
		dispatchDesc->RayGenerationShaderRecord.StartAddress = rayGen.GetResource()->GetGPUVirtualAddress();
		dispatchDesc->RayGenerationShaderRecord.SizeInBytes = rayGen.GetResource()->GetDesc().Width;
		dispatchDesc->Width = width;
		dispatchDesc->Height = height;
		dispatchDesc->Depth = depth;
		commandList->SetPipelineState1(stateObject);
		commandList->DispatchRays(dispatchDesc);
	};

	D3D12_DISPATCH_RAYS_DESC dispatchDesc = {};
	if (m_raytracingAPI == API::FallbackLayer)
		dispatchRays(m_fallback.get(), pipeline.Fallback.get(), &dispatchDesc);
	else // DirectX Raytracing
		dispatchRays(m_native.get(), pipeline.Native.get(), &dispatchDesc);
}
