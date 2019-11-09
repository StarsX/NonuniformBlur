#include "stdafx.h"
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
		ResourceFlag::ALLOW_UNORDERED_ACCESS : ResourceFlag::BIND_PACKED_UAV, m_numMips);

	N_RETURN(createPipelineLayouts(), false);
	N_RETURN(createPipelines(rtFormat, typedUAV), false);
	N_RETURN(createDescriptorTables(), false);

	return true;
}

void Filter::Process(const CommandList& commandList, XMFLOAT2 focus, float sigma,
	ResourceState dstState, PipelineType pipelineType)
{
	// Set Descriptor pools
	const DescriptorPool descriptorPools[] =
	{
		m_descriptorTableCache.GetDescriptorPool(CBV_SRV_UAV_POOL),
		m_descriptorTableCache.GetDescriptorPool(SAMPLER_POOL)
	};
	commandList.SetDescriptorPools(static_cast<uint32_t>(size(descriptorPools)), descriptorPools);

	// Update image and generate mipmaps
	updateImage(commandList);
	generateMips(commandList);

	// Up sampling
	switch (pipelineType)
	{
	case COMPUTE:
		upSampleCompute(commandList, focus, sigma, dstState);
		break;
	default:
		upSampleHybrid(commandList, focus, sigma, dstState);
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
	// Resampling
	{
		Util::PipelineLayout utilPipelineLayout;
		utilPipelineLayout.SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout.SetRange(1, DescriptorType::SRV, 1, 0);
		utilPipelineLayout.SetRange(1, DescriptorType::UAV, 1, 0, 0,
			DescriptorRangeFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		X_RETURN(m_pipelineLayouts[RESAMPLE], utilPipelineLayout.GetPipelineLayout(
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
		utilPipelineLayout.SetRange(1, DescriptorType::SRV, 1, 0);
		utilPipelineLayout.SetRange(1, DescriptorType::UAV, 1, 0, 0,
			DescriptorRangeFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		utilPipelineLayout.SetConstants(2, SizeOfInUint32(GaussianConstants), 0);
		X_RETURN(m_pipelineLayouts[UP_SAMPLE_C], utilPipelineLayout.GetPipelineLayout(
			m_pipelineLayoutCache, PipelineLayoutFlag::NONE, L"UpSamplingComputeLayout"), false);
	}

	return true;
}

bool Filter::createPipelines(Format rtFormat, bool typedUAV)
{
	// Resampling
	{
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::CS, RESAMPLE, typedUAV ? L"CSResample.cso" : L"CSResampleU.cso"), false);

		Compute::State state;
		state.SetPipelineLayout(m_pipelineLayouts[RESAMPLE]);
		state.SetShader(m_shaderPool.GetShader(Shader::Stage::CS, RESAMPLE));
		X_RETURN(m_pipelines[RESAMPLE], state.GetPipeline(m_computePipelineCache, L"Resampling"), false);
	}

	// Up sampling graphics
	{
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::VS, 0, L"VSScreenQuad.cso"), false);
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::PS, 0, L"PSUpSample.cso"), false);

		Graphics::State state;
		state.SetPipelineLayout(m_pipelineLayouts[UP_SAMPLE_G]);
		state.SetShader(Shader::Stage::VS, m_shaderPool.GetShader(Shader::Stage::VS, 0));
		state.SetShader(Shader::Stage::PS, m_shaderPool.GetShader(Shader::Stage::PS, 0));
		state.IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state.OMSetBlendState(Graphics::NON_PRE_MUL, m_graphicsPipelineCache);
		state.OMSetNumRenderTargets(1);
		state.OMSetRTVFormat(0, rtFormat);
		X_RETURN(m_pipelines[UP_SAMPLE_G], state.GetPipeline(m_graphicsPipelineCache, L"UpSamplingGraphics"), false);
	}

	// Up sampling compute
	{
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::CS, UP_SAMPLE_C, typedUAV ? L"CSUpSample.cso" : L"CSUpSampleU.cso"), false);

		Compute::State state;
		state.SetPipelineLayout(m_pipelineLayouts[UP_SAMPLE_C]);
		state.SetShader(m_shaderPool.GetShader(Shader::Stage::CS, UP_SAMPLE_C));
		X_RETURN(m_pipelines[UP_SAMPLE_C], state.GetPipeline(m_computePipelineCache, L"UpSamplingCompute"), false);
	}

	return true;
}

bool Filter::createDescriptorTables()
{
	// Copy source
	m_uavSrvTables[TABLE_COPY].resize(2);
	{
		const Descriptor descriptors[] =
		{
			m_source->GetSRV(),
			m_filtered.GetUAV()
		};
		Util::DescriptorTable utilUavSrvTable;
		utilUavSrvTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		X_RETURN(m_uavSrvTables[TABLE_COPY][0], utilUavSrvTable.GetCbvSrvUavTable(m_descriptorTableCache), false);
	}

	const uint8_t numPasses = m_numMips > 0 ? m_numMips - 1 : 0;
	m_uavSrvTables[TABLE_RESAMPLE].resize(m_numMips);
	m_uavSrvTables[TABLE_UP_SAMPLE].resize(m_numMips);
	for (auto i = 0ui8; i < numPasses; ++i)
	{
		// Get UAV and SRVs
		{
			const Descriptor descriptors[] =
			{
				m_filtered.GetSRVLevel(i),
				m_filtered.GetUAV(i + 1)
			};
			Util::DescriptorTable utilUavSrvTable;
			utilUavSrvTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
			X_RETURN(m_uavSrvTables[TABLE_RESAMPLE][i], utilUavSrvTable.GetCbvSrvUavTable(m_descriptorTableCache), false);
		}

		{
			const auto coarser = numPasses - i;
			const auto current = coarser - 1;
			const Descriptor descriptors[] =
			{
				m_filtered.GetSRVLevel(coarser),
				m_filtered.GetUAV(current)
			};
			Util::DescriptorTable utilUavSrvTable;
			utilUavSrvTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
			X_RETURN(m_uavSrvTables[TABLE_UP_SAMPLE][i], utilUavSrvTable.GetCbvSrvUavTable(m_descriptorTableCache), false);
		}
	}

	// Get UAV and SRVs for graphics up-sampling
	{
		Util::DescriptorTable utilUavSrvTable;
		utilUavSrvTable.SetDescriptors(0, 1, &m_filtered.GetSRVLevel(numPasses));
		X_RETURN(m_uavSrvTables[TABLE_RESAMPLE][numPasses], utilUavSrvTable.GetCbvSrvUavTable(m_descriptorTableCache), false);
	}

	// Create the sampler table
	Util::DescriptorTable samplerTable;
	const auto sampler = LINEAR_CLAMP;
	samplerTable.SetSamplers(0, 1, &sampler, m_descriptorTableCache);
	X_RETURN(m_samplerTable, samplerTable.GetSamplerTable(m_descriptorTableCache), false);

	return true;
}

void Filter::updateImage(const CommandList& commandList)
{
	// Copy source
	commandList.SetComputePipelineLayout(m_pipelineLayouts[RESAMPLE]);
	commandList.SetPipelineState(m_pipelines[RESAMPLE]);
	commandList.SetComputeDescriptorTable(0, m_samplerTable);

	ResourceBarrier barrier;
	const auto numBarriers = m_filtered.SetBarrier(&barrier, ResourceState::UNORDERED_ACCESS);
	commandList.Barrier(numBarriers, &barrier);
	m_filtered.Texture2D::Blit(commandList, 8, 8, 1, m_uavSrvTables[TABLE_COPY][0], 1);
}

void Filter::generateMips(const CommandList& commandList)
{
	ResourceBarrier barriers[2];
	auto numBarriers = 0u;

	numBarriers = m_filtered.Texture2D::GenerateMips(commandList, barriers,
		8, 8, 1, ResourceState::PIXEL_SHADER_RESOURCE | ResourceState::NON_PIXEL_SHADER_RESOURCE,
		m_pipelineLayouts[RESAMPLE], m_pipelines[RESAMPLE], m_uavSrvTables[TABLE_RESAMPLE].data(),
		1, m_samplerTable, 0, numBarriers, nullptr, 0, 1, m_numMips - 1);
	commandList.Barrier(numBarriers, barriers);
}

void Filter::upSampleHybrid(const CommandList& commandList, XMFLOAT2 focus, float sigma, ResourceState dstState)
{
	// Up sampling
	ResourceBarrier barriers[2];
	auto numBarriers = 0u;
	commandList.SetGraphicsPipelineLayout(m_pipelineLayouts[UP_SAMPLE_G]);
	commandList.SetPipelineState(m_pipelines[UP_SAMPLE_G]);
	commandList.SetGraphicsDescriptorTable(0, m_samplerTable);

	const uint8_t numPasses = m_numMips - 1;
	GaussianConstants::Immutable cb = { focus, sigma };
	commandList.SetGraphics32BitConstants(2, SizeOfInUint32(cb), &cb);

	for (auto i = 0ui8; i < numPasses; ++i)
	{
		const auto c = numPasses - i;
		const auto level = c - 1;
		commandList.SetGraphics32BitConstant(2, level, SizeOfInUint32(cb));
		numBarriers = m_filtered.Blit(commandList, barriers, level, c, dstState,
			m_uavSrvTables[TABLE_RESAMPLE][c], 1, numBarriers);
	}
}

void Filter::upSampleCompute(const CommandList& commandList, XMFLOAT2 focus, float sigma, ResourceState dstState)
{
	// Up sampling
	ResourceBarrier barriers[2];
	auto numBarriers = 0u;
	commandList.SetComputePipelineLayout(m_pipelineLayouts[UP_SAMPLE_C]);
	commandList.SetPipelineState(m_pipelines[UP_SAMPLE_C]);
	commandList.SetComputeDescriptorTable(0, m_samplerTable);

	const uint8_t numPasses = m_numMips - 1;
	GaussianConstants::Immutable cb = { focus, sigma };
	commandList.SetCompute32BitConstants(2, SizeOfInUint32(cb), &cb);

	for (auto i = 0ui8; i < numPasses; ++i)
	{
		const auto c = numPasses - i;
		const auto level = c - 1;
		commandList.SetCompute32BitConstant(2, level, SizeOfInUint32(cb));
		numBarriers = m_filtered.Texture2D::Blit(commandList, barriers, 8, 8, 1, level,
			c, dstState, m_uavSrvTables[TABLE_UP_SAMPLE][i], 1, numBarriers);
	}
}
