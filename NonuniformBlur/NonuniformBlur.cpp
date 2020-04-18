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

#include "NonUniformBlur.h"

using namespace std;
using namespace XUSG;

NonUniformBlur::NonUniformBlur(uint32_t width, uint32_t height, wstring name) :
	DXFramework(width, height, name),
	m_typedUAV(false),
	m_isAutoFocus(true),
	m_isAutoSigma(true),
	m_focus(0.0, 0.0),
	m_sigma(24.0),
	m_frameIndex(0),
	m_pipelineType(Filter::COMPUTE),
	m_showFPS(true),
	m_fileName(L"Sashimi.dds")
{
#if defined (_DEBUG)
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
	AllocConsole();
	FILE* stream;
	freopen_s(&stream, "CONOUT$", "w+t", stdout);
	freopen_s(&stream, "CONIN$", "r+t", stdin);
#endif
}

NonUniformBlur::~NonUniformBlur()
{
#if defined (_DEBUG)
	FreeConsole();
#endif
}

void NonUniformBlur::OnInit()
{
	vector<Resource> uploaders(0);
	LoadPipeline(uploaders);
	LoadAssets();
}

// Load the rendering pipeline dependencies.
void NonUniformBlur::LoadPipeline(vector<Resource>& uploaders)
{
	auto dxgiFactoryFlags = 0u;

#if defined(_DEBUG)
	// Enable the debug layer (requires the Graphics Tools "optional feature").
	// NOTE: Enabling the debug layer after device creation will invalidate the active device.
	{
		com_ptr<ID3D12Debug1> debugController;
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
		hr = D3D12CreateDevice(dxgiAdapter.get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device));
	}

	dxgiAdapter->GetDesc1(&dxgiAdapterDesc);
	if (dxgiAdapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
		m_title += dxgiAdapterDesc.VendorId == 0x1414 && dxgiAdapterDesc.DeviceId == 0x8c ? L" (WARP)" : L" (Software)";
	ThrowIfFailed(hr);

	D3D12_FEATURE_DATA_D3D12_OPTIONS featureData = {};
	hr = m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &featureData, sizeof(featureData));
	if (SUCCEEDED(hr))
	{
		// TypedUAVLoadAdditionalFormats contains a Boolean that tells you whether the feature is supported or not
		if (featureData.TypedUAVLoadAdditionalFormats)
		{
			// Can assume “all-or-nothing” subset is supported (e.g. R32G32B32A32_FLOAT)
			// Cannot assume other formats are supported, so we check:
			D3D12_FEATURE_DATA_FORMAT_SUPPORT formatSupport = { DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE };
			hr = m_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &formatSupport, sizeof(formatSupport));
			if (SUCCEEDED(hr) && (formatSupport.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD))
				m_typedUAV = true;
		}
	}

	// Create the command queue.
	N_RETURN(m_device->GetCommandQueue(m_commandQueue, CommandListType::DIRECT, CommandQueueFlag::NONE), ThrowIfFailed(E_FAIL));

	// This sample does not support fullscreen transitions.
	ThrowIfFailed(factory->MakeWindowAssociation(Win32Application::GetHwnd(), DXGI_MWA_NO_ALT_ENTER));

	// Create a command allocator for each frame.
	for (auto n = 0u; n < FrameCount; n++)
		N_RETURN(m_device->GetCommandAllocator(m_commandAllocators[n], CommandListType::DIRECT), ThrowIfFailed(E_FAIL));

	// Create the command list.
	m_commandList = CommandList::MakeUnique();
	const auto pCommandList = m_commandList.get();
	N_RETURN(m_device->GetCommandList(pCommandList, 0, CommandListType::DIRECT,
		m_commandAllocators[0], nullptr), ThrowIfFailed(E_FAIL));

	m_filter = make_unique<Filter>(m_device);
	if (!m_filter) ThrowIfFailed(E_FAIL);

	if (!m_filter->Init(pCommandList, uploaders, Format::B8G8R8A8_UNORM, m_fileName.c_str(), m_typedUAV))
		ThrowIfFailed(E_FAIL);
	
	m_filter->GetImageSize(m_width, m_height);

	// Resize window
	{
		RECT windowRect;
		GetWindowRect(Win32Application::GetHwnd(), &windowRect);
		windowRect.right = windowRect.left + m_width;
		windowRect.bottom = windowRect.top + m_height;

		AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, FALSE);
		SetWindowPos(Win32Application::GetHwnd(), HWND_TOP, windowRect.left, windowRect.top,
			windowRect.right - windowRect.left, windowRect.bottom - windowRect.top, 0);
	}

	// Describe and create the swap chain.
	DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
	swapChainDesc.BufferCount = FrameCount;
	swapChainDesc.Width = m_width;
	swapChainDesc.Height = m_height;
	swapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	swapChainDesc.SampleDesc.Count = 1;

	com_ptr<IDXGISwapChain1> swapChain;
	ThrowIfFailed(factory->CreateSwapChainForHwnd(
		m_commandQueue.get(),		// Swap chain needs the queue so that it can force a flush on it.
		Win32Application::GetHwnd(),
		&swapChainDesc,
		nullptr,
		nullptr,
		&swapChain
	));

	ThrowIfFailed(swapChain.As(&m_swapChain));
	m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

	// Create frame resources.
	// Create a RTV for each frame.
	for (auto n = 0u; n < FrameCount; n++)
	{
		m_renderTargets[n] = RenderTarget::MakeUnique();
		N_RETURN(m_renderTargets[n]->CreateFromSwapChain(m_device, m_swapChain, n), ThrowIfFailed(E_FAIL));
	}
}

// Load the sample assets.
void NonUniformBlur::LoadAssets()
{
	// Close the command list and execute it to begin the initial GPU setup.
	ThrowIfFailed(m_commandList->Close());
	m_commandQueue->SubmitCommandList(m_commandList.get());

	// Create synchronization objects and wait until assets have been uploaded to the GPU.
	{
		N_RETURN(m_device->GetFence(m_fence, m_fenceValues[m_frameIndex]++, FenceFlag::NONE), ThrowIfFailed(E_FAIL));

		// Create an event handle to use for frame synchronization.
		m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (m_fenceEvent == nullptr)
		{
			ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
		}

		// Wait for the command list to execute; we are reusing the same command 
		// list in our main loop but for now, we just want to wait for setup to 
		// complete before continuing.
		WaitForGpu();
	}
}

// Update frame-based values.
void NonUniformBlur::OnUpdate()
{
	// Timer
	static auto time = 0.0, pauseTime = 0.0;

	m_timer.Tick();
	float timeStep;
	const auto totalTime = CalculateFrameStats(&timeStep);
	pauseTime = m_isPaused ? totalTime - time : pauseTime;
	timeStep = m_isPaused ? 0.0f : timeStep;
	time = totalTime - pauseTime;

	const auto t = 1.6 * time;
	if (m_isAutoFocus)
	{
		const float r = static_cast<float>(sin(XM_PIDIV2 * t)) * 0.25f + 0.25f;
		m_focus.x = r * static_cast<float>(cos(XM_PI * t));
		m_focus.y = r * static_cast<float>(sin(XM_PI * t));
	}

	if (m_isAutoSigma) m_sigma = 32.0f * (-static_cast<float>(cos(t)) * 0.5f + 0.5f);
}

// Render the scene.
void NonUniformBlur::OnRender()
{
	// Record all the commands we need to render the scene into the command list.
	PopulateCommandList();

	// Execute the command list.
	m_commandQueue->SubmitCommandList(m_commandList.get());

	// Present the frame.
	ThrowIfFailed(m_swapChain->Present(0, 0));

	MoveToNextFrame();
}

void NonUniformBlur::OnDestroy()
{
	// Ensure that the GPU is no longer referencing resources that are about to be
	// cleaned up by the destructor.
	WaitForGpu();

	CloseHandle(m_fenceEvent);
}

// User hot-key interactions.
void NonUniformBlur::OnKeyUp(uint8_t key)
{
	switch (key)
	{
	case 0x20:	// case VK_SPACE:
		m_isPaused = !m_isPaused;
		break;
	case 0x70:	//case VK_F1:
		m_showFPS = !m_showFPS;
		break;
	case 'P':
		m_pipelineType = static_cast<Filter::PipelineType>((m_pipelineType + 1) % Filter::NUM_PIPE_TYPE);
		break;
	}
}

void NonUniformBlur::ParseCommandLineArgs(wchar_t* argv[], int argc)
{
	DXFramework::ParseCommandLineArgs(argv, argc);

	for (auto i = 1; i < argc; ++i)
	{
		if (_wcsnicmp(argv[i], L"-image", wcslen(argv[i])) == 0 ||
			_wcsnicmp(argv[i], L"/image", wcslen(argv[i])) == 0)
		{
			if (i + 1 < argc) m_fileName = argv[i + 1];
		}
		else if (_wcsnicmp(argv[i], L"-sigma", wcslen(argv[i])) == 0 ||
			_wcsnicmp(argv[i], L"/sigma", wcslen(argv[i])) == 0)
		{
			if (i + 1 < argc) m_sigma = stof(argv[i + 1]);
			m_isAutoSigma = false;
		}
		else if (_wcsnicmp(argv[i], L"-focus", wcslen(argv[i])) == 0 ||
				_wcsnicmp(argv[i], L"/focus", wcslen(argv[i])) == 0)
		{
			if (i + 1 < argc) m_focus.x = stof(argv[i + 1]);
			if (i + 2 < argc) m_focus.y = stof(argv[i + 2]);
			m_isAutoFocus = false;
		}
		else if (_wcsnicmp(argv[i], L"-uniform", wcslen(argv[i])) == 0 ||
			_wcsnicmp(argv[i], L"/uniform", wcslen(argv[i])) == 0)
		{
			reinterpret_cast<uint32_t&>(m_focus.x) = UINT32_MAX;
			m_isAutoFocus = false;
		}
	}
}

void NonUniformBlur::PopulateCommandList()
{
	// Command list allocators can only be reset when the associated 
	// command lists have finished execution on the GPU; apps should use 
	// fences to determine GPU execution progress.
	ThrowIfFailed(m_commandAllocators[m_frameIndex]->Reset());

	// However, when ExecuteCommandList() is called on a particular command 
	// list, that command list can then be reset at any time and must be before 
	// re-recording.
	const auto pCommandList = m_commandList.get();
	ThrowIfFailed(pCommandList->Reset(m_commandAllocators[m_frameIndex], nullptr));

	// Record commands.
	const auto dstState = ResourceState::PIXEL_SHADER_RESOURCE |
		ResourceState::NON_PIXEL_SHADER_RESOURCE | ResourceState::COPY_SOURCE;
	m_filter->Process(pCommandList, m_focus, m_sigma, m_pipelineType);	// V-cycle

	{
		auto& result = m_filter->GetResult();
		const TextureCopyLocation dst(m_renderTargets[m_frameIndex]->GetResource().get(), 0);
		const TextureCopyLocation src(result.GetResource().get(), 0);

		ResourceBarrier barriers[2];
		auto numBarriers = m_renderTargets[m_frameIndex]->SetBarrier(barriers, ResourceState::COPY_DEST);
		numBarriers = result.SetBarrier(barriers, dstState, numBarriers, 0);
		pCommandList->Barrier(numBarriers, barriers);

		pCommandList->CopyTextureRegion(dst, 0, 0, 0, src);

		// Indicate that the back buffer will now be used to present.
		numBarriers = m_renderTargets[m_frameIndex]->SetBarrier(barriers, ResourceState::PRESENT);
		pCommandList->Barrier(numBarriers, barriers);
	}

	ThrowIfFailed(pCommandList->Close());
}

// Wait for pending GPU work to complete.
void NonUniformBlur::WaitForGpu()
{
	// Schedule a Signal command in the queue.
	ThrowIfFailed(m_commandQueue->Signal(m_fence.get(), m_fenceValues[m_frameIndex]));

	// Wait until the fence has been processed.
	ThrowIfFailed(m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent));
	WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);

	// Increment the fence value for the current frame.
	m_fenceValues[m_frameIndex]++;
}

// Prepare to render the next frame.
void NonUniformBlur::MoveToNextFrame()
{
	// Schedule a Signal command in the queue.
	const auto currentFenceValue = m_fenceValues[m_frameIndex];
	ThrowIfFailed(m_commandQueue->Signal(m_fence.get(), currentFenceValue));

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

double NonUniformBlur::CalculateFrameStats(float* pTimeStep)
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
		windowText << L"    fps: ";
		if (m_showFPS) windowText << setprecision(2) << fixed << fps;
		else windowText << L"[F1]";

		windowText << L"    [P] ";
		switch (m_pipelineType)
		{
		case Filter::GRAPHICS:
			windowText << L"Pure graphics pipelines";
			break;
		case Filter::COMPUTE:
			windowText << L"Pure compute pipelines";
			break;
		default:
			windowText << L"Hybrid pipelines (mip-gen by compute and up-sampling by graphics)";
		}

		SetCustomWindowText(windowText.str().c_str());
	}

	if (pTimeStep)* pTimeStep = static_cast<float>(totalTime - previousTime);
	previousTime = totalTime;

	return totalTime;
}
