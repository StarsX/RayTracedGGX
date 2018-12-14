//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

#pragma once

#include "XUSGRayTracingType.h"
#include "Core/XUSGResource.h"

namespace XUSG
{
	namespace RayTracing
	{
		class AccelerationStructure
		{
		public:
			AccelerationStructure();
			virtual ~AccelerationStructure();

			RawBuffer &GetResult();

			uint32_t GetResultDataMaxSize() const;
			uint32_t GetScratchDataMaxSize() const;
			uint32_t GetUpdateScratchDataSize() const;
			const WRAPPED_GPU_POINTER &GetResultPointer() const;
			
			static bool AllocateUAVBuffer(const XUSG::Device &device, Resource &resource,
				uint64_t byteWidth, ResourceState dstState = ResourceState(0x8));
			static bool AllocateUploadBuffer(const XUSG::Device &device, Resource &resource,
				uint64_t byteWidth, void *pData);

			static void Barrier(RayTracing::CommandList &commandList,
				uint32_t numInstances, AccelerationStructure *bottomLevelASs);

		protected:
			bool preBuild(const RayTracing::Device &device, uint32_t descriptorIndex,
				uint32_t numUAVs, uint32_t numSRVs = 0);

			BuildDesc		m_buildDesc;
			PrebuildInfo	m_prebuildInfo;

			RawBuffer		m_result;
			WRAPPED_GPU_POINTER m_pointer;
		};

		class BottomLevelAS :
			public AccelerationStructure
		{
		public:
			BottomLevelAS();
			virtual ~BottomLevelAS();

			bool PreBuild(const RayTracing::Device &device, uint32_t numDescs, Geometry *geometries,
				uint32_t descriptorIndex, uint32_t numUAVs, BuildFlags flags = BuildFlags(0x4));
			void Build(const RayTracing::CommandList &commandList, const Resource &scratch,
				const DescriptorPool &descriptorPool, uint32_t numUAVs);

			static void SetGeometries(Geometry *geometries, uint32_t numGeometries, Format vertexFormat,
				const VertexBufferView *pVBs, const IndexBufferView *pIBs = nullptr,
				const GeometryFlags *geometryFlags = nullptr);
		};

		class TopLevelAS :
			public AccelerationStructure
		{
		public:
			TopLevelAS();
			virtual ~TopLevelAS();

			bool PreBuild(const RayTracing::Device &device, uint32_t numDescs, uint32_t descriptorIndex,
				uint32_t numUAVs, BuildFlags flags = BuildFlags(0x4));
			void Build(const RayTracing::CommandList &commandList, const Resource &scratch,
				const Resource &instanceDescs, const DescriptorPool &descriptorPool, uint32_t numUAVs);

			static void SetInstances(const RayTracing::Device &device, Resource &instances,
				uint32_t numInstances, BottomLevelAS *bottomLevelASs, float *const *transforms);
		};
	}
}
