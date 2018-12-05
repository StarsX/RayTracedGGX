//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

#pragma once

#include "Core/XUSGType.h"

namespace XUSG
{
	using Microsoft::WRL::ComPtr;

	namespace RayTracing
	{
		enum class API
		{
			FallbackLayer,
			DirectXRaytracing,
		};

		struct Device
		{
			API RaytracingAPI;
			XUSG::Device Common;
			ComPtr<ID3D12RaytracingFallbackDevice> Fallback;
			ComPtr<ID3D12Device5> DXR;
		};

		struct CommandList
		{
			XUSG::GraphicsCommandList Common;
			ComPtr<ID3D12RaytracingFallbackCommandList> Fallback;
			ComPtr<ID3D12GraphicsCommandList4> DXR;
		};

		struct Pipeline
		{
			ComPtr<ID3D12RaytracingFallbackStateObject> Fallback;
			ComPtr<ID3D12StateObject> DXR;
		};

		using BuildFlags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS;
		using BuildDesc = D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC;
		using PrebuildInfo = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO;

		using Geometry = D3D12_RAYTRACING_GEOMETRY_DESC;
		using GeometryFlags = D3D12_RAYTRACING_GEOMETRY_FLAGS;

		using PipilineDesc = CD3D12_STATE_OBJECT_DESC;
	}
}
