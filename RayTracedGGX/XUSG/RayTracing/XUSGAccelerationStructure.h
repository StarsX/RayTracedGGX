//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#pragma once

#include "XUSGRayTracingType.h"
#include "Core/XUSGResource.h"

namespace XUSG
{
	namespace RayTracing
	{
		struct ResourceView
		{
			Resource resource;
			uint64_t offset;
		};

		class AccelerationStructure
		{
		public:
			AccelerationStructure();
			virtual ~AccelerationStructure();

			RawBuffer& GetResult();

			uint32_t GetResultDataMaxSize() const;
			uint32_t GetScratchDataMaxSize() const;
			uint32_t GetUpdateScratchDataSize() const;
			const WRAPPED_GPU_POINTER& GetResultPointer() const;

			static void SetFrameCount(uint32_t frameCount);

			static bool AllocateUAVBuffer(const Device& device, Resource& resource,
				uint64_t byteWidth, ResourceState dstState = ResourceState(0x8));
			static bool AllocateUploadBuffer(const Device& device, Resource& resource,
				uint64_t byteWidth, void* pData);

		protected:
			bool preBuild(const RayTracing::Device& device, uint32_t descriptorIndex,
				uint32_t numUAVs, uint32_t numSRVs = 0);

			BuildDesc		m_buildDesc;
			PrebuildInfo	m_prebuildInfo;

			std::vector<RawBuffer> m_results;
			std::vector<WRAPPED_GPU_POINTER> m_pointers;

			uint32_t		m_currentFrame;

			static uint32_t FrameCount;
		};

		class BottomLevelAS :
			public AccelerationStructure
		{
		public:
			BottomLevelAS();
			virtual ~BottomLevelAS();

			bool PreBuild(const RayTracing::Device& device, uint32_t numDescs, Geometry* geometries,
				uint32_t descriptorIndex, uint32_t numUAVs, BuildFlags flags = BuildFlags(0x4));
			void Build(const RayTracing::CommandList& commandList, const Resource& scratch,
				const DescriptorPool& descriptorPool, uint32_t numUAVs, bool update = false);

			static void SetGeometries(Geometry* geometries, uint32_t numGeometries, Format vertexFormat,
				const VertexBufferView* pVBs, const IndexBufferView* pIBs = nullptr,
				const GeometryFlags* geometryFlags = nullptr, const ResourceView* pTransforms = nullptr);
		};

		class TopLevelAS :
			public AccelerationStructure
		{
		public:
			TopLevelAS();
			virtual ~TopLevelAS();

			bool PreBuild(const RayTracing::Device& device, uint32_t numDescs, uint32_t descriptorIndex,
				uint32_t numUAVs, BuildFlags flags = BuildFlags(0x4));
			void Build(const RayTracing::CommandList& commandList, const Resource& scratch,
				const Resource& instanceDescs, const DescriptorPool& descriptorPool,
				uint32_t numUAVs, bool update = false);

			static void SetInstances(const RayTracing::Device& device, Resource& instances,
				uint32_t numInstances, BottomLevelAS* bottomLevelASs, float* const* transforms);
		};
	}
}
