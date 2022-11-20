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

#include "DXFramework.h"
#include "StepTimer.h"
#include "Filter.h"
#include "FilterEZ.h"

using namespace DirectX;

// Note that while ComPtr is used to manage the lifetime of resources on the CPU,
// it has no understanding of the lifetime of resources on the GPU. Apps must account
// for the GPU lifetime of resources to avoid destroying objects that may still be
// referenced by the GPU.
// An example of this can be found in the class method: OnDestroy().

class NonUniformBlur : public DXFramework
{
public:
	NonUniformBlur(uint32_t width, uint32_t height, std::wstring name);
	virtual ~NonUniformBlur();

	virtual void OnInit();
	virtual void OnUpdate();
	virtual void OnRender();
	virtual void OnDestroy();

	virtual void OnKeyUp(uint8_t /*key*/);

	virtual void ParseCommandLineArgs(wchar_t* argv[], int argc);

private:
	static const uint8_t FrameCount = Filter::FrameCount;

	XUSG::DescriptorTableLib::sptr	m_descriptorTableLib;

	XUSG::SwapChain::uptr			m_swapChain;
	XUSG::CommandAllocator::uptr	m_commandAllocators[FrameCount];
	XUSG::CommandQueue::uptr		m_commandQueue;

	XUSG::Device::uptr				m_device;
	XUSG::RenderTarget::uptr		m_renderTargets[FrameCount];
	XUSG::CommandList::uptr			m_commandList;
	XUSG::EZ::CommandList::uptr		m_commandListEZ;

	// App resources.
	std::unique_ptr<Filter>		m_filter;
	std::unique_ptr<FilterEZ>	m_filterEZ;

	bool		m_typedUAV;

	// Animation
	bool		m_isAutoFocus;
	bool		m_isAutoSigma;
	XMFLOAT2	m_focus;
	float		m_sigma;

	// Synchronization objects.
	uint8_t		m_frameIndex;
	HANDLE		m_fenceEvent;
	XUSG::Fence::uptr m_fence;
	uint64_t	m_fenceValues[FrameCount];

	// Application state
	Filter::PipelineType m_pipelineType;
	bool		m_showFPS;
	bool		m_isPaused;
	StepTimer	m_timer;

	// User external settings
	std::wstring m_fileName;

	void LoadPipeline(std::vector<XUSG::Resource::uptr>& uploaders);
	void LoadAssets();

	void PopulateCommandList();
	void WaitForGpu();
	void MoveToNextFrame();
	double CalculateFrameStats(float* fTimeStep = nullptr);
};
