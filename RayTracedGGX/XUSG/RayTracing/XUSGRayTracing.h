//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

#pragma once

#include "RayTracing/XUSGRayTracingPipelineLayout.h"
#include "RayTracing/XUSGShaderTable.h"
#include "RayTracing/XUSGAccelerationStructure.h"

namespace XUSG
{
	namespace RayTracing
	{
		void SetDescriptorPool(const Device &device, const CommandList &commandList,
			uint32_t numDescriptorPools, const DescriptorPool *pDescriptorPools);

		void SetTopLevelAccelerationStructure(const Device &device, const CommandList &commandList,
			uint32_t index, const TopLevelAS &topLevelAS, const DescriptorTable &srvTopLevelASTable);

		void DispatchRays(const Device &device, const CommandList &commandList,
			const Pipeline &pipeline, uint32_t width, uint32_t height, uint32_t depth,
			const ShaderTable &hitGroup, const ShaderTable &miss, const ShaderTable &rayGen);
	}
}
