//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

#pragma once

#include "XUSGRayTracingType.h"
#include "Core/XUSGPipelineLayout.h"

namespace XUSG
{
	namespace RayTracing
	{
		class PipelineLayout :
			public Util::PipelineLayout
		{
		public:
			PipelineLayout();
			virtual ~PipelineLayout();

			XUSG::PipelineLayout CreatePipelineLayout(const Device &device, PipelineLayoutCache &pipelineLayoutCache,
				uint8_t flags, uint32_t numUAVs, const wchar_t *name = nullptr);
			XUSG::PipelineLayout GetPipelineLayout(const Device &device, PipelineLayoutCache &pipelineLayoutCache,
				uint8_t flags, uint32_t numUAVs, const wchar_t *name = nullptr);
		};
	}
}
