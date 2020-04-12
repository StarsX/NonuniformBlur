//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "Filter.h"
#define _INDEPENDENT_DDS_LOADER_
#include "Advanced/XUSGDDSLoader.h"
#undef _INDEPENDENT_DDS_LOADER_

#define SizeOfInUint32(obj)	DIV_UP(sizeof(obj), sizeof(uint32_t))

using namespace std;
using namespace DirectX;
using namespace XUSG;

struct GaussianConstants
{
	struct Immutable
	{
		XMFLOAT2	Focus;
		float		Sigma;
	} Imm;
	uint32_t Level;
};

Filter::Filter(const Device& device) :
	m_device(device),
	m_imageSize(1, 1),
	m_numMips(11)
{
	m_shaderPool = ShaderPool::MakeUnique();
	m_graphicsPipelineCache = Graphics::PipelineCache::MakeUnique(device);
	m_computePipelineCache = Compute::PipelineCache::MakeUnique(device);
	m_descriptorTableCache = DescriptorTableCache::MakeUnique(device);
	m_pipelineLayoutCache = PipelineLayoutCache::MakeUnique(device);
}

Filter::~Filter()
{
}

bool Filter::Init(CommandList* pCommandList,  vector<Resource>& uploaders,
	Format rtFormat, const wchar_t* fileName, bool typedUAV)
{
	m_typedUAV = typedUAV;

	// Load input image
	{
		DDS::Loader textureLoader;
		DDS::AlphaMode alphaMode;

		uploaders.push_back(nullptr);
		N_RETURN(textureLoader.CreateTextureFromFile(m_device, pCommandList, fileName,
			8192, false, m_source, uploaders.back(), &alphaMode), false);
	}

	// Create resources and pipelines
	m_imageSize.x = static_cast<uint32_t>(m_source->GetResource()->GetDesc().Width);
	m_imageSize.y = m_source->GetResource()->GetDesc().Height;
	m_numMips = (max)(Log2((max)(m_imageSize.x, m_imageSize.y)), 0ui8) + 1;

	m_filtered = RenderTarget::MakeShared();
	m_filtered->Create(m_device, m_imageSize.x, m_imageSize.y, rtFormat, 1, typedUAV ?
		ResourceFlag::ALLOW_UNORDERED_ACCESS : ResourceFlag::NEED_PACKED_UAV,
		m_numMips, 1, nullptr, false, L"FilteredImage");

	N_RETURN(createPipelineLayouts(), false);
	N_RETURN(createPipelines(rtFormat), false);
	N_RETURN(createDescriptorTables(), false);

	return true;
}

void Filter::Process(const CommandList* pCommandList, XMFLOAT2 focus, float sigma,
	PipelineType pipelineType)
{
	// Set Descriptor pools
	const DescriptorPool descriptorPools[] =
	{
		m_descriptorTableCache->GetDescriptorPool(CBV_SRV_UAV_POOL),
		m_descriptorTableCache->GetDescriptorPool(SAMPLER_POOL)
	};
	pCommandList->SetDescriptorPools(static_cast<uint32_t>(size(descriptorPools)), descriptorPools);

	ResourceBarrier barriers[2];
	uint32_t numBarriers;

	switch (pipelineType)
	{
	case GRAPHICS:
		numBarriers = generateMipsGraphics(pCommandList, barriers);
		upsampleGraphics(pCommandList, barriers, numBarriers, focus, sigma);
		break;
	case COMPUTE:
		numBarriers = generateMipsCompute(pCommandList, barriers);
		upsampleCompute(pCommandList, barriers, numBarriers, focus, sigma);
		break;
	default:
		numBarriers = generateMipsCompute(pCommandList, barriers);
		m_filtered->SetBarrier(barriers, m_numMips - 1, ResourceState::UNORDERED_ACCESS, --numBarriers);
		upsampleGraphics(pCommandList, barriers, numBarriers, focus, sigma);
	}
}

ResourceBase& Filter::GetResult()
{
	return *m_filtered;
}

void Filter::GetImageSize(uint32_t& width, uint32_t& height) const
{
	width = m_imageSize.x;
	height = m_imageSize.y;
}

bool Filter::createPipelineLayouts()
{
	// Resampling graphics
	{
		const auto utilPipelineLayout = Util::PipelineLayout::MakeUnique();
		utilPipelineLayout->SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout->SetRange(1, DescriptorType::SRV, 1, 0);
		utilPipelineLayout->SetShaderStage(0, Shader::PS);
		utilPipelineLayout->SetShaderStage(1, Shader::PS);
		X_RETURN(m_pipelineLayouts[RESAMPLE_G], utilPipelineLayout->GetPipelineLayout(
			*m_pipelineLayoutCache, PipelineLayoutFlag::NONE, L"ResamplingGraphicsLayout"), false);
	}

	// Resampling compute
	{
		const auto utilPipelineLayout = Util::PipelineLayout::MakeUnique();
		utilPipelineLayout->SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout->SetRange(1, DescriptorType::UAV, 1, 0, 0,
			DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		utilPipelineLayout->SetRange(2, DescriptorType::SRV, 1, 0);
		X_RETURN(m_pipelineLayouts[RESAMPLE_C], utilPipelineLayout->GetPipelineLayout(
			*m_pipelineLayoutCache, PipelineLayoutFlag::NONE, L"ResamplingLayout"), false);
	}

	// Up sampling graphics
	{
		const auto utilPipelineLayout = Util::PipelineLayout::MakeUnique();
		utilPipelineLayout->SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout->SetRange(1, DescriptorType::SRV, 1, 0);
		utilPipelineLayout->SetConstants(2, SizeOfInUint32(GaussianConstants), 0);
		utilPipelineLayout->SetShaderStage(0, Shader::PS);
		utilPipelineLayout->SetShaderStage(1, Shader::PS);
		utilPipelineLayout->SetShaderStage(2, Shader::PS);
		X_RETURN(m_pipelineLayouts[UP_SAMPLE_G], utilPipelineLayout->GetPipelineLayout(
			*m_pipelineLayoutCache, PipelineLayoutFlag::NONE, L"UpSamplingGraphicsLayout"), false);
	}

	// Up sampling compute
	{
		const auto utilPipelineLayout = Util::PipelineLayout::MakeUnique();
		utilPipelineLayout->SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout->SetRange(1, DescriptorType::UAV, 1, 0, 0,
			DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		utilPipelineLayout->SetRange(2, DescriptorType::SRV, 1, 0);
		utilPipelineLayout->SetConstants(3, SizeOfInUint32(GaussianConstants), 0);
		X_RETURN(m_pipelineLayouts[UP_SAMPLE_C], utilPipelineLayout->GetPipelineLayout(
			*m_pipelineLayoutCache, PipelineLayoutFlag::NONE, L"UpSamplingComputeLayout"), false);
	}

	// Final pass graphics
	{
		const auto utilPipelineLayout = Util::PipelineLayout::MakeUnique();
		utilPipelineLayout->SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout->SetRange(1, DescriptorType::SRV, 2, 0);
		utilPipelineLayout->SetConstants(2, SizeOfInUint32(GaussianConstants), 0);
		utilPipelineLayout->SetShaderStage(0, Shader::PS);
		utilPipelineLayout->SetShaderStage(1, Shader::PS);
		utilPipelineLayout->SetShaderStage(2, Shader::PS);
		X_RETURN(m_pipelineLayouts[FINAL_G], utilPipelineLayout->GetPipelineLayout(
			*m_pipelineLayoutCache, PipelineLayoutFlag::NONE, L"FinalPassGraphicsLayout"), false);
	}

	// Final pass compute
	{
		const auto utilPipelineLayout = Util::PipelineLayout::MakeUnique();
		utilPipelineLayout->SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout->SetRange(1, DescriptorType::UAV, 1, 0, 0,
			DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		utilPipelineLayout->SetRange(2, DescriptorType::SRV, 2, 0);
		utilPipelineLayout->SetConstants(3, SizeOfInUint32(GaussianConstants), 0);
		X_RETURN(m_pipelineLayouts[FINAL_C], utilPipelineLayout->GetPipelineLayout(
			*m_pipelineLayoutCache, PipelineLayoutFlag::NONE, L"FinalPassComputeLayout"), false);
	}

	return true;
}

bool Filter::createPipelines(Format rtFormat)
{
	auto vsIndex = 0u;
	auto psIndex = 0u;
	auto csIndex = 0u;

	// Resampling graphics
	N_RETURN(m_shaderPool->CreateShader(Shader::Stage::VS, vsIndex, L"VSScreenQuad.cso"), false);
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, psIndex, L"PSResample.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[RESAMPLE_G]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, vsIndex));
		state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, psIndex++));
		state->DSSetState(Graphics::DEPTH_STENCIL_NONE, *m_graphicsPipelineCache);
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->OMSetNumRenderTargets(1);
		state->OMSetRTVFormat(0, rtFormat);
		X_RETURN(m_pipelines[RESAMPLE_G], state->GetPipeline(*m_graphicsPipelineCache, L"ResamplingGraphics"), false);
	}

	// Resampling compute
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"CSResample.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[RESAMPLE_C]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[RESAMPLE_C], state->GetPipeline(*m_computePipelineCache, L"Resampling"), false);
	}

	// Up sampling graphics
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, psIndex, L"PSUpSample.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[UP_SAMPLE_G]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, vsIndex));
		state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, psIndex++));
		state->DSSetState(Graphics::DEPTH_STENCIL_NONE, *m_graphicsPipelineCache);
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->OMSetBlendState(Graphics::NON_PRE_MUL, *m_graphicsPipelineCache);
		state->OMSetNumRenderTargets(1);
		state->OMSetRTVFormat(0, rtFormat);
		X_RETURN(m_pipelines[UP_SAMPLE_G], state->GetPipeline(*m_graphicsPipelineCache, L"UpSamplingGraphics"), false);
	}

	// Up sampling compute
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, m_typedUAV ? L"CSUpSample.cso" : L"CSUpSampleU.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[UP_SAMPLE_C]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[UP_SAMPLE_C], state->GetPipeline(*m_computePipelineCache, L"UpSamplingCompute"), false);
	}

	// Final pass graphics
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, psIndex, L"PSFinal.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[FINAL_G]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, vsIndex));
		state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, psIndex));
		state->DSSetState(Graphics::DEPTH_STENCIL_NONE, *m_graphicsPipelineCache);
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->OMSetNumRenderTargets(1);
		state->OMSetRTVFormat(0, rtFormat);
		X_RETURN(m_pipelines[FINAL_G], state->GetPipeline(*m_graphicsPipelineCache, L"FinalPassGraphics"), false);
	}

	// Final pass compute
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"CSFinal.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[FINAL_C]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex));
		X_RETURN(m_pipelines[FINAL_C], state->GetPipeline(*m_computePipelineCache, L"FinalPassCompute"), false);
	}

	return true;
}

bool Filter::createDescriptorTables()
{
	// Get UAVs for resampling
	m_uavTables[UAV_TABLE_TYPED].resize(m_numMips);
	for (auto i = 0ui8; i < m_numMips; ++i)
	{
		// Get UAV
		const auto utilUavTable = Util::DescriptorTable::MakeUnique();
		utilUavTable->SetDescriptors(0, 1, &m_filtered->GetUAV(i));
		X_RETURN(m_uavTables[UAV_TABLE_TYPED][i], utilUavTable->GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	if (!m_typedUAV)
	{
		m_uavTables[UAV_TABLE_PACKED].resize(m_numMips);
		for (auto i = 0ui8; i < m_numMips; ++i)
		{
			// Get UAV
			const auto utilUavTable = Util::DescriptorTable::MakeUnique();
			utilUavTable->SetDescriptors(0, 1, &m_filtered->GetPackedUAV(i));
			X_RETURN(m_uavTables[UAV_TABLE_PACKED][i], utilUavTable->GetCbvSrvUavTable(*m_descriptorTableCache), false);
		}
	}

	// Get SRVs for resampling
	m_srvTables.resize(m_numMips);
	for (auto i = 0ui8; i < m_numMips; ++i)
	{
		const auto utilSrvTable = Util::DescriptorTable::MakeUnique();
		utilSrvTable->SetDescriptors(0, 1, i ? &m_filtered->GetSRVLevel(i) : &m_source->GetSRV());
		X_RETURN(m_srvTables[i], utilSrvTable->GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	// Create the sampler table
	const auto samplerTable = Util::DescriptorTable::MakeUnique();
	const auto sampler = LINEAR_CLAMP;
	samplerTable->SetSamplers(0, 1, &sampler, *m_descriptorTableCache);
	X_RETURN(m_samplerTable, samplerTable->GetSamplerTable(*m_descriptorTableCache), false);

	return true;
}

uint32_t Filter::generateMipsGraphics(const CommandList* pCommandList, ResourceBarrier* pBarriers)
{
	// Generate mipmaps
	return m_filtered->GenerateMips(pCommandList, pBarriers, ResourceState::PIXEL_SHADER_RESOURCE,
		m_pipelineLayouts[RESAMPLE_G], m_pipelines[RESAMPLE_G], m_srvTables.data(), 1, m_samplerTable, 0);
}

uint32_t Filter::generateMipsCompute(const CommandList* pCommandList, ResourceBarrier* pBarriers)
{
	// Generate mipmaps
	return m_filtered->AsTexture2D()->GenerateMips(pCommandList, pBarriers, 8, 8, 1,
		ResourceState::NON_PIXEL_SHADER_RESOURCE, m_pipelineLayouts[RESAMPLE_C],
		m_pipelines[RESAMPLE_C], &m_uavTables[UAV_TABLE_TYPED][1], 1, m_samplerTable, 0, 0, &m_srvTables[0], 2);
}

void Filter::upsampleGraphics(const CommandList* pCommandList, ResourceBarrier* pBarriers,
	uint32_t numBarriers, XMFLOAT2 focus, float sigma)
{
	// Up sampling
	pCommandList->SetGraphicsPipelineLayout(m_pipelineLayouts[UP_SAMPLE_G]);
	pCommandList->SetPipelineState(m_pipelines[UP_SAMPLE_G]);
	pCommandList->SetGraphicsDescriptorTable(0, m_samplerTable);

	GaussianConstants cb = { focus, sigma };
	pCommandList->SetGraphics32BitConstants(2, SizeOfInUint32(cb.Imm), &cb);

	const uint8_t numPasses = m_numMips - 1;
	for (auto i = 0ui8; i + 1 < numPasses; ++i)
	{
		const auto c = numPasses - i;
		const auto level = c - 1;
		pCommandList->SetGraphics32BitConstant(2, level, SizeOfInUint32(cb.Imm));
		numBarriers = m_filtered->Blit(pCommandList, pBarriers, level, c,
			ResourceState::PIXEL_SHADER_RESOURCE, m_srvTables[c], 1, numBarriers);
	}

	// Final pass
	pCommandList->SetGraphicsPipelineLayout(m_pipelineLayouts[FINAL_G]);
	pCommandList->SetPipelineState(m_pipelines[FINAL_G]);
	pCommandList->SetGraphicsDescriptorTable(0, m_samplerTable);
	pCommandList->SetGraphics32BitConstants(2, SizeOfInUint32(cb), &cb);
	numBarriers = m_filtered->Blit(pCommandList, pBarriers, 0, 1,
		ResourceState::PIXEL_SHADER_RESOURCE, m_srvTables[0], 1, numBarriers);
}

void Filter::upsampleCompute(const CommandList* pCommandList, ResourceBarrier* pBarriers,
	uint32_t numBarriers, XMFLOAT2 focus, float sigma)
{
	// Up sampling
	pCommandList->SetComputePipelineLayout(m_pipelineLayouts[UP_SAMPLE_C]);
	pCommandList->SetPipelineState(m_pipelines[UP_SAMPLE_C]);
	pCommandList->SetComputeDescriptorTable(0, m_samplerTable);

	GaussianConstants cb = { focus, sigma };
	pCommandList->SetCompute32BitConstants(3, SizeOfInUint32(cb.Imm), &cb);

	const uint8_t numPasses = m_numMips - 1;
	for (auto i = 0ui8; i + 1 < numPasses; ++i)
	{
		const auto c = numPasses - i;
		const auto level = c - 1;
		pCommandList->SetCompute32BitConstant(3, level, SizeOfInUint32(cb.Imm));
		numBarriers = m_filtered->AsTexture2D()->Blit(pCommandList, pBarriers,
			8, 8, 1, level, c, ResourceState::NON_PIXEL_SHADER_RESOURCE,
			m_uavTables[m_typedUAV ? UAV_TABLE_TYPED : UAV_TABLE_PACKED][level],
			1, numBarriers, m_srvTables[c], 2);
	}

	// Final pass
	pCommandList->SetComputePipelineLayout(m_pipelineLayouts[FINAL_C]);
	pCommandList->SetPipelineState(m_pipelines[FINAL_C]);
	pCommandList->SetComputeDescriptorTable(0, m_samplerTable);
	pCommandList->SetCompute32BitConstants(3, SizeOfInUint32(cb), &cb);
	numBarriers = m_filtered->AsTexture2D()->Blit(pCommandList, pBarriers,
		8, 8, 1, 0, 1, ResourceState::NON_PIXEL_SHADER_RESOURCE,
		m_uavTables[UAV_TABLE_TYPED][0], 1, numBarriers, m_srvTables[0], 2);
}
