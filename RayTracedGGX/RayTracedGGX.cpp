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

#include "RayTracedGGX.h"

using namespace std;
using namespace XUSG;
using namespace XUSG::RayTracing;

#define	PIDIV4	0.785398163f

static const float g_FOVAngleY = PIDIV4;
static const float g_zNear = 1.0f;
static const float g_zFar = 1000.0f;

RayTracedGGX::RayTracedGGX(uint32_t width, uint32_t height, std::wstring name) :
	DXFramework(width, height, name),
	m_isDxrSupported(false),
	m_frameIndex(0),
	m_viewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)),
	m_scissorRect(0, 0, static_cast<long>(width), static_cast<long>(height)),
	m_asyncCompute(true),
	m_useSharedMemVariance(false),
	m_isPaused(false),
	m_tracking(false),
	m_meshFileName("Media/dragon.obj"),
	m_envFileName(L"Media/rnl_cross.dds"),
	m_meshPosScale(0.0f, 0.0f, 0.0f, 1.0f)
{
#if defined (_DEBUG)
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
	AllocConsole();
	FILE* stream;
	freopen_s(&stream, "CONOUT$", "w+t", stdout);
	freopen_s(&stream, "CONIN$", "r+t", stdin);
#endif
}

RayTracedGGX::~RayTracedGGX()
{
#if defined (_DEBUG)
	FreeConsole();
#endif
}

void RayTracedGGX::OnInit()
{
	LoadPipeline();
	LoadAssets();
}

// Load the rendering pipeline dependencies.
void RayTracedGGX::LoadPipeline()
{
	auto dxgiFactoryFlags = 0u;

#if defined(_DEBUG)
	// Enable the debug layer (requires the Graphics Tools "optional feature").
	// NOTE: Enabling the debug layer after device creation will invalidate the active device.
	{
		ComPtr<ID3D12Debug1> debugController;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
		{
			debugController->EnableDebugLayer();
			//debugController->SetEnableGPUBasedValidation(TRUE);

			// Enable additional debug layers.
			dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
		}
	}
#endif

	com_ptr<IDXGIFactory4> factory;
	ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&factory)));

	DXGI_ADAPTER_DESC1 dxgiAdapterDesc;
	com_ptr<IDXGIAdapter1> dxgiAdapter;
	auto hr = DXGI_ERROR_UNSUPPORTED;
	for (auto i = 0u; hr == DXGI_ERROR_UNSUPPORTED; ++i)
	{
		dxgiAdapter = nullptr;
		ThrowIfFailed(factory->EnumAdapters1(i, &dxgiAdapter));
		EnableDirectXRaytracing(dxgiAdapter.get());
		hr = D3D12CreateDevice(dxgiAdapter.get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device.Base));
	}

	dxgiAdapter->GetDesc1(&dxgiAdapterDesc);
	if (dxgiAdapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
		m_title += dxgiAdapterDesc.VendorId == 0x1414 && dxgiAdapterDesc.DeviceId == 0x8c ? L" (WARP)" : L" (Software)";
	ThrowIfFailed(hr);

	// Create the command queue.
	N_RETURN(m_device.Base->GetCommandQueue(m_commandQueues[UNIVERSAL], CommandListType::DIRECT, CommandQueueFlag::NONE), ThrowIfFailed(E_FAIL));
	N_RETURN(m_device.Base->GetCommandQueue(m_commandQueues[COMPUTE], CommandListType::COMPUTE, CommandQueueFlag::NONE), ThrowIfFailed(E_FAIL));

	// Describe and create the swap chain.
	DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
	swapChainDesc.BufferCount = FrameCount;
	swapChainDesc.Width = m_width;
	swapChainDesc.Height = m_height;
	swapChainDesc.Format = GetDXGIFormat(Format::R8G8B8A8_UNORM);
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	swapChainDesc.SampleDesc.Count = 1;

	ComPtr<IDXGISwapChain1> swapChain;
	ThrowIfFailed(factory->CreateSwapChainForHwnd(
		m_commandQueues[UNIVERSAL].get(),		// Swap chain needs the queue so that it can force a flush on it.
		Win32Application::GetHwnd(),
		&swapChainDesc,
		nullptr,
		nullptr,
		&swapChain
	));

	// This sample does not support fullscreen transitions.
	ThrowIfFailed(factory->MakeWindowAssociation(Win32Application::GetHwnd(), DXGI_MWA_NO_ALT_ENTER));

	ThrowIfFailed(swapChain->QueryInterface(IID_PPV_ARGS(&m_swapChain)));
	m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

	// Create frame resources.
	// Create a RTV and a command allocator for each frame.
	for (uint8_t n = 0; n < FrameCount; ++n)
	{
		m_renderTargets[n] = RenderTarget::MakeUnique();
		N_RETURN(m_renderTargets[n]->CreateFromSwapChain(m_device, m_swapChain, n), ThrowIfFailed(E_FAIL));
		N_RETURN(m_device.Base->GetCommandAllocator(m_commandAllocators[ALLOCATOR_UPDATE_AS][n], CommandListType::COMPUTE), ThrowIfFailed(E_FAIL));
		N_RETURN(m_device.Base->GetCommandAllocator(m_commandAllocators[ALLOCATOR_GEOMETRY][n], CommandListType::DIRECT), ThrowIfFailed(E_FAIL));
		N_RETURN(m_device.Base->GetCommandAllocator(m_commandAllocators[ALLOCATOR_RAY_TRACE][n], CommandListType::DIRECT), ThrowIfFailed(E_FAIL));
		N_RETURN(m_device.Base->GetCommandAllocator(m_commandAllocators[ALLOCATOR_COMPUTE][n], CommandListType::COMPUTE), ThrowIfFailed(E_FAIL));
		N_RETURN(m_device.Base->GetCommandAllocator(m_commandAllocators[ALLOCATOR_IMAGE][n], CommandListType::DIRECT), ThrowIfFailed(E_FAIL));
		m_commandAllocators[ALLOCATOR_UPDATE_AS][n]->SetName((L"UpdateASAllocator" + to_wstring(n)).c_str());
		m_commandAllocators[ALLOCATOR_GEOMETRY][n]->SetName((L"GeometryAllocator" + to_wstring(n)).c_str());
		m_commandAllocators[ALLOCATOR_RAY_TRACE][n]->SetName((L"RayTracingAllocator" + to_wstring(n)).c_str());
		m_commandAllocators[ALLOCATOR_COMPUTE][n]->SetName((L"ComputeAllocator" + to_wstring(n)).c_str());
		m_commandAllocators[ALLOCATOR_IMAGE][n]->SetName((L"ImageAllocator" + to_wstring(n)).c_str());
	}
}

// Load the sample assets.
void RayTracedGGX::LoadAssets()
{
	// Create the command lists.
	m_commandLists[UNIVERSAL] = RayTracing::CommandList::MakeUnique();
	const auto pCommandList = m_commandLists[UNIVERSAL].get();
	N_RETURN(m_device.Base->GetCommandList(pCommandList, 0, CommandListType::DIRECT,
		m_commandAllocators[ALLOCATOR_GEOMETRY][m_frameIndex], nullptr), ThrowIfFailed(E_FAIL));

	{
		m_commandLists[COMPUTE] = RayTracing::CommandList::MakeUnique();
		const auto pCommandList = m_commandLists[COMPUTE].get();
		N_RETURN(m_device.Base->GetCommandList(pCommandList, 0, CommandListType::COMPUTE,
			m_commandAllocators[ALLOCATOR_COMPUTE][m_frameIndex], nullptr), ThrowIfFailed(E_FAIL));
		ThrowIfFailed(pCommandList->Close());
	}

	// Create ray tracing interfaces
	CreateRaytracingInterfaces();

	// Create ray tracer
	vector<Resource> uploaders(0);
	{
		m_rayTracer = make_unique<RayTracer>(m_device);
		if (!m_rayTracer) ThrowIfFailed(E_FAIL);

		Geometry geometries[RayTracer::NUM_MESH];
		if (!m_rayTracer->Init(pCommandList, m_width, m_height, uploaders, geometries, m_meshFileName.c_str(),
			m_envFileName.c_str(), Format::R8G8B8A8_UNORM, m_meshPosScale)) ThrowIfFailed(E_FAIL);
	}

	// Create denoiser
	{
		m_denoiser = make_unique<Denoiser>(m_device);
		if (!m_denoiser) ThrowIfFailed(E_FAIL);

		if (!m_denoiser->Init(pCommandList, m_width, m_height, uploaders, Format::R8G8B8A8_UNORM,
			m_rayTracer->GetRayTracingOutput(), m_rayTracer->GetGBuffers(), m_rayTracer->GetDepth()))
			ThrowIfFailed(E_FAIL);
	}

	// Close the command list and execute it to begin the initial GPU setup.
	ThrowIfFailed(pCommandList->Close());
	m_commandQueues[UNIVERSAL]->SubmitCommandList(pCommandList);

	// Create synchronization objects and wait until assets have been uploaded to the GPU.
	{
		if (!m_fence) N_RETURN(m_device.Base->GetFence(m_fence, m_fenceValues[m_frameIndex]++, FenceFlag::NONE), ThrowIfFailed(E_FAIL));

		// Create an event handle to use for frame synchronization.
		m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (!m_fenceEvent) ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));

		// Wait for the command list to execute; we are reusing the same command 
		// list in our main loop but for now, we just want to wait for setup to 
		// complete before continuing.
		WaitForGpu();
	}

	if (!m_semaphore) N_RETURN(m_device.Base->GetFence(m_semaphore, m_semaphoreValue, FenceFlag::NONE), ThrowIfFailed(E_FAIL));

	// Projection
	const auto aspectRatio = m_width / static_cast<float>(m_height);
	const auto proj = XMMatrixPerspectiveFovLH(g_FOVAngleY, aspectRatio, g_zNear, g_zFar);
	XMStoreFloat4x4(&m_proj, proj);

	// View initialization
	m_focusPt = XMFLOAT3(0.0f, 3.0f, 0.0f);
	m_eyePt = XMFLOAT3(10.0f, 10.0f, -24.0f);
	const auto focusPt = XMLoadFloat3(&m_focusPt);
	const auto eyePt = XMLoadFloat3(&m_eyePt);
	const auto view = XMMatrixLookAtLH(eyePt, focusPt, XMVectorSet(0.0f, 1.0f, 0.0f, 1.0f));
	XMStoreFloat4x4(&m_view, view);
}

// Update frame-based values.
void RayTracedGGX::OnUpdate()
{
	// Timer
	static auto time = 0.0, pauseTime = 0.0;

	m_timer.Tick();
	float timeStep;
	const auto totalTime = CalculateFrameStats(&timeStep);
	pauseTime = m_isPaused ? totalTime - time : pauseTime;
	time = totalTime - pauseTime;
	timeStep = m_isPaused ? 0.0f : timeStep;

	// View
	const auto eyePt = XMLoadFloat3(&m_eyePt);
	const auto view = XMLoadFloat4x4(&m_view);
	const auto proj = XMLoadFloat4x4(&m_proj);
	m_rayTracer->UpdateFrame(m_frameIndex, eyePt, view * proj, timeStep);
}

// Render the scene.
void RayTracedGGX::OnRender()
{
	if (m_asyncCompute)
	{
		// Record all the commands we need to render the scene into the command list.
		{
			const auto commandType = COMPUTE;
			const auto commandQueue = m_commandQueues[commandType].get();
			PopulateUpdateASCommandList(commandType);
			ThrowIfFailed(commandQueue->Wait(m_semaphore.get(), m_semaphoreValue++));
			commandQueue->SubmitCommandList(m_commandLists[commandType].get()); // Execute the command lists.
			ThrowIfFailed(commandQueue->Signal(m_semaphore.get(), m_semaphoreValue));
		}

		// Record all the commands we need to render the scene into the command list.
		{
			const auto commandType = UNIVERSAL;
			const auto commandQueue = m_commandQueues[commandType].get();
			PopulateGeometryCommandList(commandType);
			commandQueue->SubmitCommandList(m_commandLists[commandType].get()); // Execute the command lists.
		}

		// Record all the commands we need to render the scene into the command list.
		{
			const auto commandType = UNIVERSAL;
			const auto commandQueue = m_commandQueues[commandType].get();
			PopulateRayTraceCommandList(commandType);
			ThrowIfFailed(commandQueue->Wait(m_semaphore.get(), m_semaphoreValue++));
			commandQueue->SubmitCommandList(m_commandLists[commandType].get()); // Execute the command lists.
			ThrowIfFailed(commandQueue->Signal(m_semaphore.get(), m_semaphoreValue));
		}

		// Record all the commands we need to render the scene into the command list.
		{
			const auto commandType = UNIVERSAL;
			const auto commandQueue = m_commandQueues[commandType].get();
			PopulateDenoiseCommandList(commandType);
			commandQueue->SubmitCommandList(m_commandLists[commandType].get()); // Execute the command lists.
		}
	}
	else
	{
		// Record all the commands we need to render the scene into the command list.
		PopulateCommandList();

		// Execute the command list.
		m_commandQueues[UNIVERSAL]->SubmitCommandList(m_commandLists[UNIVERSAL].get());
	}

	// Present the frame.
	ThrowIfFailed(m_swapChain->Present(0, 0));

	MoveToNextFrame();
}

void RayTracedGGX::OnDestroy()
{
	// Ensure that the GPU is no longer referencing resources that are about to be
	// cleaned up by the destructor.
	WaitForGpu();

	CloseHandle(m_fenceEvent);
}

// User hot-key interactions.
void RayTracedGGX::OnKeyUp(uint8_t key)
{
	switch (key)
	{
	case VK_SPACE:
		m_isPaused = !m_isPaused;
		break;
	case 'V':
		m_useSharedMemVariance = !m_useSharedMemVariance;
		break;
	case 'A':
		m_asyncCompute = !m_asyncCompute;
		break;
	}
}

// User camera interactions.
void RayTracedGGX::OnLButtonDown(float posX, float posY)
{
	m_tracking = true;
	m_mousePt = XMFLOAT2(posX, posY);
}

void RayTracedGGX::OnLButtonUp(float posX, float posY)
{
	m_tracking = false;
}

void RayTracedGGX::OnMouseMove(float posX, float posY)
{
	if (m_tracking)
	{
		const auto dPos = XMFLOAT2(m_mousePt.x - posX, m_mousePt.y - posY);

		XMFLOAT2 radians;
		radians.x = XM_2PI * dPos.y / m_height;
		radians.y = XM_2PI * dPos.x / m_width;

		const auto focusPt = XMLoadFloat3(&m_focusPt);
		auto eyePt = XMLoadFloat3(&m_eyePt);

		const auto len = XMVectorGetX(XMVector3Length(focusPt - eyePt));
		auto transform = XMMatrixTranslation(0.0f, 0.0f, -len);
		transform *= XMMatrixRotationRollPitchYaw(radians.x, radians.y, 0.0f);
		transform *= XMMatrixTranslation(0.0f, 0.0f, len);

		const auto view = XMLoadFloat4x4(&m_view) * transform;
		const auto viewInv = XMMatrixInverse(nullptr, view);
		eyePt = viewInv.r[3];

		XMStoreFloat3(&m_eyePt, eyePt);
		XMStoreFloat4x4(&m_view, view);

		m_mousePt = XMFLOAT2(posX, posY);
	}
}

void RayTracedGGX::OnMouseWheel(float deltaZ, float posX, float posY)
{
	const auto focusPt = XMLoadFloat3(&m_focusPt);
	auto eyePt = XMLoadFloat3(&m_eyePt);

	const auto len = XMVectorGetX(XMVector3Length(focusPt - eyePt));
	const auto transform = XMMatrixTranslation(0.0f, 0.0f, -len * deltaZ / 16.0f);

	const auto view = XMLoadFloat4x4(&m_view) * transform;
	const auto viewInv = XMMatrixInverse(nullptr, view);
	eyePt = viewInv.r[3];

	XMStoreFloat3(&m_eyePt, eyePt);
	XMStoreFloat4x4(&m_view, view);
}

void RayTracedGGX::OnMouseLeave()
{
	m_tracking = false;
}

void RayTracedGGX::ParseCommandLineArgs(wchar_t* argv[], int argc)
{
	DXFramework::ParseCommandLineArgs(argv, argc);

	for (auto i = 1; i < argc; ++i)
	{
		if (_wcsnicmp(argv[i], L"-mesh", wcslen(argv[i])) == 0 ||
			_wcsnicmp(argv[i], L"/mesh", wcslen(argv[i])) == 0)
		{
			if (i + 1 < argc)
			{
				m_meshFileName.resize(wcslen(argv[i + 1]));
				for (size_t j = 0; j < m_meshFileName.size(); ++j)
					m_meshFileName[j] = static_cast<char>(argv[i + 1][j]);
			}
			m_meshPosScale.x = i + 2 < argc ? static_cast<float>(_wtof(argv[i + 2])) : m_meshPosScale.x;
			m_meshPosScale.y = i + 3 < argc ? static_cast<float>(_wtof(argv[i + 3])) : m_meshPosScale.y;
			m_meshPosScale.z = i + 4 < argc ? static_cast<float>(_wtof(argv[i + 4])) : m_meshPosScale.z;
			m_meshPosScale.w = i + 5 < argc ? static_cast<float>(_wtof(argv[i + 5])) : m_meshPosScale.w;
			break;
		}
		else if (_wcsnicmp(argv[i], L"-env", wcslen(argv[i])) == 0 ||
			_wcsnicmp(argv[i], L"/env", wcslen(argv[i])) == 0)
			if (i + 1 < argc) m_envFileName = argv[i + 1];
	}
}

void RayTracedGGX::PopulateCommandList()
{
	// Command list allocators can only be reset when the associated 
	// command lists have finished execution on the GPU; apps should use 
	// fences to determine GPU execution progress.
	const auto commandAllocator = m_commandAllocators[ALLOCATOR_RAY_TRACE][m_frameIndex].get();
	ThrowIfFailed(commandAllocator->Reset());

	// However, when ExecuteCommandList() is called on a particular command 
	// list, that command list can then be reset at any time and must be before 
	// re-recording.
	const auto pCommandList = m_commandLists[UNIVERSAL].get();
	ThrowIfFailed(pCommandList->Reset(commandAllocator, nullptr));

	// Record commands.
	m_rayTracer->UpdateAccelerationStructures(pCommandList, m_frameIndex);
	m_rayTracer->Render(pCommandList, m_frameIndex);
	m_denoiser->Denoise(pCommandList, m_useSharedMemVariance);

	ResourceBarrier barriers[2];
	auto numBarriers = m_renderTargets[m_frameIndex]->SetBarrier(barriers, ResourceState::RENDER_TARGET);
	m_denoiser->ToneMap(pCommandList, m_renderTargets[m_frameIndex]->GetRTV(), numBarriers, barriers);

	// Indicate that the back buffer will now be used to present.
	numBarriers = m_renderTargets[m_frameIndex]->SetBarrier(barriers, ResourceState::PRESENT);
	pCommandList->Barrier(numBarriers, barriers);

	ThrowIfFailed(pCommandList->Close());
}

void RayTracedGGX::PopulateUpdateASCommandList(CommandType commandType)
{
	// Command list allocators can only be reset when the associated 
	// command lists have finished execution on the GPU; apps should use 
	// fences to determine GPU execution progress.
	const auto commandAllocator = m_commandAllocators[ALLOCATOR_UPDATE_AS][m_frameIndex].get();
	ThrowIfFailed(commandAllocator->Reset());

	// However, when ExecuteCommandList() is called on a particular command 
	// list, that command list can then be reset at any time and must be before 
	// re-recording.
	const auto pCommandList = m_commandLists[commandType].get();
	ThrowIfFailed(pCommandList->Reset(commandAllocator, nullptr));

	// Record commands.
	m_rayTracer->UpdateAccelerationStructures(pCommandList, m_frameIndex);

	ThrowIfFailed(pCommandList->Close());
}

void RayTracedGGX::PopulateGeometryCommandList(CommandType commandType)
{
	// Command list allocators can only be reset when the associated 
	// command lists have finished execution on the GPU; apps should use 
	// fences to determine GPU execution progress.
	const auto commandAllocator = m_commandAllocators[ALLOCATOR_GEOMETRY][m_frameIndex].get();
	ThrowIfFailed(commandAllocator->Reset());

	// However, when ExecuteCommandList() is called on a particular command 
	// list, that command list can then be reset at any time and must be before 
	// re-recording.
	const auto pCommandList = m_commandLists[commandType].get();
	ThrowIfFailed(pCommandList->Reset(commandAllocator, nullptr));

	// Record commands.
	m_rayTracer->RenderGeometry(pCommandList, m_frameIndex);

	ThrowIfFailed(pCommandList->Close());
}

void RayTracedGGX::PopulateRayTraceCommandList(CommandType commandType)
{
	// Command list allocators can only be reset when the associated 
	// command lists have finished execution on the GPU; apps should use 
	// fences to determine GPU execution progress.
	const auto commandAllocator = m_commandAllocators[ALLOCATOR_RAY_TRACE][m_frameIndex].get();
	ThrowIfFailed(commandAllocator->Reset());

	// However, when ExecuteCommandList() is called on a particular command 
	// list, that command list can then be reset at any time and must be before 
	// re-recording.
	const auto pCommandList = m_commandLists[commandType].get();
	ThrowIfFailed(pCommandList->Reset(commandAllocator, nullptr));

	// Record commands.
	m_rayTracer->RayTrace(pCommandList, m_frameIndex);

	ThrowIfFailed(pCommandList->Close());
}

void RayTracedGGX::PopulateDenoiseCommandList(CommandType commandType)
{
	// Command list allocators can only be reset when the associated 
	// command lists have finished execution on the GPU; apps should use 
	// fences to determine GPU execution progress.
	const auto commandAllocator = m_commandAllocators[ALLOCATOR_IMAGE][m_frameIndex].get();
	ThrowIfFailed(commandAllocator->Reset());

	// However, when ExecuteCommandList() is called on a particular command 
	// list, that command list can then be reset at any time and must be before 
	// re-recording.
	const auto pCommandList = m_commandLists[commandType].get();
	ThrowIfFailed(pCommandList->Reset(commandAllocator, nullptr));

	// Record commands.
	m_denoiser->Denoise(pCommandList, m_useSharedMemVariance);

	ResourceBarrier barriers[2];
	auto numBarriers = m_renderTargets[m_frameIndex]->SetBarrier(barriers, ResourceState::RENDER_TARGET);
	m_denoiser->ToneMap(pCommandList, m_renderTargets[m_frameIndex]->GetRTV(), numBarriers, barriers);

	// Indicate that the back buffer will now be used to present.
	numBarriers = m_renderTargets[m_frameIndex]->SetBarrier(barriers, ResourceState::PRESENT);
	pCommandList->Barrier(numBarriers, barriers);

	ThrowIfFailed(pCommandList->Close());
}

// Wait for pending GPU work to complete.
void RayTracedGGX::WaitForGpu()
{
	// Schedule a Signal command in the queue.
	ThrowIfFailed(m_commandQueues[UNIVERSAL]->Signal(m_fence.get(), m_fenceValues[m_frameIndex]));

	// Wait until the fence has been processed, and increment the fence value for the current frame.
	ThrowIfFailed(m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex]++, m_fenceEvent));
	WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
}

// Prepare to render the next frame.
void RayTracedGGX::MoveToNextFrame()
{
	// Schedule a Signal command in the queue.
	const auto currentFenceValue = m_fenceValues[m_frameIndex];
	ThrowIfFailed(m_commandQueues[UNIVERSAL]->Signal(m_fence.get(), currentFenceValue));

	// Update the frame index.
	m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

	// If the next frame is not ready to be rendered yet, wait until it is ready.
	if (m_fence->GetCompletedValue() < m_fenceValues[m_frameIndex])
	{
		ThrowIfFailed(m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent));
		WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
	}

	// Set the fence value for the next frame.
	m_fenceValues[m_frameIndex] = currentFenceValue + 1;
}

double RayTracedGGX::CalculateFrameStats(float* pTimeStep)
{
	static int frameCnt = 0;
	static double elapsedTime = 0.0;
	static double previousTime = 0.0;
	const auto totalTime = m_timer.GetTotalSeconds();
	++frameCnt;

	const auto timeStep = static_cast<float>(totalTime - elapsedTime);

	// Compute averages over one second period.
	if ((totalTime - elapsedTime) >= 1.0f)
	{
		float fps = static_cast<float>(frameCnt) / timeStep;	// Normalize to an exact second.

		frameCnt = 0;
		elapsedTime = totalTime;

		wstringstream windowText;
		windowText << setprecision(2) << fixed << L"    fps: " << fps;
		windowText << L"    [V] " << (m_useSharedMemVariance ? L"Shared memory" : L"Direct access");
		windowText << L"    [A] " << (m_asyncCompute ? L"Async compute" : L"Single command list");
		SetCustomWindowText(windowText.str().c_str());
	}

	if (pTimeStep)* pTimeStep = static_cast<float>(totalTime - previousTime);
	previousTime = totalTime;

	return totalTime;
}

//--------------------------------------------------------------------------------------
// Ray tracing
//--------------------------------------------------------------------------------------

// Enable experimental features required for compute-based raytracing fallback.
// This will set active D3D12 devices to DEVICE_REMOVED state.
// Returns bool whether the call succeeded and the device supports the feature.
inline bool EnableComputeRaytracingFallback(IDXGIAdapter1* adapter)
{
	ComPtr<ID3D12Device> testDevice;
	UUID experimentalFeatures[] = { D3D12ExperimentalShaderModels };

	return SUCCEEDED(D3D12EnableExperimentalFeatures(1, experimentalFeatures, nullptr, nullptr))
		&& SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&testDevice)));
}

// Returns bool whether the device supports DirectX Raytracing tier.
inline bool IsDirectXRaytracingSupported(IDXGIAdapter1* adapter)
{
	ComPtr<ID3D12Device> testDevice;
	D3D12_FEATURE_DATA_D3D12_OPTIONS5 featureSupportData = {};

	return SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&testDevice)))
		&& SUCCEEDED(testDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &featureSupportData, sizeof(featureSupportData)))
		&& featureSupportData.RaytracingTier != D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
}

void RayTracedGGX::EnableDirectXRaytracing(IDXGIAdapter1* adapter)
{
	// Fallback Layer uses an experimental feature and needs to be enabled before creating a D3D12 device.
	bool isFallbackSupported = EnableComputeRaytracingFallback(adapter);

	if (!isFallbackSupported)
	{
		OutputDebugString(
			L"Warning: Could not enable Compute Raytracing Fallback (D3D12EnableExperimentalFeatures() failed).\n" \
			L"         Possible reasons: your OS is not in developer mode.\n\n");
	}

	m_isDxrSupported = IsDirectXRaytracingSupported(adapter);

	if (!m_isDxrSupported)
	{
		OutputDebugString(L"Warning: DirectX Raytracing is not supported by your GPU and driver.\n\n");

		if (!isFallbackSupported)
			OutputDebugString(L"Could not enable compute based fallback raytracing support (D3D12EnableExperimentalFeatures() failed).\n"\
				L"Possible reasons: your OS is not in developer mode.\n\n");
		ThrowIfFailed(isFallbackSupported ? S_OK : E_FAIL);
	}
}

void RayTracedGGX::CreateRaytracingInterfaces()
{
	const auto createDeviceFlags = EnableRootDescriptorsInShaderRecords;
	ThrowIfFailed(D3D12CreateRaytracingFallbackDevice(m_device.Base.get(), createDeviceFlags, 0, IID_PPV_ARGS(&m_device.Derived)));
	for (auto& commandList : m_commandLists)
		N_RETURN(commandList->CreateInterface(m_device), ThrowIfFailed(E_FAIL));
}
