//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "FilterEZ.h"
#define _INDEPENDENT_DDS_LOADER_
#include "Advanced/XUSGDDSLoader.h"
#undef _INDEPENDENT_DDS_LOADER_

using namespace std;
using namespace DirectX;
using namespace XUSG;

struct CBGaussian
{
	XMFLOAT2	Focus;
	float		Sigma;
};

FilterEZ::FilterEZ() :
	m_imageSize(1, 1)
{
	m_shaderLib = ShaderLib::MakeUnique();
}

FilterEZ::~FilterEZ()
{
}

bool FilterEZ::Init(CommandList* pCommandList, vector<Resource::uptr>& uploaders,
	XUSG::Format rtFormat, const wchar_t* fileName, bool typedUAV)
{
	const auto pDevice = pCommandList->GetDevice();
	m_typedUAV = typedUAV;

	// Load input image
	{
		DDS::Loader textureLoader;
		DDS::AlphaMode alphaMode;

		uploaders.emplace_back(Resource::MakeUnique());
		XUSG_N_RETURN(textureLoader.CreateTextureFromFile(pCommandList, fileName,
			8192, false, m_source, uploaders.back().get(), &alphaMode), false);
	}

	// Create resources and pipelines
	m_imageSize.x = static_cast<uint32_t>(m_source->GetWidth());
	m_imageSize.y = m_source->GetHeight();

	const auto numUavFormats = typedUAV ? 0u : 1u;
	const auto uavFormat = Format::R32_UINT;
	m_filtered = RenderTarget::MakeUnique();
	m_filtered->Create(pDevice, m_imageSize.x, m_imageSize.y, rtFormat, 1, ResourceFlag::ALLOW_UNORDERED_ACCESS,
		0, 1, nullptr, false, MemoryFlag::NONE, L"FilteredImage", XUSG_DEFAULT_SRV_COMPONENT_MAPPING,
		TextureLayout::UNKNOWN, numUavFormats, typedUAV ? nullptr : &uavFormat);

	m_cbPerFrame = ConstantBuffer::MakeUnique();
	XUSG_N_RETURN(m_cbPerFrame->Create(pDevice, sizeof(CBGaussian[FrameCount]),
		FrameCount, nullptr, MemoryType::UPLOAD, MemoryFlag::NONE, L"CBPerFrame"), false);

	const uint8_t numPasses = m_filtered->GetNumMips() - 1;
	m_cbPerPass = ConstantBuffer::MakeUnique();
	XUSG_N_RETURN(m_cbPerPass->Create(pDevice, sizeof(uint32_t) * numPasses,
		numPasses, nullptr, MemoryType::UPLOAD, MemoryFlag::NONE, L"CBPerPass"), false);
	for (uint8_t i = 0; i < numPasses; ++i)
		*static_cast<uint32_t*>(m_cbPerPass->Map(i)) = numPasses - (i + 1);

	return createShaders();
}

void FilterEZ::UpdateFrame(DirectX::XMFLOAT2 focus, float sigma, uint8_t frameIndex)
{
	// Update per-frame CB
	{
		const auto pCbData = reinterpret_cast<CBGaussian*>(m_cbPerFrame->Map(frameIndex));
		pCbData->Focus = focus;
		pCbData->Sigma = sigma;
	}
}

void FilterEZ::Process(EZ::CommandList* pCommandList, uint8_t frameIndex, PipelineType pipelineType)
{
	switch (pipelineType)
	{
	case GRAPHICS:
		generateMipsGraphics(pCommandList);
		upsampleGraphics(pCommandList, frameIndex);
		break;
	case COMPUTE:
		generateMipsCompute(pCommandList);
		upsampleCompute(pCommandList, frameIndex);
		break;
	default:
		generateMipsCompute(pCommandList);
		upsampleGraphics(pCommandList, frameIndex);
	}
}

Resource* FilterEZ::GetResult() const
{
	return m_filtered.get();
}

void FilterEZ::GetImageSize(uint32_t& width, uint32_t& height) const
{
	width = m_imageSize.x;
	height = m_imageSize.y;
}

bool FilterEZ::createShaders()
{
	auto vsIndex = 0u;
	auto psIndex = 0u;
	auto csIndex = 0u;

	XUSG_N_RETURN(m_shaderLib->CreateShader(Shader::Stage::VS, vsIndex, L"VSScreenQuad.cso"), false);
	m_shaders[VS_SCREEN_QUAD] = m_shaderLib->GetShader(Shader::Stage::VS, vsIndex++);

	XUSG_N_RETURN(m_shaderLib->CreateShader(Shader::Stage::PS, psIndex, L"PSBlit2D.cso"), false);
	m_shaders[PS_BLIT_2D] = m_shaderLib->GetShader(Shader::Stage::PS, psIndex++);

	XUSG_N_RETURN(m_shaderLib->CreateShader(Shader::Stage::PS, psIndex, L"PSUpSample.cso"), false);
	m_shaders[PS_UP_SAMPLE] = m_shaderLib->GetShader(Shader::Stage::PS, psIndex++);

	XUSG_N_RETURN(m_shaderLib->CreateShader(Shader::Stage::PS, psIndex, L"PSUpSample_blend.cso"), false);
	m_shaders[PS_UP_SAMPLE_BLEND] = m_shaderLib->GetShader(Shader::Stage::PS, psIndex++);

	XUSG_N_RETURN(m_shaderLib->CreateShader(Shader::Stage::CS, csIndex, L"CSBlit2D.cso"), false);
	m_shaders[CS_BLIT_2D] = m_shaderLib->GetShader(Shader::Stage::CS, csIndex++);

	XUSG_N_RETURN(m_shaderLib->CreateShader(Shader::Stage::CS, csIndex, L"CSUpSample.cso"), false);
	m_shaders[CS_UP_SAMPLE] = m_shaderLib->GetShader(Shader::Stage::CS, csIndex++);

	XUSG_N_RETURN(m_shaderLib->CreateShader(Shader::Stage::CS, csIndex, m_typedUAV ? L"CSUpSample_in_place.cso" : L"CSUpSample_typeless.cso"), false);
	m_shaders[CS_UP_SAMPLE_INPLACE] = m_shaderLib->GetShader(Shader::Stage::CS, csIndex++);

	return true;
}

void FilterEZ::generateMipsGraphics(EZ::CommandList* pCommandList)
{
	// Generate mipmaps
	// Set pipeline state
	pCommandList->SetGraphicsShader(Shader::Stage::VS, m_shaders[VS_SCREEN_QUAD]);
	pCommandList->SetGraphicsShader(Shader::Stage::PS, m_shaders[PS_BLIT_2D]);
	pCommandList->DSSetState(Graphics::DEPTH_STENCIL_NONE);
	pCommandList->OMSetBlendState(Graphics::DEFAULT_OPAQUE);

	// Set IA
	pCommandList->IASetPrimitiveTopology(PrimitiveTopology::TRIANGLELIST);

	// Set sampler
	const auto sampler = LINEAR_CLAMP;
	pCommandList->SetSamplerStates(Shader::Stage::PS, 0, 1, &sampler);

	const uint32_t width = static_cast<uint32_t>(m_filtered->GetWidth());
	const uint32_t height = m_filtered->GetHeight();
	const uint8_t numMips = m_filtered->GetNumMips();
	for (uint8_t i = 1; i < numMips; ++i)
	{
		// Set render target
		const auto rtv = EZ::GetRTV(m_filtered.get(), 0, i);
		pCommandList->OMSetRenderTargets(1, &rtv);

		// Set SRV
		const auto srv = i > 1 ? EZ::GetSRV(m_filtered.get(), i - 1, true) : EZ::GetSRV(m_source.get());
		pCommandList->SetResources(Shader::Stage::PS, DescriptorType::SRV, 0, 1, &srv);

		// Set viewport
		const auto viewportX = width >> i;
		const auto viewportY = height >> i;
		Viewport viewport(0.0f, 0.0f, static_cast<float>(viewportX), static_cast<float>(viewportY));
		RectRange scissorRect(0, 0, viewportX, viewportY);
		pCommandList->RSSetViewports(1, &viewport);
		pCommandList->RSSetScissorRects(1, &scissorRect);

		// Draw
		pCommandList->Draw(3, 1, 0, 0);
	}
}

void FilterEZ::generateMipsCompute(EZ::CommandList* pCommandList)
{
	// Generate mipmaps
	// Set pipeline state
	pCommandList->SetComputeShader(m_shaders[CS_BLIT_2D]);

	// Set sampler
	const auto sampler = LINEAR_CLAMP;
	pCommandList->SetSamplerStates(Shader::Stage::CS, 0, 1, &sampler);

	const uint32_t width = static_cast<uint32_t>(m_filtered->GetWidth());
	const uint32_t height = m_filtered->GetHeight();
	const uint8_t numMips = m_filtered->GetNumMips();
	for (uint8_t i = 1; i < numMips; ++i)
	{
		// Set UAV
		const auto uav = EZ::GetUAV(m_filtered.get(), i);
		pCommandList->SetResources(Shader::Stage::CS, DescriptorType::UAV, 0, 1, &uav);

		// Set SRV
		const auto srv = i > 1 ? EZ::GetSRV(m_filtered.get(), i - 1, true) : EZ::GetSRV(m_source.get());
		pCommandList->SetResources(Shader::Stage::CS, DescriptorType::SRV, 0, 1, &srv);

		// Dispatch grid
		const auto threadsX = width >> i;
		const auto threadsY = height >> i;
		pCommandList->Dispatch(XUSG_DIV_UP(threadsX, 8), XUSG_DIV_UP(threadsY, 8), 1);
	}
}

void FilterEZ::upsampleGraphics(EZ::CommandList* pCommandList, uint8_t frameIndex)
{
	// Set pipeline state
	pCommandList->SetGraphicsShader(Shader::Stage::VS, m_shaders[VS_SCREEN_QUAD]);
	pCommandList->SetGraphicsShader(Shader::Stage::PS, m_shaders[PS_UP_SAMPLE_BLEND]);
	pCommandList->DSSetState(Graphics::DEPTH_STENCIL_NONE);
	pCommandList->OMSetBlendState(Graphics::NON_PRE_MUL);

	// Set IA
	pCommandList->IASetPrimitiveTopology(PrimitiveTopology::TRIANGLELIST);

	// Set sampler
	const auto sampler = LINEAR_CLAMP;
	pCommandList->SetSamplerStates(Shader::Stage::PS, 0, 1, &sampler);

	const uint32_t width = static_cast<uint32_t>(m_filtered->GetWidth());
	const uint32_t height = m_filtered->GetHeight();
	const uint8_t numPasses = m_filtered->GetNumMips() - 1;
	for (uint8_t i = 0; i + 1 < numPasses; ++i)
	{
		const auto c = numPasses - i;
		const auto level = c - 1;

		// Set render target
		const auto rtv = EZ::GetRTV(m_filtered.get(), 0, level);
		pCommandList->OMSetRenderTargets(1, &rtv);

		// Set CBVs
		const EZ::ResourceView cbvs[] =
		{
			EZ::GetCBV(m_cbPerFrame.get(), frameIndex),
			EZ::GetCBV(m_cbPerPass.get(), i)
		};
		pCommandList->SetResources(Shader::Stage::PS, DescriptorType::CBV, 0, static_cast<uint32_t>(size(cbvs)), cbvs);

		// Set SRV
		const auto srv = EZ::GetSRV(m_filtered.get(), c, true);
		pCommandList->SetResources(Shader::Stage::PS, DescriptorType::SRV, 0, 1, &srv);

		// Set viewport
		const auto viewportX = width >> level;
		const auto viewportY = height >> level;
		Viewport viewport(0.0f, 0.0f, static_cast<float>(viewportX), static_cast<float>(viewportY));
		RectRange scissorRect(0, 0, viewportX, viewportY);
		pCommandList->RSSetViewports(1, &viewport);
		pCommandList->RSSetScissorRects(1, &scissorRect);

		// Draw
		pCommandList->Draw(3, 1, 0, 0);
	}

	// Final pass
	pCommandList->SetGraphicsShader(Shader::Stage::PS, m_shaders[PS_UP_SAMPLE]);

	// Set render target
	const auto rtv = EZ::GetRTV(m_filtered.get());
	pCommandList->OMSetRenderTargets(1, &rtv);

	// Set CBVs
	const EZ::ResourceView cbvs[] =
	{
		EZ::GetCBV(m_cbPerFrame.get(), frameIndex),
		EZ::GetCBV(m_cbPerPass.get(), numPasses - 1)
	};
	pCommandList->SetResources(Shader::Stage::PS, DescriptorType::CBV, 0, static_cast<uint32_t>(size(cbvs)), cbvs);

	// Set SRV
	const EZ::ResourceView srvs[] =
	{
		EZ::GetSRV(m_source.get()),
		EZ::GetSRV(m_filtered.get(), 1, true)
	};
	pCommandList->SetResources(Shader::Stage::PS, DescriptorType::SRV, 0, static_cast<uint32_t>(size(srvs)), srvs);

	// Set viewport
	Viewport viewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
	RectRange scissorRect(0, 0, width, height);
	pCommandList->RSSetViewports(1, &viewport);
	pCommandList->RSSetScissorRects(1, &scissorRect);

	// Draw
	pCommandList->Draw(3, 1, 0, 0);
}

void FilterEZ::upsampleCompute(EZ::CommandList* pCommandList, uint8_t frameIndex)
{
	// Up sampling
	// Set pipeline state
	pCommandList->SetComputeShader(m_shaders[CS_UP_SAMPLE_INPLACE]);

	// Set sampler
	const auto sampler = LINEAR_CLAMP;
	pCommandList->SetSamplerStates(Shader::Stage::CS, 0, 1, &sampler);

	const uint32_t width = static_cast<uint32_t>(m_filtered->GetWidth());
	const uint32_t height = m_filtered->GetHeight();
	const uint8_t numPasses = m_filtered->GetNumMips() - 1;
	for (uint8_t i = 0; i + 1 < numPasses; ++i)
	{
		const auto c = numPasses - i;
		const auto level = c - 1;

		// Set UAV
		const auto uav = m_typedUAV ? EZ::GetUAV(m_filtered.get(), level) : EZ::GetUAV(m_filtered.get(), level, Format::R32_UINT);
		pCommandList->SetResources(Shader::Stage::CS, DescriptorType::UAV, 0, 1, &uav);

		// Set CBVs
		const EZ::ResourceView cbvs[] =
		{
			EZ::GetCBV(m_cbPerFrame.get(), frameIndex),
			EZ::GetCBV(m_cbPerPass.get(), i)
		};
		pCommandList->SetResources(Shader::Stage::CS, DescriptorType::CBV, 0, static_cast<uint32_t>(size(cbvs)), cbvs);

		// Set SRV
		const auto srv = EZ::GetSRV(m_filtered.get(), c, true);
		pCommandList->SetResources(Shader::Stage::CS, DescriptorType::SRV, 0, 1, &srv);

		// Dispatch grid
		const auto threadsX = width >> level;
		const auto threadsY = height >> level;
		pCommandList->Dispatch(XUSG_DIV_UP(threadsX, 8), XUSG_DIV_UP(threadsY, 8), 1);
	}

	// Final pass
	pCommandList->SetComputeShader(m_shaders[CS_UP_SAMPLE]);

	// Set UAV
	const auto uav = EZ::GetUAV(m_filtered.get());
	pCommandList->SetResources(Shader::Stage::CS, DescriptorType::UAV, 0, 1, &uav);

	// Set CBVs
	const EZ::ResourceView cbvs[] =
	{
		EZ::GetCBV(m_cbPerFrame.get(), frameIndex),
		EZ::GetCBV(m_cbPerPass.get(), numPasses - 1)
	};
	pCommandList->SetResources(Shader::Stage::CS, DescriptorType::CBV, 0, static_cast<uint32_t>(size(cbvs)), cbvs);

	// Set SRV
	const EZ::ResourceView srvs[] =
	{
		EZ::GetSRV(m_source.get()),
		EZ::GetSRV(m_filtered.get(), 1, true)
	};
	pCommandList->SetResources(Shader::Stage::CS, DescriptorType::SRV, 0, static_cast<uint32_t>(size(srvs)), srvs);

	// Dispatch grid
	pCommandList->Dispatch(XUSG_DIV_UP(width, 8), XUSG_DIV_UP(height, 8), 1);
}
