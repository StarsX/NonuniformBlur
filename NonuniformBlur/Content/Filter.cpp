//--------------------------------------------------------------------------------------
// By XU, Tianchen
//--------------------------------------------------------------------------------------

#include "Filter.h"
#include "Advanced/XUSGDDSLoader.h"

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
	m_graphicsPipelineCache.SetDevice(device);
	m_computePipelineCache.SetDevice(device);
	m_descriptorTableCache.SetDevice(device);
	m_pipelineLayoutCache.SetDevice(device);
}

Filter::~Filter()
{
}

bool Filter::Init(const CommandList& commandList,  vector<Resource>& uploaders,
	Format rtFormat, const wchar_t* fileName, bool typedUAV)
{
	// Load input image
	{
		DDS::Loader textureLoader;
		DDS::AlphaMode alphaMode;

		uploaders.push_back(nullptr);
		N_RETURN(textureLoader.CreateTextureFromFile(m_device, commandList, fileName,
			8192, false, m_source, uploaders.back(), &alphaMode), false);
	}

	// Create resources and pipelines
	m_imageSize.x = static_cast<uint32_t>(m_source->GetResource()->GetDesc().Width);
	m_imageSize.y = m_source->GetResource()->GetDesc().Height;
	m_numMips = (max)(Log2((max)(m_imageSize.x, m_imageSize.y)), 0ui8) + 1;

	m_filtered.Create(m_device, m_imageSize.x, m_imageSize.y, rtFormat, 1, typedUAV ?
		ResourceFlag::ALLOW_UNORDERED_ACCESS : ResourceFlag::BIND_PACKED_UAV,
		m_numMips, 1, ResourceState::COMMON, nullptr, false, L"FilteredImage");

	N_RETURN(createPipelineLayouts(), false);
	N_RETURN(createPipelines(rtFormat, typedUAV), false);
	N_RETURN(createDescriptorTables(), false);

	return true;
}

void Filter::Process(const CommandList& commandList, XMFLOAT2 focus, float sigma,
	PipelineType pipelineType)
{
	// Set Descriptor pools
	const DescriptorPool descriptorPools[] =
	{
		m_descriptorTableCache.GetDescriptorPool(CBV_SRV_UAV_POOL),
		m_descriptorTableCache.GetDescriptorPool(SAMPLER_POOL)
	};
	commandList.SetDescriptorPools(static_cast<uint32_t>(size(descriptorPools)), descriptorPools);

	ResourceBarrier barriers[2];
	uint32_t numBarriers;

	switch (pipelineType)
	{
	case GRAPHICS:
		numBarriers = generateMipsGraphics(commandList, barriers);
		upsampleGraphics(commandList, barriers, numBarriers, focus, sigma);
		break;
	case COMPUTE:
		numBarriers = generateMipsCompute(commandList, barriers);
		upsampleCompute(commandList, barriers, numBarriers, focus, sigma);
		break;
	default:
		numBarriers = generateMipsCompute(commandList, barriers);
		m_filtered.SetBarrier(barriers, m_numMips - 1, ResourceState::UNORDERED_ACCESS, --numBarriers);
		upsampleGraphics(commandList, barriers, numBarriers, focus, sigma);
	}
}

ResourceBase& Filter::GetResult()
{
	return m_filtered;
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
		Util::PipelineLayout utilPipelineLayout;
		utilPipelineLayout.SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout.SetRange(1, DescriptorType::SRV, 1, 0);
		utilPipelineLayout.SetShaderStage(0, Shader::PS);
		utilPipelineLayout.SetShaderStage(1, Shader::PS);
		X_RETURN(m_pipelineLayouts[RESAMPLE_G], utilPipelineLayout.GetPipelineLayout(
			m_pipelineLayoutCache, PipelineLayoutFlag::NONE, L"ResamplingGraphicsLayout"), false);
	}

	// Resampling compute
	{
		Util::PipelineLayout utilPipelineLayout;
		utilPipelineLayout.SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout.SetRange(1, DescriptorType::UAV, 1, 0, 0,
			DescriptorRangeFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		utilPipelineLayout.SetRange(2, DescriptorType::SRV, 1, 0);
		X_RETURN(m_pipelineLayouts[RESAMPLE_C], utilPipelineLayout.GetPipelineLayout(
			m_pipelineLayoutCache, PipelineLayoutFlag::NONE, L"ResamplingLayout"), false);
	}

	// Up sampling graphics
	{
		Util::PipelineLayout utilPipelineLayout;
		utilPipelineLayout.SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout.SetRange(1, DescriptorType::SRV, 1, 0);
		utilPipelineLayout.SetConstants(2, SizeOfInUint32(GaussianConstants), 0);
		utilPipelineLayout.SetShaderStage(0, Shader::PS);
		utilPipelineLayout.SetShaderStage(1, Shader::PS);
		utilPipelineLayout.SetShaderStage(2, Shader::PS);
		X_RETURN(m_pipelineLayouts[UP_SAMPLE_G], utilPipelineLayout.GetPipelineLayout(
			m_pipelineLayoutCache, PipelineLayoutFlag::NONE, L"UpSamplingGraphicsLayout"), false);
	}

	// Up sampling compute
	{
		Util::PipelineLayout utilPipelineLayout;
		utilPipelineLayout.SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout.SetRange(1, DescriptorType::UAV, 1, 0, 0,
			DescriptorRangeFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		utilPipelineLayout.SetRange(2, DescriptorType::SRV, 1, 0);
		utilPipelineLayout.SetConstants(3, SizeOfInUint32(GaussianConstants), 0);
		X_RETURN(m_pipelineLayouts[UP_SAMPLE_C], utilPipelineLayout.GetPipelineLayout(
			m_pipelineLayoutCache, PipelineLayoutFlag::NONE, L"UpSamplingComputeLayout"), false);
	}

	// Final pass graphics
	{
		Util::PipelineLayout utilPipelineLayout;
		utilPipelineLayout.SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout.SetRange(1, DescriptorType::SRV, 2, 0);
		utilPipelineLayout.SetConstants(2, SizeOfInUint32(GaussianConstants), 0);
		utilPipelineLayout.SetShaderStage(0, Shader::PS);
		utilPipelineLayout.SetShaderStage(1, Shader::PS);
		utilPipelineLayout.SetShaderStage(2, Shader::PS);
		X_RETURN(m_pipelineLayouts[FINAL_G], utilPipelineLayout.GetPipelineLayout(
			m_pipelineLayoutCache, PipelineLayoutFlag::NONE, L"FinalPassGraphicsLayout"), false);
	}

	// Final pass compute
	{
		Util::PipelineLayout utilPipelineLayout;
		utilPipelineLayout.SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout.SetRange(1, DescriptorType::UAV, 1, 0, 0,
			DescriptorRangeFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		utilPipelineLayout.SetRange(2, DescriptorType::SRV, 2, 0);
		utilPipelineLayout.SetConstants(3, SizeOfInUint32(GaussianConstants), 0);
		X_RETURN(m_pipelineLayouts[FINAL_C], utilPipelineLayout.GetPipelineLayout(
			m_pipelineLayoutCache, PipelineLayoutFlag::NONE, L"FinalPassComputeLayout"), false);
	}

	return true;
}

bool Filter::createPipelines(Format rtFormat, bool typedUAV)
{
	auto vsIndex = 0u;
	auto psIndex = 0u;
	auto csIndex = 0u;

	// Resampling graphics
	N_RETURN(m_shaderPool.CreateShader(Shader::Stage::VS, vsIndex, L"VSScreenQuad.cso"), false);
	{
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::PS, psIndex, L"PSResample.cso"), false);

		Graphics::State state;
		state.SetPipelineLayout(m_pipelineLayouts[RESAMPLE_G]);
		state.SetShader(Shader::Stage::VS, m_shaderPool.GetShader(Shader::Stage::VS, vsIndex));
		state.SetShader(Shader::Stage::PS, m_shaderPool.GetShader(Shader::Stage::PS, psIndex++));
		state.DSSetState(Graphics::DEPTH_STENCIL_NONE, m_graphicsPipelineCache);
		state.IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state.OMSetNumRenderTargets(1);
		state.OMSetRTVFormat(0, rtFormat);
		X_RETURN(m_pipelines[RESAMPLE_G], state.GetPipeline(m_graphicsPipelineCache, L"ResamplingGraphics"), false);
	}

	// Resampling compute
	{
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::CS, csIndex, typedUAV ? L"CSResample.cso" : L"CSResampleU.cso"), false);

		Compute::State state;
		state.SetPipelineLayout(m_pipelineLayouts[RESAMPLE_C]);
		state.SetShader(m_shaderPool.GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[RESAMPLE_C], state.GetPipeline(m_computePipelineCache, L"Resampling"), false);
	}

	// Up sampling graphics
	{
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::PS, psIndex, L"PSUpSample.cso"), false);

		Graphics::State state;
		state.SetPipelineLayout(m_pipelineLayouts[UP_SAMPLE_G]);
		state.SetShader(Shader::Stage::VS, m_shaderPool.GetShader(Shader::Stage::VS, vsIndex));
		state.SetShader(Shader::Stage::PS, m_shaderPool.GetShader(Shader::Stage::PS, psIndex++));
		state.DSSetState(Graphics::DEPTH_STENCIL_NONE, m_graphicsPipelineCache);
		state.IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state.OMSetBlendState(Graphics::NON_PRE_MUL, m_graphicsPipelineCache);
		state.OMSetNumRenderTargets(1);
		state.OMSetRTVFormat(0, rtFormat);
		X_RETURN(m_pipelines[UP_SAMPLE_G], state.GetPipeline(m_graphicsPipelineCache, L"UpSamplingGraphics"), false);
	}

	// Up sampling compute
	{
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::CS, csIndex, typedUAV ? L"CSUpSample.cso" : L"CSUpSampleU.cso"), false);

		Compute::State state;
		state.SetPipelineLayout(m_pipelineLayouts[UP_SAMPLE_C]);
		state.SetShader(m_shaderPool.GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[UP_SAMPLE_C], state.GetPipeline(m_computePipelineCache, L"UpSamplingCompute"), false);
	}

	// Final pass graphics
	{
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::PS, psIndex, L"PSFinal.cso"), false);

		Graphics::State state;
		state.SetPipelineLayout(m_pipelineLayouts[FINAL_G]);
		state.SetShader(Shader::Stage::VS, m_shaderPool.GetShader(Shader::Stage::VS, vsIndex));
		state.SetShader(Shader::Stage::PS, m_shaderPool.GetShader(Shader::Stage::PS, psIndex));
		state.DSSetState(Graphics::DEPTH_STENCIL_NONE, m_graphicsPipelineCache);
		state.IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state.OMSetNumRenderTargets(1);
		state.OMSetRTVFormat(0, rtFormat);
		X_RETURN(m_pipelines[FINAL_G], state.GetPipeline(m_graphicsPipelineCache, L"FinalPassGraphics"), false);
	}

	// Final pass compute
	{
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::CS, csIndex, typedUAV ? L"CSFinal.cso" : L"CSFinalU.cso"), false);

		Compute::State state;
		state.SetPipelineLayout(m_pipelineLayouts[FINAL_C]);
		state.SetShader(m_shaderPool.GetShader(Shader::Stage::CS, csIndex));
		X_RETURN(m_pipelines[FINAL_C], state.GetPipeline(m_computePipelineCache, L"FinalPassCompute"), false);
	}

	return true;
}

bool Filter::createDescriptorTables()
{
	// Get UAVs for resampling
	m_uavTables.resize(m_numMips);
	for (auto i = 0ui8; i < m_numMips; ++i)
	{
		// Get UAV
		Util::DescriptorTable utilUavTable;
		utilUavTable.SetDescriptors(0, 1, &m_filtered.GetUAV(i));
		X_RETURN(m_uavTables[i], utilUavTable.GetCbvSrvUavTable(m_descriptorTableCache), false);
	}

	// Get SRVs for resampling
	m_srvTables.resize(m_numMips);
	for (auto i = 0ui8; i < m_numMips; ++i)
	{
		Util::DescriptorTable utilSrvTable;
		utilSrvTable.SetDescriptors(0, 1, i ? &m_filtered.GetSRVLevel(i) : &m_source->GetSRV());
		X_RETURN(m_srvTables[i], utilSrvTable.GetCbvSrvUavTable(m_descriptorTableCache), false);
	}

	// Create the sampler table
	Util::DescriptorTable samplerTable;
	const auto sampler = LINEAR_CLAMP;
	samplerTable.SetSamplers(0, 1, &sampler, m_descriptorTableCache);
	X_RETURN(m_samplerTable, samplerTable.GetSamplerTable(m_descriptorTableCache), false);

	return true;
}

uint32_t Filter::generateMipsGraphics(const CommandList& commandList, ResourceBarrier* pBarriers)
{
	// Generate mipmaps
	return m_filtered.GenerateMips(commandList, pBarriers, ResourceState::PIXEL_SHADER_RESOURCE,
		m_pipelineLayouts[RESAMPLE_G], m_pipelines[RESAMPLE_G], m_srvTables.data(), 1, m_samplerTable, 0);
}

uint32_t Filter::generateMipsCompute(const CommandList& commandList, ResourceBarrier* pBarriers)
{
	// Generate mipmaps
	return m_filtered.Texture2D::GenerateMips(commandList, pBarriers, 8, 8, 1,
		ResourceState::NON_PIXEL_SHADER_RESOURCE, m_pipelineLayouts[RESAMPLE_C],
		m_pipelines[RESAMPLE_C], &m_uavTables[1], 1, m_samplerTable, 0, 0, &m_srvTables[0], 2);
}

void Filter::upsampleGraphics(const CommandList& commandList, ResourceBarrier* pBarriers,
	uint32_t numBarriers, XMFLOAT2 focus, float sigma)
{
	// Up sampling
	commandList.SetGraphicsPipelineLayout(m_pipelineLayouts[UP_SAMPLE_G]);
	commandList.SetPipelineState(m_pipelines[UP_SAMPLE_G]);
	commandList.SetGraphicsDescriptorTable(0, m_samplerTable);

	GaussianConstants cb = { focus, sigma };
	commandList.SetGraphics32BitConstants(2, SizeOfInUint32(cb.Imm), &cb);

	const uint8_t numPasses = m_numMips - 1;
	for (auto i = 0ui8; i + 1 < numPasses; ++i)
	{
		const auto c = numPasses - i;
		const auto level = c - 1;
		commandList.SetGraphics32BitConstant(2, level, SizeOfInUint32(cb.Imm));
		numBarriers = m_filtered.Blit(commandList, pBarriers, level, c,
			ResourceState::PIXEL_SHADER_RESOURCE, m_srvTables[c], 1, numBarriers);
	}

	// Final pass
	commandList.SetGraphicsPipelineLayout(m_pipelineLayouts[FINAL_G]);
	commandList.SetPipelineState(m_pipelines[FINAL_G]);
	commandList.SetGraphicsDescriptorTable(0, m_samplerTable);
	commandList.SetGraphics32BitConstants(2, SizeOfInUint32(cb), &cb);
	numBarriers = m_filtered.Blit(commandList, pBarriers, 0, 1,
		ResourceState::PIXEL_SHADER_RESOURCE, m_srvTables[0], 1, numBarriers);
}

void Filter::upsampleCompute(const CommandList& commandList, ResourceBarrier* pBarriers,
	uint32_t numBarriers, XMFLOAT2 focus, float sigma)
{
	// Up sampling
	commandList.SetComputePipelineLayout(m_pipelineLayouts[UP_SAMPLE_C]);
	commandList.SetPipelineState(m_pipelines[UP_SAMPLE_C]);
	commandList.SetComputeDescriptorTable(0, m_samplerTable);

	GaussianConstants cb = { focus, sigma };
	commandList.SetCompute32BitConstants(3, SizeOfInUint32(cb.Imm), &cb);

	const uint8_t numPasses = m_numMips - 1;
	for (auto i = 0ui8; i + 1 < numPasses; ++i)
	{
		const auto c = numPasses - i;
		const auto level = c - 1;
		commandList.SetCompute32BitConstant(3, level, SizeOfInUint32(cb.Imm));
		numBarriers = m_filtered.Texture2D::Blit(commandList, pBarriers,
			8, 8, 1, level, c, ResourceState::NON_PIXEL_SHADER_RESOURCE,
			m_uavTables[level], 1, numBarriers, m_srvTables[c], 2);
	}

	// Final pass
	commandList.SetComputePipelineLayout(m_pipelineLayouts[FINAL_C]);
	commandList.SetPipelineState(m_pipelines[FINAL_C]);
	commandList.SetComputeDescriptorTable(0, m_samplerTable);
	commandList.SetCompute32BitConstants(3, SizeOfInUint32(cb), &cb);
	numBarriers = m_filtered.Texture2D::Blit(commandList, pBarriers,
		8, 8, 1, 0, 1, ResourceState::NON_PIXEL_SHADER_RESOURCE,
		m_uavTables[0], 1, numBarriers, m_srvTables[0], 2);
}
