//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

#pragma once

#include "Core/XUSGResource.h"
#include "RayTracing/XUSGRayTracingState.h"

namespace XUSG
{
	namespace RayTracing
	{
		class ShaderRecord
		{
		public:
			ShaderRecord(const Device &device, const Pipeline &pipeline, const void *shader,
				void *pLocalDescriptorArgs = nullptr, uint32_t localDescriptorArgSize = 0);
			ShaderRecord(void *pShaderID, uint32_t shaderIDSize,
				void *pLocalDescriptorArgs = nullptr, uint32_t localDescriptorArgSize = 0);
			virtual ~ShaderRecord();

			void CopyTo(void *dest) const;

			static uint32_t GetShaderIDSize(const Device &device);

		protected:
			struct PointerWithSize
			{
				PointerWithSize() : Ptr(nullptr), Size(0) {}
				PointerWithSize(void *ptr, uint32_t size) : Ptr(ptr), Size(size) {};

				void *Ptr;
				uint32_t Size;
			};
			PointerWithSize m_shaderID;
			PointerWithSize m_localDescriptorArgs;
		};

		class ShaderTable
		{
		public:
			ShaderTable();
			virtual ~ShaderTable();

			bool Create(const Device &device, uint32_t numShaderRecords, uint32_t shaderRecordSize,
				const wchar_t *name = nullptr);

			bool AddShaderRecord(const ShaderRecord& shaderRecord);

			void *Map();
			void Unmap();
			void Reset();

			const Resource& GetResource() const;
			uint32_t GetShaderRecordSize() const;

		protected:
			bool allocate(const Device &device, uint32_t byteWidth, const wchar_t *name);

			Resource m_resource;

			//std::vector<ShaderRecord> m_shaderRecords;

			void *m_mappedShaderRecords;
			uint32_t m_shaderRecordSize;
		};
	}
}
