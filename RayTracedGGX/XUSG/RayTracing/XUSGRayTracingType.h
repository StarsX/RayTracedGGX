//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

#pragma once

#include "Core/XUSGType.h"
#include "Core/XUSGCommand.h"

namespace XUSG
{
	namespace RayTracing
	{
		enum class API
		{
			FallbackLayer,
			NativeRaytracing,
		};

		struct Device
		{
			API RaytracingAPI;
			XUSG::Device Common;
			com_ptr<ID3D12RaytracingFallbackDevice> Fallback;
			com_ptr<ID3D12Device5> Native;
		};

		struct Pipeline
		{
			com_ptr<ID3D12RaytracingFallbackStateObject> Fallback;
			com_ptr<ID3D12StateObject> Native;
		};

		using FallbackCommandList = com_ptr<ID3D12RaytracingFallbackCommandList>;
		using NativeCommandList = com_ptr<ID3D12GraphicsCommandList4>;

		using BuildFlags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS;
		using BuildDesc = D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC;
		using PrebuildInfo = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO;
		using PostbuildInfo = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC;

		using Geometry = D3D12_RAYTRACING_GEOMETRY_DESC;
		using GeometryFlags = D3D12_RAYTRACING_GEOMETRY_FLAGS;

		using PipilineDesc = CD3D12_STATE_OBJECT_DESC;

		class CommandList;
	}
}
