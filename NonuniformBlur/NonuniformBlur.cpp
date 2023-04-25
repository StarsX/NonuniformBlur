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
#include "stb_image_write.h"

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
	m_useEZ(true),
	m_showFPS(true),
	m_fileName(L"Assets/Sashimi.dds")
{
#if defined (_DEBUG)
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
	AllocConsole();
	FILE* stream;
	freopen_s(&stream, "CONIN$", "r+t", stdin);
	freopen_s(&stream, "CONOUT$", "w+t", stdout);
	freopen_s(&stream, "CONOUT$", "w+t", stderr);
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
	vector<Resource::uptr> uploaders(0);
	LoadPipeline(uploaders);
	LoadAssets();
}

// Load the rendering pipeline dependencies.
void NonUniformBlur::LoadPipeline(vector<Resource::uptr>& uploaders)
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

	com_ptr<IDXGIFactory5> factory;
	ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&factory)));

	DXGI_ADAPTER_DESC1 dxgiAdapterDesc;
	com_ptr<IDXGIAdapter1> dxgiAdapter;
	auto hr = DXGI_ERROR_UNSUPPORTED;
	for (auto i = 0u; hr == DXGI_ERROR_UNSUPPORTED; ++i)
	{
		dxgiAdapter = nullptr;
		ThrowIfFailed(factory->EnumAdapters1(i, &dxgiAdapter));

		m_device = Device::MakeUnique();
		hr = m_device->Create(dxgiAdapter.get(), D3D_FEATURE_LEVEL_11_0);
	}

	dxgiAdapter->GetDesc1(&dxgiAdapterDesc);
	if (dxgiAdapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
		m_title += dxgiAdapterDesc.VendorId == 0x1414 && dxgiAdapterDesc.DeviceId == 0x8c ? L" (WARP)" : L" (Software)";
	ThrowIfFailed(hr);

	D3D12_FEATURE_DATA_D3D12_OPTIONS featureData = {};
	const auto pDevice = static_cast<ID3D12Device*>(m_device->GetHandle());
	hr = pDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &featureData, sizeof(featureData));
	if (SUCCEEDED(hr))
	{
		// TypedUAVLoadAdditionalFormats contains a Boolean that tells you whether the feature is supported or not
		if (featureData.TypedUAVLoadAdditionalFormats)
		{
			// Can assume "all-or-nothing" subset is supported (e.g. R32G32B32A32_FLOAT)
			// Cannot assume other formats are supported, so we check:
			D3D12_FEATURE_DATA_FORMAT_SUPPORT formatSupport = { DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE };
			hr = pDevice->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &formatSupport, sizeof(formatSupport));
			if (SUCCEEDED(hr) && (formatSupport.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD))
				m_typedUAV = true;
		}
	}

	// Create the command queue.
	m_commandQueue = CommandQueue::MakeUnique();
	XUSG_N_RETURN(m_commandQueue->Create(m_device.get(), CommandListType::DIRECT, CommandQueueFlag::NONE,
		0, 0, L"CommandQueue"), ThrowIfFailed(E_FAIL));

	// This sample does not support fullscreen transitions.
	ThrowIfFailed(factory->MakeWindowAssociation(Win32Application::GetHwnd(), DXGI_MWA_NO_ALT_ENTER));

	// Create a command allocator for each frame.
	for (uint8_t n = 0; n < FrameCount; ++n)
	{
		m_commandAllocators[n] = CommandAllocator::MakeUnique();
		XUSG_N_RETURN(m_commandAllocators[n]->Create(m_device.get(), CommandListType::DIRECT,
			(L"CommandAllocator" + to_wstring(n)).c_str()), ThrowIfFailed(E_FAIL));
	}

	// Create the command list.
	m_commandList = CommandList::MakeUnique();
	const auto pCommandList = m_commandList.get();
	XUSG_N_RETURN(pCommandList->Create(m_device.get(), 0, CommandListType::DIRECT,
		m_commandAllocators[m_frameIndex].get(), nullptr), ThrowIfFailed(E_FAIL));

	// Create descriptor-table lib.
	m_descriptorTableLib = DescriptorTableLib::MakeShared(m_device.get(), L"DescriptorTableLib");

	m_filter = make_unique<Filter>();
	XUSG_N_RETURN(m_filter->Init(pCommandList, m_descriptorTableLib, uploaders,
		Format::B8G8R8A8_UNORM, m_fileName.c_str(), m_typedUAV), ThrowIfFailed(E_FAIL));
	
	//m_filter->GetImageSize(m_width, m_height);

	m_filterEZ = make_unique<FilterEZ>();
	XUSG_N_RETURN(m_filterEZ->Init(pCommandList, uploaders, Format::B8G8R8A8_UNORM,
		m_fileName.c_str(), m_typedUAV), ThrowIfFailed(E_FAIL));

	m_filterEZ->GetImageSize(m_width, m_height);

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
	m_swapChain = SwapChain::MakeUnique();
	XUSG_N_RETURN(m_swapChain->Create(factory.get(), Win32Application::GetHwnd(), m_commandQueue->GetHandle(),
		FrameCount, m_width, m_height, Format::B8G8R8A8_UNORM, SwapChainFlag::ALLOW_TEARING), ThrowIfFailed(E_FAIL));

	m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

	// Create frame resources.
	// Create a RTV for each frame.
	for (uint8_t n = 0; n < FrameCount; ++n)
	{
		m_renderTargets[n] = RenderTarget::MakeUnique();
		XUSG_N_RETURN(m_renderTargets[n]->CreateFromSwapChain(m_device.get(), m_swapChain.get(), n), ThrowIfFailed(E_FAIL));
	}
}

// Load the sample assets.
void NonUniformBlur::LoadAssets()
{
	// Close the command list and execute it to begin the initial GPU setup.
	XUSG_N_RETURN(m_commandList->Close(), ThrowIfFailed(E_FAIL));
	m_commandQueue->ExecuteCommandList(m_commandList.get());

	m_commandListEZ = EZ::CommandList::MakeUnique();
	XUSG_N_RETURN(m_commandListEZ->Create(m_commandList.get(), 1, 137),
		ThrowIfFailed(E_FAIL));

	// Create synchronization objects and wait until assets have been uploaded to the GPU.
	{
		if (!m_fence)
		{
			m_fence = Fence::MakeUnique();
			XUSG_N_RETURN(m_fence->Create(m_device.get(), m_fenceValues[m_frameIndex]++, FenceFlag::NONE, L"Fence"), ThrowIfFailed(E_FAIL));
		}

		// Create an event handle to use for frame synchronization.
		m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (!m_fenceEvent) ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));

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
	if (m_useEZ) m_filterEZ->UpdateFrame(m_focus, m_sigma, m_frameIndex);
	else m_filter->UpdateFrame(m_focus, m_sigma, m_frameIndex);
}

// Render the scene.
void NonUniformBlur::OnRender()
{
	// Record all the commands we need to render the scene into the command list.
	PopulateCommandList();

	// Execute the command list.
	m_commandQueue->ExecuteCommandList(m_commandList.get());

	// Present the frame.
	XUSG_N_RETURN(m_swapChain->Present(0, PresentFlag::ALLOW_TEARING), ThrowIfFailed(E_FAIL));

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
	case VK_SPACE:
		m_isPaused = !m_isPaused;
		break;
	case VK_F1:
		m_showFPS = !m_showFPS;
		break;
	case VK_F11:
		m_screenShot = 1;
		break;
	case VK_ESCAPE:
		PostQuitMessage(0);
		break;
	case 'P':
		m_pipelineType = static_cast<Filter::PipelineType>((m_pipelineType + 1) % Filter::NUM_PIPE_TYPE);
		break;
	case 'X':
		m_useEZ = !m_useEZ;
		break;
	}
}

void NonUniformBlur::ParseCommandLineArgs(wchar_t* argv[], int argc)
{
	DXFramework::ParseCommandLineArgs(argv, argc);

	for (auto i = 1; i < argc; ++i)
	{
		if (wcsncmp(argv[i], L"-image", wcslen(argv[i])) == 0 ||
			wcsncmp(argv[i], L"/image", wcslen(argv[i])) == 0)
		{
			if (i + 1 < argc) m_fileName = argv[++i];
		}
		else if (wcsncmp(argv[i], L"-sigma", wcslen(argv[i])) == 0 ||
			wcsncmp(argv[i], L"/sigma", wcslen(argv[i])) == 0)
		{
			if (i + 1 < argc) m_sigma = stof(argv[++i]);
			m_isAutoSigma = false;
		}
		else if (wcsncmp(argv[i], L"-focus", wcslen(argv[i])) == 0 ||
			wcsncmp(argv[i], L"/focus", wcslen(argv[i])) == 0)
		{
			if (i + 1 < argc) m_focus.x = stof(argv[++i]);
			if (i + 1 < argc) m_focus.y = stof(argv[++i]);
			m_isAutoFocus = false;
		}
		else if (wcsncmp(argv[i], L"-uniform", wcslen(argv[i])) == 0 ||
			wcsncmp(argv[i], L"/uniform", wcslen(argv[i])) == 0)
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
	const auto pCommandAllocator = m_commandAllocators[m_frameIndex].get();
	XUSG_N_RETURN(pCommandAllocator->Reset(), ThrowIfFailed(E_FAIL));

	const auto pRenderTarget = m_renderTargets[m_frameIndex].get();
	if (m_useEZ)
	{
		// However, when ExecuteCommandList() is called on a particular command 
		// list, that command list can then be reset at any time and must be before 
		// re-recording.
		const auto pCommandList = m_commandListEZ.get();
		XUSG_N_RETURN(pCommandList->Reset(pCommandAllocator, nullptr), ThrowIfFailed(E_FAIL));

		// Record commands.
		m_filterEZ->Process(pCommandList, m_frameIndex, static_cast<FilterEZ::PipelineType>(m_pipelineType));

		const auto pResult = m_filterEZ->GetResult();
		const TextureCopyLocation dst(pRenderTarget, 0);
		const TextureCopyLocation src(pResult, 0);

		pCommandList->CopyTextureRegion(dst, 0, 0, 0, src);

		// Screen-shot helper
		if (m_screenShot == 1)
		{
			if (!m_readBuffer) m_readBuffer = Buffer::MakeUnique();
			pRenderTarget->ReadBack(pCommandList->AsCommandList(), m_readBuffer.get());
			m_screenShot = 2;
		}

		XUSG_N_RETURN(pCommandList->Close(pRenderTarget), ThrowIfFailed(E_FAIL));
	}
	else
	{
		// However, when ExecuteCommandList() is called on a particular command 
		// list, that command list can then be reset at any time and must be before 
		// re-recording.
		const auto pCommandList = m_commandList.get();
		XUSG_N_RETURN(pCommandList->Reset(pCommandAllocator, nullptr), ThrowIfFailed(E_FAIL));

		// Record commands.
		// Set descriptor heaps
		const DescriptorHeap descriptorPools[] =
		{
			m_descriptorTableLib->GetDescriptorHeap(CBV_SRV_UAV_HEAP),
			m_descriptorTableLib->GetDescriptorHeap(SAMPLER_HEAP)
		};
		pCommandList->SetDescriptorHeaps(static_cast<uint32_t>(size(descriptorPools)), descriptorPools);

		const auto dstState = ResourceState::PIXEL_SHADER_RESOURCE |
			ResourceState::NON_PIXEL_SHADER_RESOURCE | ResourceState::COPY_SOURCE;
		m_filter->Process(pCommandList, m_frameIndex, m_pipelineType);	// V-cycle

		{
			const auto pResult = m_filter->GetResult();
			const TextureCopyLocation dst(pRenderTarget, 0);
			const TextureCopyLocation src(pResult, 0);

			ResourceBarrier barriers[2];
			auto numBarriers = pRenderTarget->SetBarrier(barriers, ResourceState::COPY_DEST);
			numBarriers = pResult->SetBarrier(barriers, dstState, numBarriers, 0);
			pCommandList->Barrier(numBarriers, barriers);

			pCommandList->CopyTextureRegion(dst, 0, 0, 0, src);

			// Indicate that the back buffer will now be used to present.
			numBarriers = pRenderTarget->SetBarrier(barriers, ResourceState::PRESENT);
			pCommandList->Barrier(numBarriers, barriers);
		}

		// Screen-shot helper
		if (m_screenShot == 1)
		{
			if (!m_readBuffer) m_readBuffer = Buffer::MakeUnique();
			pRenderTarget->ReadBack(pCommandList, m_readBuffer.get());
			m_screenShot = 2;
		}

		XUSG_N_RETURN(pCommandList->Close(), ThrowIfFailed(E_FAIL));
	}
}

// Wait for pending GPU work to complete.
void NonUniformBlur::WaitForGpu()
{
	// Schedule a Signal command in the queue.
	XUSG_N_RETURN(m_commandQueue->Signal(m_fence.get(), m_fenceValues[m_frameIndex]), ThrowIfFailed(E_FAIL));

	// Wait until the fence has been processed.
	XUSG_N_RETURN(m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent), ThrowIfFailed(E_FAIL));
	WaitForSingleObject(m_fenceEvent, INFINITE);

	// Increment the fence value for the current frame.
	m_fenceValues[m_frameIndex]++;
}

// Prepare to render the next frame.
void NonUniformBlur::MoveToNextFrame()
{
	// Schedule a Signal command in the queue.
	const auto currentFenceValue = m_fenceValues[m_frameIndex];
	XUSG_N_RETURN(m_commandQueue->Signal(m_fence.get(), currentFenceValue), ThrowIfFailed(E_FAIL));

	// Update the frame index.
	m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

	// If the next frame is not ready to be rendered yet, wait until it is ready.
	if (m_fence->GetCompletedValue() < m_fenceValues[m_frameIndex])
	{
		XUSG_N_RETURN(m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent), ThrowIfFailed(E_FAIL));
		WaitForSingleObject(m_fenceEvent, INFINITE);
	}

	// Set the fence value for the next frame.
	m_fenceValues[m_frameIndex] = currentFenceValue + 1;

	// Screen-shot helper
	if (m_screenShot)
	{
		if (m_screenShot > FrameCount)
		{
			char timeStr[15];
			tm dateTime;
			const auto now = time(nullptr);
			if (!localtime_s(&dateTime, &now) && strftime(timeStr, sizeof(timeStr), "%Y%m%d%H%M%S", &dateTime))
				SaveImage((string("RenderingX_") + timeStr + ".png").c_str(), m_readBuffer.get(), m_width, m_height);
			m_screenShot = 0;
		}
		else ++m_screenShot;
	}
}

void NonUniformBlur::SaveImage(char const* fileName, Buffer* imageBuffer, uint32_t w, uint32_t h, uint8_t comp)
{
	assert(comp == 3 || comp == 4);
	const auto pData = static_cast<uint8_t*>(imageBuffer->Map());

	//stbi_write_png_compression_level = 1024;
	vector<uint8_t> imageData(comp * w * h);
	for (auto i = 0u; i < w * h; ++i)
		for (uint8_t j = 0; j < comp; ++j)
			imageData[comp * i + j] = pData[4 * i + j];

	stbi_write_png(fileName, w, h, comp, imageData.data(), 0);

	m_readBuffer->Unmap();
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

		windowText << L"    [X] " << (m_useEZ ? "XUSG-EZ" : "XUSGCore");
		windowText << L"    [F11] screen shot";

		SetCustomWindowText(windowText.str().c_str());
	}

	if (pTimeStep)* pTimeStep = static_cast<float>(totalTime - previousTime);
	previousTime = totalTime;

	return totalTime;
}
