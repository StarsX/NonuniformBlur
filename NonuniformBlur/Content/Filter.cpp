//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "Filter.h"
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

Filter::Filter() :
	m_imageSize(1, 1)
{
	m_shaderLib = ShaderLib::MakeUnique();
}

Filter::~Filter()
{
}

bool Filter::Init(CommandList* pCommandList, const DescriptorTableLib::sptr& descriptorTableLib,
	vector<Resource::uptr>& uploaders, Format rtFormat, const wchar_t* fileName, bool typedUAV)
{
	const auto pDevice = pCommandList->GetDevice();
	m_graphicsPipelineLib = Graphics::PipelineLib::MakeUnique(pDevice);
	m_computePipelineLib = Compute::PipelineLib::MakeUnique(pDevice);
	m_pipelineLayoutLib = PipelineLayoutLib::MakeUnique(pDevice);
	m_descriptorTableLib = descriptorTableLib;

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
	const auto numMips = CalculateMipLevels(m_imageSize.x, m_imageSize.y);

	m_filtered = RenderTarget::MakeUnique();
	m_filtered->Create(pDevice, m_imageSize.x, m_imageSize.y, rtFormat, 1, typedUAV ?
		ResourceFlag::ALLOW_UNORDERED_ACCESS : ResourceFlag::NEED_PACKED_UAV,
		numMips, 1, nullptr, false, MemoryFlag::NONE, L"FilteredImage");

	m_cbPerFrame = ConstantBuffer::MakeUnique();
	XUSG_N_RETURN(m_cbPerFrame->Create(pDevice, sizeof(CBGaussian[FrameCount]),
		FrameCount, nullptr, MemoryType::UPLOAD, MemoryFlag::NONE, L"CBPerFrame"), false);

	XUSG_N_RETURN(createPipelineLayouts(), false);
	XUSG_N_RETURN(createPipelines(rtFormat), false);
	XUSG_N_RETURN(createDescriptorTables(), false);

	return true;
}

void Filter::UpdateFrame(DirectX::XMFLOAT2 focus, float sigma, uint8_t frameIndex)
{
	// Update per-frame CB
	{
		const auto pCbData = reinterpret_cast<CBGaussian*>(m_cbPerFrame->Map(frameIndex));
		pCbData->Focus = focus;
		pCbData->Sigma = sigma;
	}
}

void Filter::Process(CommandList* pCommandList, uint8_t frameIndex, PipelineType pipelineType)
{
	ResourceBarrier barriers[2];
	uint32_t numBarriers;

	switch (pipelineType)
	{
	case GRAPHICS:
		numBarriers = generateMipsGraphics(pCommandList, barriers);
		upsampleGraphics(pCommandList, barriers, numBarriers, frameIndex);
		break;
	case COMPUTE:
		numBarriers = generateMipsCompute(pCommandList, barriers);
		upsampleCompute(pCommandList, barriers, numBarriers, frameIndex);
		break;
	default:
		numBarriers = generateMipsCompute(pCommandList, barriers);
		m_filtered->SetBarrier(barriers, m_filtered->GetNumMips() - 1, ResourceState::UNORDERED_ACCESS, --numBarriers);
		upsampleGraphics(pCommandList, barriers, numBarriers, frameIndex);
	}
}

Resource* Filter::GetResult() const
{
	return m_filtered.get();
}

void Filter::GetImageSize(uint32_t& width, uint32_t& height) const
{
	width = m_imageSize.x;
	height = m_imageSize.y;
}

bool Filter::createPipelineLayouts()
{
	// Blit graphics
	{
		const auto utilPipelineLayout = Util::PipelineLayout::MakeUnique();
		utilPipelineLayout->SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout->SetRange(1, DescriptorType::SRV, 1, 0);
		utilPipelineLayout->SetShaderStage(0, Shader::PS);
		utilPipelineLayout->SetShaderStage(1, Shader::PS);
		XUSG_X_RETURN(m_pipelineLayouts[BLIT_GRAPHICS], utilPipelineLayout->GetPipelineLayout(
			m_pipelineLayoutLib.get(), PipelineLayoutFlag::NONE, L"GraphicsBlitLayout"), false);
	}

	// Blit compute
	{
		const auto utilPipelineLayout = Util::PipelineLayout::MakeUnique();
		utilPipelineLayout->SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout->SetRange(1, DescriptorType::UAV, 1, 0, 0, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		utilPipelineLayout->SetRange(2, DescriptorType::SRV, 1, 0);
		XUSG_X_RETURN(m_pipelineLayouts[BLIT_COMPUTE], utilPipelineLayout->GetPipelineLayout(
			m_pipelineLayoutLib.get(), PipelineLayoutFlag::NONE, L"ComputeBlitLayout"), false);
	}

	// Up sampling graphics with alpha blending
	{
		const auto utilPipelineLayout = Util::PipelineLayout::MakeUnique();
		utilPipelineLayout->SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout->SetRange(1, DescriptorType::SRV, 1, 0);
		utilPipelineLayout->SetRootCBV(2, 0, 0, Shader::PS);
		utilPipelineLayout->SetConstants(3, XUSG_UINT32_SIZE_OF(uint32_t), 1, 0, Shader::PS);
		utilPipelineLayout->SetShaderStage(0, Shader::PS);
		utilPipelineLayout->SetShaderStage(1, Shader::PS);
		XUSG_X_RETURN(m_pipelineLayouts[UP_SAMPLE_BLEND], utilPipelineLayout->GetPipelineLayout(
			m_pipelineLayoutLib.get(), PipelineLayoutFlag::NONE, L"UpSamplingBlendLayout"), false);
	}

	// Up sampling compute, in-place
	{
		const auto utilPipelineLayout = Util::PipelineLayout::MakeUnique();
		utilPipelineLayout->SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout->SetRange(1, DescriptorType::UAV, 1, 0, 0, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		utilPipelineLayout->SetRange(2, DescriptorType::SRV, 1, 0);
		utilPipelineLayout->SetRootCBV(3, 0);
		utilPipelineLayout->SetConstants(4, XUSG_UINT32_SIZE_OF(uint32_t), 1);
		XUSG_X_RETURN(m_pipelineLayouts[UP_SAMPLE_INPLACE], utilPipelineLayout->GetPipelineLayout(
			m_pipelineLayoutLib.get(), PipelineLayoutFlag::NONE, L"UpSamplingInPlaceLayout"), false);
	}

	// Up sampling graphics, for the final pass
	{
		const auto utilPipelineLayout = Util::PipelineLayout::MakeUnique();
		utilPipelineLayout->SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout->SetRange(1, DescriptorType::SRV, 2, 0);
		utilPipelineLayout->SetRootCBV(2, 0, 0, Shader::PS);
		utilPipelineLayout->SetConstants(3, XUSG_UINT32_SIZE_OF(uint32_t), 1, 0, Shader::PS);
		utilPipelineLayout->SetShaderStage(0, Shader::PS);
		utilPipelineLayout->SetShaderStage(1, Shader::PS);
		XUSG_X_RETURN(m_pipelineLayouts[UP_SAMPLE_GRAPHICS], utilPipelineLayout->GetPipelineLayout(
			m_pipelineLayoutLib.get(), PipelineLayoutFlag::NONE, L"UpSamplingGraphicsLayout"), false);
	}

	// Up sampling compute, for the final pass
	{
		const auto utilPipelineLayout = Util::PipelineLayout::MakeUnique();
		utilPipelineLayout->SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout->SetRange(1, DescriptorType::UAV, 1, 0, 0, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		utilPipelineLayout->SetRange(2, DescriptorType::SRV, 2, 0);
		utilPipelineLayout->SetRootCBV(3, 0);
		utilPipelineLayout->SetConstants(4, XUSG_UINT32_SIZE_OF(uint32_t), 1);
		XUSG_X_RETURN(m_pipelineLayouts[UP_SAMPLE_COMPUTE], utilPipelineLayout->GetPipelineLayout(
			m_pipelineLayoutLib.get(), PipelineLayoutFlag::NONE, L"UpSamplingComputeLayout"), false);
	}

	return true;
}

bool Filter::createPipelines(Format rtFormat)
{
	auto vsIndex = 0u;
	auto psIndex = 0u;
	auto csIndex = 0u;

	// Blit graphics
	XUSG_N_RETURN(m_shaderLib->CreateShader(Shader::Stage::VS, vsIndex, L"VSScreenQuad.cso"), false);
	{
		XUSG_N_RETURN(m_shaderLib->CreateShader(Shader::Stage::PS, psIndex, L"PSBlit2D.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[BLIT_GRAPHICS]);
		state->SetShader(Shader::Stage::VS, m_shaderLib->GetShader(Shader::Stage::VS, vsIndex));
		state->SetShader(Shader::Stage::PS, m_shaderLib->GetShader(Shader::Stage::PS, psIndex++));
		state->DSSetState(Graphics::DEPTH_STENCIL_NONE, m_graphicsPipelineLib.get());
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->OMSetNumRenderTargets(1);
		state->OMSetRTVFormat(0, rtFormat);
		XUSG_X_RETURN(m_pipelines[BLIT_GRAPHICS], state->GetPipeline(m_graphicsPipelineLib.get(), L"Blit_graphics"), false);
	}

	// Blit compute
	{
		XUSG_N_RETURN(m_shaderLib->CreateShader(Shader::Stage::CS, csIndex, L"CSBlit2D.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[BLIT_COMPUTE]);
		state->SetShader(m_shaderLib->GetShader(Shader::Stage::CS, csIndex++));
		XUSG_X_RETURN(m_pipelines[BLIT_COMPUTE], state->GetPipeline(m_computePipelineLib.get(), L"Blit_compute"), false);
	}

	// Up sampling graphics with alpha blending
	{
		XUSG_N_RETURN(m_shaderLib->CreateShader(Shader::Stage::PS, psIndex, L"PSUpSample_blend.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[UP_SAMPLE_BLEND]);
		state->SetShader(Shader::Stage::VS, m_shaderLib->GetShader(Shader::Stage::VS, vsIndex));
		state->SetShader(Shader::Stage::PS, m_shaderLib->GetShader(Shader::Stage::PS, psIndex++));
		state->DSSetState(Graphics::DEPTH_STENCIL_NONE, m_graphicsPipelineLib.get());
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->OMSetBlendState(Graphics::NON_PRE_MUL, m_graphicsPipelineLib.get());
		state->OMSetNumRenderTargets(1);
		state->OMSetRTVFormat(0, rtFormat);
		XUSG_X_RETURN(m_pipelines[UP_SAMPLE_BLEND], state->GetPipeline(m_graphicsPipelineLib.get(), L"UpSampling_alpha_blend"), false);
	}

	// Up sampling compute, in-place
	{
		XUSG_N_RETURN(m_shaderLib->CreateShader(Shader::Stage::CS, csIndex, m_typedUAV ? L"CSUpSample_in_place.cso" : L"CSUpSample_typeless.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[UP_SAMPLE_INPLACE]);
		state->SetShader(m_shaderLib->GetShader(Shader::Stage::CS, csIndex++));
		XUSG_X_RETURN(m_pipelines[UP_SAMPLE_INPLACE], state->GetPipeline(m_computePipelineLib.get(), L"UpSampling_in_place"), false);
	}

	// Up sampling graphics, for the final pass
	{
		XUSG_N_RETURN(m_shaderLib->CreateShader(Shader::Stage::PS, psIndex, L"PSUpSample.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[UP_SAMPLE_GRAPHICS]);
		state->SetShader(Shader::Stage::VS, m_shaderLib->GetShader(Shader::Stage::VS, vsIndex));
		state->SetShader(Shader::Stage::PS, m_shaderLib->GetShader(Shader::Stage::PS, psIndex));
		state->DSSetState(Graphics::DEPTH_STENCIL_NONE, m_graphicsPipelineLib.get());
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->OMSetNumRenderTargets(1);
		state->OMSetRTVFormat(0, rtFormat);
		XUSG_X_RETURN(m_pipelines[UP_SAMPLE_GRAPHICS], state->GetPipeline(m_graphicsPipelineLib.get(), L"UpSampling_graphics"), false);
	}

	// Up sampling compute, for the final pass
	{
		XUSG_N_RETURN(m_shaderLib->CreateShader(Shader::Stage::CS, csIndex, L"CSUpSample.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[UP_SAMPLE_COMPUTE]);
		state->SetShader(m_shaderLib->GetShader(Shader::Stage::CS, csIndex));
		XUSG_X_RETURN(m_pipelines[UP_SAMPLE_COMPUTE], state->GetPipeline(m_computePipelineLib.get(), L"UpSampling_compute"), false);
	}

	return true;
}

bool Filter::createDescriptorTables()
{
	const auto numMips = m_filtered->GetNumMips();

	// Get UAVs for resampling
	m_uavTables[UAV_TABLE_TYPED].resize(numMips);
	for (uint8_t i = 0; i < numMips; ++i)
	{
		// Get UAV
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_filtered->GetUAV(i));
		XUSG_X_RETURN(m_uavTables[UAV_TABLE_TYPED][i], descriptorTable->GetCbvSrvUavTable(m_descriptorTableLib.get()), false);
	}

	if (!m_typedUAV)
	{
		m_uavTables[UAV_TABLE_PACKED].resize(numMips);
		for (uint8_t i = 0; i < numMips; ++i)
		{
			// Get UAV
			const auto descriptorTable = Util::DescriptorTable::MakeUnique();
			descriptorTable->SetDescriptors(0, 1, &m_filtered->GetPackedUAV(i));
			XUSG_X_RETURN(m_uavTables[UAV_TABLE_PACKED][i], descriptorTable->GetCbvSrvUavTable(m_descriptorTableLib.get()), false);
		}
	}

	// Get SRVs for resampling
	m_srvTables.resize(numMips);
	for (uint8_t i = 0; i < numMips; ++i)
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, i ? &m_filtered->GetSRVLevel(i) : &m_source->GetSRV());
		XUSG_X_RETURN(m_srvTables[i], descriptorTable->GetCbvSrvUavTable(m_descriptorTableLib.get()), false);
	}

	// Create the sampler table
	const auto descriptorTable = Util::DescriptorTable::MakeUnique();
	const auto sampler = LINEAR_CLAMP;
	descriptorTable->SetSamplers(0, 1, &sampler, m_descriptorTableLib.get());
	XUSG_X_RETURN(m_samplerTable, descriptorTable->GetSamplerTable(m_descriptorTableLib.get()), false);

	return true;
}

uint32_t Filter::generateMipsGraphics(CommandList* pCommandList, ResourceBarrier* pBarriers)
{
	// Generate mipmaps
	return m_filtered->GenerateMips(pCommandList, pBarriers, ResourceState::PIXEL_SHADER_RESOURCE,
		m_pipelineLayouts[BLIT_GRAPHICS], m_pipelines[BLIT_GRAPHICS], m_srvTables.data(), 1, m_samplerTable, 0);
}

uint32_t Filter::generateMipsCompute(CommandList* pCommandList, ResourceBarrier* pBarriers)
{
	// Generate mipmaps
	return m_filtered->AsTexture()->GenerateMips(pCommandList, pBarriers, 8, 8, 1,
		ResourceState::NON_PIXEL_SHADER_RESOURCE, m_pipelineLayouts[BLIT_COMPUTE],
		m_pipelines[BLIT_COMPUTE], &m_uavTables[UAV_TABLE_TYPED][1], 1, m_samplerTable, 0, 0, &m_srvTables[0], 2);
}

void Filter::upsampleGraphics(CommandList* pCommandList, ResourceBarrier* pBarriers,
	uint32_t numBarriers, uint8_t frameIndex)
{
	// Up sampling
	const auto cbvOffset = m_cbPerFrame->GetCBVOffset(frameIndex);
	pCommandList->SetGraphicsPipelineLayout(m_pipelineLayouts[UP_SAMPLE_BLEND]);
	pCommandList->SetPipelineState(m_pipelines[UP_SAMPLE_BLEND]);
	pCommandList->SetGraphicsDescriptorTable(0, m_samplerTable);
	pCommandList->SetGraphicsRootConstantBufferView(2, m_cbPerFrame.get(), cbvOffset);

	const uint8_t numPasses = m_filtered->GetNumMips() - 1;
	for (uint8_t i = 0; i + 1 < numPasses; ++i)
	{
		const auto c = numPasses - i;
		const auto level = c - 1;
		pCommandList->SetGraphics32BitConstant(3, level);
		numBarriers = m_filtered->Blit(pCommandList, pBarriers, level, c,
			ResourceState::PIXEL_SHADER_RESOURCE, m_srvTables[c], 1, numBarriers);
	}

	// Final pass
	pCommandList->SetGraphicsPipelineLayout(m_pipelineLayouts[UP_SAMPLE_GRAPHICS]);
	pCommandList->SetPipelineState(m_pipelines[UP_SAMPLE_GRAPHICS]);
	pCommandList->SetGraphicsDescriptorTable(0, m_samplerTable);
	pCommandList->SetGraphicsRootConstantBufferView(2, m_cbPerFrame.get(), cbvOffset);
	pCommandList->SetGraphics32BitConstant(3, 0);
	numBarriers = m_filtered->Blit(pCommandList, pBarriers, 0, 1,
		ResourceState::PIXEL_SHADER_RESOURCE, m_srvTables[0], 1, numBarriers);
}

void Filter::upsampleCompute(CommandList* pCommandList, ResourceBarrier* pBarriers,
	uint32_t numBarriers, uint8_t frameIndex)
{
	// Up sampling
	const auto cbvOffset = m_cbPerFrame->GetCBVOffset(frameIndex);
	pCommandList->SetComputePipelineLayout(m_pipelineLayouts[UP_SAMPLE_INPLACE]);
	pCommandList->SetPipelineState(m_pipelines[UP_SAMPLE_INPLACE]);
	pCommandList->SetComputeDescriptorTable(0, m_samplerTable);
	pCommandList->SetComputeRootConstantBufferView(3, m_cbPerFrame.get(), cbvOffset);

	const uint8_t numPasses = m_filtered->GetNumMips() - 1;
	for (uint8_t i = 0; i + 1 < numPasses; ++i)
	{
		const auto c = numPasses - i;
		const auto level = c - 1;
		pCommandList->SetCompute32BitConstant(4, level);
		numBarriers = m_filtered->AsTexture()->Blit(pCommandList, pBarriers,
			8, 8, 1, level, c, ResourceState::NON_PIXEL_SHADER_RESOURCE,
			m_uavTables[m_typedUAV ? UAV_TABLE_TYPED : UAV_TABLE_PACKED][level],
			1, numBarriers, m_srvTables[c], 2);
	}

	// Final pass
	pCommandList->SetComputePipelineLayout(m_pipelineLayouts[UP_SAMPLE_COMPUTE]);
	pCommandList->SetPipelineState(m_pipelines[UP_SAMPLE_COMPUTE]);
	pCommandList->SetComputeDescriptorTable(0, m_samplerTable);
	pCommandList->SetComputeRootConstantBufferView(3, m_cbPerFrame.get(), cbvOffset);
	pCommandList->SetCompute32BitConstant(4, 0);
	numBarriers = m_filtered->AsTexture()->Blit(pCommandList, pBarriers,
		8, 8, 1, 0, 1, ResourceState::NON_PIXEL_SHADER_RESOURCE,
		m_uavTables[UAV_TABLE_TYPED][0], 1, numBarriers, m_srvTables[0], 2);
}
