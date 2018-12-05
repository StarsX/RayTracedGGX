//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

#pragma once

#include "XUSGDescriptor.h"

#define	BIND_PACKED_UAV	ResourceFlags(0x4 | 0x8000)
#define ALIGN(x, n)		(((x) + (n - 1)) & ~(n - 1))

namespace XUSG
{
	//--------------------------------------------------------------------------------------
	// Constant buffer
	//--------------------------------------------------------------------------------------
	class ConstantBuffer
	{
	public:
		ConstantBuffer();
		virtual ~ConstantBuffer();

		bool Create(const Device &device, uint32_t byteWidth,
			uint32_t numCBVs = 1, const uint32_t *offsets = nullptr,
			const wchar_t *name = nullptr);

		void *Map(uint32_t i = 0);
		void Unmap();

		const Resource	&GetResource() const;
		Descriptor		GetCBV(uint32_t i = 0) const;

	protected:
		bool allocateDescriptorPool(uint32_t numDescriptors, const wchar_t *name);

		Device			m_device;

		Resource		m_resource;
		DescriptorPool	m_cbvPool;
		std::vector<Descriptor> m_CBVs;
		Descriptor		m_cbvCurrent;
		
		std::vector<uint32_t> m_CBVOffsets;

		void			*m_pDataBegin;
	};

	//--------------------------------------------------------------------------------------
	// Resource base
	//--------------------------------------------------------------------------------------
	class ResourceBase
	{
	public:
		ResourceBase();
		virtual ~ResourceBase();

		void Barrier(const GraphicsCommandList &commandList, ResourceState dstState);

		const Resource	&GetResource() const;
		Descriptor		GetSRV(uint32_t i = 0) const;

		ResourceBarrier	Transition(ResourceState dstState);

		//static void CreateReadBuffer(const Device &device,
			//CPDXBuffer &pDstBuffer, const CPDXBuffer &pSrcBuffer);
	protected:
		void setDevice(const Device &device);
		bool allocateDescriptorPool(uint32_t numDescriptors);

		Device			m_device;

		Resource		m_resource;
		DescriptorPool	m_srvUavPool;
		std::vector<Descriptor> m_SRVs;
		Descriptor		m_srvUavCurrent;

		ResourceState	m_state;
		uint32_t		m_strideSrvUav;

		std::wstring	m_name;
	};

	//--------------------------------------------------------------------------------------
	// 2D Texture
	//--------------------------------------------------------------------------------------
	class Texture2D :
		public ResourceBase
	{
	public:
		Texture2D();
		virtual ~Texture2D();

		bool Create(const Device &device, uint32_t width, uint32_t height, Format format,
			uint32_t arraySize = 1, ResourceFlags resourceFlags = ResourceFlags(0),
			uint8_t numMips = 1, uint8_t sampleCount = 1, PoolType poolType = PoolType(1),
			ResourceState state = ResourceState(0), const wchar_t *name = nullptr);
		bool Upload(const GraphicsCommandList &commandList, Resource &resourceUpload,
			SubresourceData *pSubresourceData, uint32_t numSubresources = 1,
			ResourceState dstState = ResourceState(0));
		bool Upload(const GraphicsCommandList &commandList, Resource &resourceUpload, const uint8_t *pData,
			uint8_t stride = sizeof(float), ResourceState dstState = ResourceState(0));

		void CreateSRVs(uint32_t arraySize, Format format = Format(0),
			uint8_t numMips = 1, uint8_t sampleCount = 1);
		void CreateSRVLevels(uint32_t arraySize, uint8_t numMips, Format format = Format(0), uint8_t sampleCount = 1);
		void CreateUAVs(uint32_t arraySize, Format format = Format(0), uint8_t numMips = 1);

		Descriptor GetUAV(uint8_t i = 0) const;
		Descriptor GetSRVLevel(uint8_t i) const;

	protected:
		Resource m_counter;
		std::vector<Descriptor>	m_UAVs;
		std::vector<Descriptor>	m_SRVLevels;
	};

	//--------------------------------------------------------------------------------------
	// Render target
	//--------------------------------------------------------------------------------------
	class RenderTarget :
		public Texture2D
	{
	public:
		RenderTarget();
		virtual ~RenderTarget();

		bool Create(const Device &device, uint32_t width, uint32_t height, Format format,
			uint32_t arraySize = 1, ResourceFlags resourceFlags = ResourceFlags(0),
			uint8_t numMips = 1, uint8_t sampleCount = 1, ResourceState state = ResourceState(0),
			const wchar_t *name = nullptr);
		bool CreateArray(const Device &device, uint32_t width, uint32_t height, uint32_t arraySize,
			Format format, ResourceFlags resourceFlags = ResourceFlags(0), uint8_t numMips = 1,
			uint8_t sampleCount = 1, ResourceState state = ResourceState(0),
			const wchar_t *name = nullptr);

		//void Populate(const CPDXShaderResourceView &pSRVSrc, const spShader &pShader,
			//const uint8_t uSRVSlot = 0, const uint8_t uSlice = 0, const uint8_t uMip = 0);

		Descriptor	GetRTV(uint32_t slice = 0, uint8_t mipLevel = 0) const;
		uint32_t	GetArraySize() const;
		uint8_t		GetNumMips(uint32_t slice = 0) const;

	protected:
		bool create(const Device &device, uint32_t width, uint32_t height,
			uint32_t arraySize, Format format, uint8_t numMips, uint8_t sampleCount,
			ResourceFlags resourceFlags, ResourceState state, const wchar_t *name);
		bool allocateRtvPool(uint32_t numDescriptors);

		DescriptorPool	m_rtvPool;
		std::vector<std::vector<Descriptor>> m_RTVs;
		Descriptor		m_rtvCurrent;

		uint32_t		m_strideRtv;
	};

	//--------------------------------------------------------------------------------------
	// Depth stencil
	//--------------------------------------------------------------------------------------
	class DepthStencil :
		public Texture2D
	{
	public:
		DepthStencil();
		virtual ~DepthStencil();

		bool Create(const Device &device, uint32_t width, uint32_t height, Format format =
			DXGI_FORMAT_D24_UNORM_S8_UINT, ResourceFlags resourceFlags = ResourceFlags(0),
			uint32_t arraySize = 1, uint8_t numMips = 1, uint8_t sampleCount = 1,
			ResourceState state = ResourceState(0), uint8_t clearStencil = 0,
			float clearDepth = 1.0f, const wchar_t *name = nullptr);

		Descriptor GetDSV(uint8_t mipLevel = 0) const;
		Descriptor GetDSVReadOnly(uint8_t mipLevel = 0) const;
		const Descriptor &GetSRVStencil() const;

		const uint8_t GetNumMips() const;

	protected:
		bool allocateDsvPool(uint32_t numDescriptors);

		DescriptorPool m_dsvPool;
		std::vector<Descriptor> m_DSVs;
		std::vector<Descriptor> m_DSVROs;
		Descriptor	m_SRVStencil;
		Descriptor	m_dsvCurrent;

		uint32_t	m_strideDsv;
	};

	//--------------------------------------------------------------------------------------
	// 3D Texture
	//--------------------------------------------------------------------------------------
	class Texture3D :
		public ResourceBase
	{
	public:
		Texture3D();
		virtual ~Texture3D();

		bool Create(const Device &device, uint32_t width, uint32_t height, uint32_t depth,
			Format format, ResourceFlags resourceFlags = ResourceFlags(0), uint8_t numMips = 1,
			PoolType poolType = PoolType(1), ResourceState state = ResourceState(0),
			const wchar_t *name = nullptr);

		void CreateSRVs(Format format = Format(0), uint8_t numMips = 1);
		void CreateSRVLevels(uint8_t numMips, Format format = Format(0));
		void CreateUAVs(Format format = Format(0), uint8_t numMips = 1);
		
		Descriptor GetUAV(uint8_t i = 0) const;
		Descriptor GetSRVLevel(uint8_t i) const;

	protected:
		Resource m_counter;
		std::vector<Descriptor>	m_UAVs;
		std::vector<Descriptor>	m_SRVLevels;
	};

	//--------------------------------------------------------------------------------------
	// Raw buffer
	//--------------------------------------------------------------------------------------
	class RawBuffer :
		public ResourceBase
	{
	public:
		RawBuffer();
		virtual ~RawBuffer();

		bool Create(const Device &device, uint32_t byteWidth, ResourceFlags resourceFlags = ResourceFlags(0),
			PoolType poolType = PoolType(1), ResourceState state = ResourceState(0),
			uint32_t numSRVs = 1, const uint32_t *firstSRVElements = nullptr,
			uint32_t numUAVs = 1, const uint32_t *firstUAVElements = nullptr,
			const wchar_t *name = nullptr);
		bool Upload(const GraphicsCommandList &commandList, Resource &resourceUpload,
			const void *pData, ResourceState dstState = ResourceState(0));

		void CreateSRVs(uint32_t byteWidth, const uint32_t *firstElements = nullptr,
			uint32_t numDescriptors = 1);
		void CreateUAVs(uint32_t byteWidth, const uint32_t *firstElements = nullptr,
			uint32_t numDescriptors = 1);

		Descriptor GetUAV(uint32_t i = 0) const;
		
		void *Map(uint32_t i = 0);
		void Unmap();

	protected:
		bool create(const Device &device, uint32_t byteWidth, ResourceFlags resourceFlags,
			PoolType poolType, ResourceState state, uint32_t numSRVs, uint32_t numUAVs,
			const wchar_t *name);

		Resource m_counter;
		std::vector<Descriptor>	m_UAVs;
		std::vector<uint32_t>	m_SRVOffsets;

		void *m_pDataBegin;
	};

	//--------------------------------------------------------------------------------------
	// Structured buffer
	//--------------------------------------------------------------------------------------
	class StructuredBuffer :
		public RawBuffer
	{
	public:
		StructuredBuffer();
		virtual ~StructuredBuffer();

		bool Create(const Device &device, uint32_t numElements, uint32_t stride,
			ResourceFlags resourceFlags = ResourceFlags(0), PoolType poolType = PoolType(1),
			ResourceState state = ResourceState(0),
			uint32_t numSRVs = 1, const uint32_t *firstSRVElements = nullptr,
			uint32_t numUAVs = 1, const uint32_t *firstUAVElements = nullptr,
			const wchar_t *name = nullptr);
		
		void CreateSRVs(uint32_t numElements, uint32_t stride,
			const uint32_t *firstElements = nullptr, uint32_t numDescriptors = 1);
		void CreateUAVs(uint32_t numElements, uint32_t stride,
			const uint32_t *firstElements = nullptr, uint32_t numDescriptors = 1);
	};

	//--------------------------------------------------------------------------------------
	// Typed buffer
	//--------------------------------------------------------------------------------------
	class TypedBuffer :
		public RawBuffer
	{
	public:
		TypedBuffer();
		virtual ~TypedBuffer();

		bool Create(const Device &device, uint32_t numElements, uint32_t stride, Format format,
			ResourceFlags resourceFlags = ResourceFlags(0), PoolType poolType = PoolType(1),
			ResourceState state = ResourceState(0),
			uint32_t numSRVs = 1, const uint32_t *firstSRVElements = nullptr,
			uint32_t numUAVs = 1, const uint32_t *firstUAVElements = nullptr,
			const wchar_t *name = nullptr);
		
		void CreateSRVs(uint32_t numElements, Format format, uint32_t stride,
			const uint32_t *firstElements = nullptr, uint32_t numDescriptors = 1);
		void CreateUAVs(uint32_t numElements, Format format, uint32_t stride,
			const uint32_t *firstElements = nullptr, uint32_t numDescriptors = 1);
	};

	//--------------------------------------------------------------------------------------
	// Vertex buffer
	//--------------------------------------------------------------------------------------
	class VertexBuffer :
		public StructuredBuffer
	{
	public:
		VertexBuffer();
		virtual ~VertexBuffer();

		bool Create(const Device &device, uint32_t numVertices, uint32_t stride,
			ResourceFlags resourceFlags = ResourceFlags(0), PoolType poolType = PoolType(1),
			ResourceState state = ResourceState(0),
			uint32_t numVBVs = 1, const uint32_t *firstVertices = nullptr,
			uint32_t numSRVs = 1, const uint32_t *firstSRVElements = nullptr,
			uint32_t numUAVs = 1, const uint32_t *firstUAVElements = nullptr,
			const wchar_t *name = nullptr);

		VertexBufferView GetVBV(uint32_t i = 0) const;

	protected:
		std::vector<VertexBufferView> m_VBVs;
	};

	//--------------------------------------------------------------------------------------
	// Index buffer
	//--------------------------------------------------------------------------------------
	class IndexBuffer :
		public TypedBuffer
	{
	public:
		IndexBuffer();
		virtual ~IndexBuffer();

		bool Create(const Device &device, uint32_t byteWidth, Format format = Format(42),
			ResourceFlags resourceFlags = ResourceFlags(0x8), PoolType poolType = PoolType(1),
			ResourceState state = ResourceState(0),
			uint32_t numIBVs = 1, const uint32_t *offsets = nullptr,
			uint32_t numSRVs = 1, const uint32_t *firstSRVElements = nullptr,
			uint32_t numUAVs = 1, const uint32_t *firstUAVElements = nullptr,
			const wchar_t *name = nullptr);

		IndexBufferView GetIBV(uint32_t i = 0) const;

	protected:
		std::vector<IndexBufferView> m_IBVs;
	};
}
