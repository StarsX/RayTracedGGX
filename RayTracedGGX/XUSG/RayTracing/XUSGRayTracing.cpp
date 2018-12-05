//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

#include "XUSGRayTracing.h"

using namespace std;
using namespace XUSG;
using namespace XUSG::RayTracing;

void RayTracing::SetDescriptorPool(const RayTracing::Device &device, const RayTracing::CommandList &commandList,
	uint32_t numDescriptorPools, const DescriptorPool *pDescriptorPools)
{
	vector<DescriptorPool::InterfaceType*> ppDescriptorPools(numDescriptorPools);
	for (auto i = 0u; i < numDescriptorPools; ++i)
		ppDescriptorPools[i] = pDescriptorPools[i].Get();

	if (device.RaytracingAPI == RayTracing::API::FallbackLayer)
		commandList.Fallback->SetDescriptorHeaps(numDescriptorPools, ppDescriptorPools.data());
	else // DirectX Raytracing
		commandList.Common->SetDescriptorHeaps(numDescriptorPools, ppDescriptorPools.data());
}

void RayTracing::SetTopLevelAccelerationStructure(const Device &device, const CommandList &commandList,
	uint32_t index, const TopLevelAS &topLevelAS, const DescriptorTable &srvTopLevelASTable)
{
	if (device.RaytracingAPI == RayTracing::API::FallbackLayer)
		commandList.Fallback->SetTopLevelAccelerationStructure(index, topLevelAS.GetResultPointer());
	else // DirectX Raytracing
		commandList.Common->SetComputeRootDescriptorTable(index, *srvTopLevelASTable);
}

void RayTracing::DispatchRays(const RayTracing::Device &device, const RayTracing::CommandList &commandList,
	const RayTracing::Pipeline &pipeline, uint32_t width, uint32_t height, uint32_t depth,
	const ShaderTable &hitGroup, const ShaderTable &miss, const ShaderTable &rayGen)
{
	auto dispatchRays = [&](auto *commandList, auto *stateObject, auto *dispatchDesc)
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
	if (device.RaytracingAPI == RayTracing::API::FallbackLayer)
		dispatchRays(commandList.Fallback.Get(), pipeline.Fallback.Get(), &dispatchDesc);
	else // DirectX Raytracing
		dispatchRays(commandList.DXR.Get(), pipeline.DXR.Get(), &dispatchDesc);
}
