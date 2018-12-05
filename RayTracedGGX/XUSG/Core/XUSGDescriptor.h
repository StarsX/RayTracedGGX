//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

#pragma once

#include "XUSGType.h"

namespace XUSG
{
	enum SamplerPreset : uint8_t
	{
		POINT_WRAP,
		POINT_CLAMP,
		POINT_BORDER,
		POINT_LESS_EQUAL,

		LINEAR_WRAP,
		LINEAR_CLAMP,
		LINEAR_BORDER,
		LINEAR_LESS_EQUAL,

		ANISOTROPIC_WRAP,
		ANISOTROPIC_CLAMP,
		ANISOTROPIC_BORDER,
		ANISOTROPIC_LESS_EQUAL,

		NUM_SAMPLER_PRESET
	};
	
	class DescriptorTableCache;

	namespace Util
	{
		class DescriptorTable
		{
		public:
			DescriptorTable();
			virtual ~DescriptorTable();

			void SetDescriptors(uint32_t start, uint32_t num, const Descriptor *srcDescriptors);
			void SetSamplers(uint32_t start, uint32_t num, const SamplerPreset *presets,
				DescriptorTableCache &descriptorTableCache);

			XUSG::DescriptorTable CreateCbvSrvUavTable(DescriptorTableCache &descriptorTableCache);
			XUSG::DescriptorTable GetCbvSrvUavTable(DescriptorTableCache &descriptorTableCache);

			XUSG::DescriptorTable CreateSamplerTable(DescriptorTableCache &descriptorTableCache);
			XUSG::DescriptorTable GetSamplerTable(DescriptorTableCache &descriptorTableCache);

			RenderTargetTable CreateRtvTable(DescriptorTableCache &descriptorTableCache);
			RenderTargetTable GetRtvTable(DescriptorTableCache &descriptorTableCache);

			const std::string &GetKey() const;

		protected:
			std::string m_key;
		};
	}

	class DescriptorTableCache
	{
	public:
		DescriptorTableCache();
		DescriptorTableCache(const Device &device, const wchar_t *name = nullptr);
		virtual ~DescriptorTableCache();

		void SetDevice(const Device &device);
		void SetName(const wchar_t *name);

		void AllocateCbvSrvUavPool(uint32_t numDescriptors);
		void AllocateSamplerPool(uint32_t numDescriptors);
		void AllocateRtvPool(uint32_t numDescriptors);

		DescriptorTable CreateCbvSrvUavTable(const Util::DescriptorTable &util);
		DescriptorTable GetCbvSrvUavTable(const Util::DescriptorTable &util);

		DescriptorTable CreateSamplerTable(const Util::DescriptorTable &util);
		DescriptorTable GetSamplerTable(const Util::DescriptorTable &util);

		RenderTargetTable CreateRtvTable(const Util::DescriptorTable &util);
		RenderTargetTable GetRtvTable(const Util::DescriptorTable &util);

		const DescriptorPool &GetCbvSrvUavPool() const;
		const DescriptorPool &GetSamplerPool() const;

		const std::shared_ptr<Sampler> &GetSampler(SamplerPreset preset);

	protected:
		friend class Util::DescriptorTable;

		bool allocateCbvSrvUavPool(uint32_t numDescriptors);
		bool allocateSamplerPool(uint32_t numDescriptors);
		bool allocateRtvPool(uint32_t numDescriptors);
		
		bool reallocateCbvSrvUavPool(const std::string &key);
		bool reallocateSamplerPool(const std::string &key);
		bool reallocateRtvPool(const std::string &key);
		
		DescriptorTable createCbvSrvUavTable(const std::string &key);
		DescriptorTable getCbvSrvUavTable(const std::string &key);

		DescriptorTable createSamplerTable(const std::string &key);
		DescriptorTable getSamplerTable(const std::string &key);

		RenderTargetTable createRtvTable(const std::string &key);
		RenderTargetTable getRtvTable(const std::string &key);

		Device m_device;

		std::unordered_map<std::string, DescriptorTable> m_cbvSrvUavTables;
		std::unordered_map<std::string, DescriptorTable> m_samplerTables;
		std::unordered_map<std::string, RenderTargetTable> m_rtvTables;

		DescriptorPool	m_cbvSrvUavPool;
		DescriptorPool	m_samplerPool;
		DescriptorPool	m_rtvPool;

		uint32_t		m_strideCbvSrvUav;
		uint32_t		m_numCbvSrvUavs;

		uint32_t		m_strideSampler;
		uint32_t		m_numSamplers;

		uint32_t		m_strideRtv;
		uint32_t		m_numRtvs;

		std::shared_ptr<Sampler> m_samplerPresets[NUM_SAMPLER_PRESET];
		std::function<Sampler()> m_pfnSamplers[NUM_SAMPLER_PRESET];

		std::wstring	m_name;
	};
}
