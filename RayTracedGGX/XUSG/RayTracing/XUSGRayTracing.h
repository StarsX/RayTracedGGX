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
		class CommandList :
			public XUSG::CommandList
		{
		public:
			CommandList();
			virtual ~CommandList();

			bool CreateRaytracingInterfaces(const Device& device);

			void BuildRaytracingAccelerationStructure(const BuildDesc* pDesc,
				uint32_t numPostbuildInfoDescs,
				const PostbuildInfo* pPostbuildInfoDescs,
				const DescriptorPool& descriptorPool,
				uint32_t numUAVs) const;

			void SetDescriptorPools(uint32_t numDescriptorPools, const DescriptorPool* pDescriptorPools) const;
			void SetTopLevelAccelerationStructure(uint32_t index, const TopLevelAS& topLevelAS) const;
			void DispatchRays(const Pipeline& pipeline, uint32_t width, uint32_t height, uint32_t depth,
				const ShaderTable& hitGroup, const ShaderTable& miss, const ShaderTable& rayGen) const;

		protected:
			FallbackCommandList m_fallback;
			NativeCommandList m_native;

			API m_raytracingAPI;
		};
	}
}
