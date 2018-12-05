//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

#pragma once

#include "StepTimer.h"
#include "RayTracer.h"

using namespace DirectX;

// Note that while ComPtr is used to manage the lifetime of resources on the CPU,
// it has no understanding of the lifetime of resources on the GPU. Apps must account
// for the GPU lifetime of resources to avoid destroying objects that may still be
// referenced by the GPU.
// An example of this can be found in the class method: OnDestroy().
using Microsoft::WRL::ComPtr;

class RayTracedGGX : public DXFramework
{
public:
	RayTracedGGX(uint32_t width, uint32_t height, std::wstring name);

	virtual void OnInit();
	virtual void OnUpdate();
	virtual void OnRender();
	virtual void OnDestroy();

	virtual void OnKeyUp(uint8_t /*key*/);
	virtual void OnLButtonDown(float posX, float posY);
	virtual void OnLButtonUp(float posX, float posY);
	virtual void OnMouseMove(float posX, float posY);
	virtual void OnMouseWheel(float deltaZ, float posX, float posY);
	virtual void OnMouseLeave();

private:
	static const uint32_t FrameCount = RayTracer::FrameCount;

	XUSG::DescriptorTableCache		m_descriptorTableCache;

	// Pipeline objects.
	CD3DX12_VIEWPORT	m_viewport;
	CD3DX12_RECT		m_scissorRect;

	ComPtr<IDXGISwapChain3>			m_swapChain;
	ComPtr<ID3D12CommandAllocator>	m_commandAllocators[FrameCount];
	ComPtr<ID3D12CommandQueue>		m_commandQueue;

	bool m_isDxrSupported;

	XUSG::RayTracing::Device m_device;
	XUSG::Resource	m_renderTargets[FrameCount];
	XUSG::RayTracing::CommandList m_commandList;
	
	// App resources.
	std::unique_ptr<RayTracer> m_rayTracer;
	XUSG::DescriptorPool	m_rtvPool;
	XUSG::RenderTargetTable	m_rtvTables[FrameCount];
	XUSG::DepthStencil		m_depth;
	DirectX::XMFLOAT4X4		m_proj;
	DirectX::XMFLOAT4X4		m_view;
	DirectX::XMFLOAT3		m_focusPt;
	DirectX::XMFLOAT3		m_eyePt;

	// Synchronization objects.
	uint32_t m_frameIndex;
	HANDLE m_fenceEvent;
	ComPtr<ID3D12Fence> m_fence;
	uint64_t m_fenceValues[FrameCount];

	// Application state
	bool m_pausing;
	StepTimer m_timer;

	// User camera interactions
	bool m_tracking;
	DirectX::XMFLOAT2 m_mousePt;

	void LoadPipeline();
	void LoadAssets();
	void PopulateCommandList();
	void WaitForGpu();
	void MoveToNextFrame();
	double CalculateFrameStats(float *fTimeStep = nullptr);

	// Ray tracing
	void EnableDirectXRaytracing(IDXGIAdapter1 *adapter);
	void CreateRaytracingInterfaces();
	void CopyRaytracingOutputToBackbuffer();
};
