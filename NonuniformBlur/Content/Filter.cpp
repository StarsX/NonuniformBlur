#include "stdafx.h"
#include "Filter.h"
#include "Advanced/XUSGDDSLoader.h"

#define SizeOfInUint32(obj)	DIV_UP(sizeof(obj), sizeof(uint32_t))

using namespace std;
using namespace DirectX;
using namespace XUSG;

struct GaussianConstants
{
	XMFLOAT2	Focus;
	float		Sigma;
	uint32_t	Level;
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

bool Filter::Init(const CommandList& commandList, DescriptorTable& uavSrvTable,
	vector<Resource>& uploaders, Format rtFormat, const wchar_t* fileName)
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

	for (auto& image : m_filtered)
		image.Create(m_device, m_imageSize.x, m_imageSize.y, rtFormat, 1,
			ResourceFlag::ALLOW_UNORDERED_ACCESS, m_numMips);

	m_pyramid.Create(m_device, m_imageSize.x, m_imageSize.y, rtFormat, 1,
		ResourceFlag::ALLOW_UNORDERED_ACCESS, m_numMips, 1, ResourceState::UNORDERED_ACCESS);

	N_RETURN(createPipelineLayouts(), false);
	N_RETURN(createPipelines(rtFormat), false);
	N_RETURN(createDescriptorTables(), false);

	return true;
}

void Filter::Process(const CommandList& commandList, XMFLOAT2 focus, float sigma, ResourceState dstState)
{
	const uint8_t numPasses = m_numMips - 1;

	// Set Descriptor pools
	const DescriptorPool descriptorPools[] =
	{
		m_descriptorTableCache.GetDescriptorPool(CBV_SRV_UAV_POOL),
		m_descriptorTableCache.GetDescriptorPool(SAMPLER_POOL)
	};
	commandList.SetDescriptorPools(static_cast<uint32_t>(size(descriptorPools)), descriptorPools);

	// Copy source
	ResourceBarrier barriers[2];
	auto numBarriers = m_filtered[TABLE_DOWN_SAMPLE].SetBarrier(barriers, ResourceState::UNORDERED_ACCESS, 0, 0);
	{
		commandList.SetComputePipelineLayout(m_pipelineLayouts[RESAMPLE]);
		commandList.SetPipelineState(m_pipelines[RESAMPLE]);
		commandList.SetComputeDescriptorTable(0, m_samplerTable);

		commandList.Barrier(numBarriers, barriers);
		m_filtered[TABLE_DOWN_SAMPLE].Blit(commandList, 8, 8, 1, m_uavSrvTables[TABLE_COPY][1], 1);
	}

	// Generate Mips
	numBarriers = 0;
	if (numPasses > 0) numBarriers = m_filtered[TABLE_DOWN_SAMPLE].GenerateMips(commandList, barriers,
		8, 8, 1, ResourceState::NON_PIXEL_SHADER_RESOURCE, m_pipelineLayouts[RESAMPLE],
		m_pipelines[RESAMPLE], m_uavSrvTables[TABLE_DOWN_SAMPLE].data(), 1, m_samplerTable,
		0, numBarriers, nullptr, 0, 1, numPasses - 1);
	numBarriers = m_filtered[TABLE_UP_SAMPLE].SetBarrier(barriers, ResourceState::UNORDERED_ACCESS, numBarriers);
	commandList.Barrier(numBarriers, barriers);

	commandList.SetComputeDescriptorTable(1, m_uavSrvTables[TABLE_DOWN_SAMPLE][numPasses]);
	commandList.Dispatch(1, 1, 1);

	// Up sampling
	numBarriers = 0;
	commandList.SetComputePipelineLayout(m_pipelineLayouts[UP_SAMPLE]);
	commandList.SetPipelineState(m_pipelines[UP_SAMPLE]);
	commandList.SetComputeDescriptorTable(0, m_samplerTable);

	GaussianConstants cb = { focus, sigma };
	for (auto i = 0ui8; i < numPasses; ++i)
	{
		const auto c = numPasses - i;
		cb.Level = c - 1;
		commandList.SetCompute32BitConstants(2, SizeOfInUint32(GaussianConstants), &cb);
		numBarriers = m_filtered[TABLE_UP_SAMPLE].Blit(commandList, barriers, 8, 8, 1,
			cb.Level, c, dstState, m_uavSrvTables[TABLE_UP_SAMPLE][i], 1, numBarriers);
	}
}

void Filter::Process(const CommandList& commandList, XMFLOAT2 focus, float sigma)
{
	const uint8_t numPasses = m_numMips - 1;

	// Set Descriptor pools
	const DescriptorPool descriptorPools[] =
	{
		m_descriptorTableCache.GetDescriptorPool(CBV_SRV_UAV_POOL),
		m_descriptorTableCache.GetDescriptorPool(SAMPLER_POOL)
	};
	commandList.SetDescriptorPools(static_cast<uint32_t>(size(descriptorPools)), descriptorPools);

	// Copy source
	ResourceBarrier barriers[2];
	auto numBarriers = m_pyramid.SetBarrier(barriers, ResourceState::UNORDERED_ACCESS, 0, 0);
	{
		commandList.SetComputePipelineLayout(m_pipelineLayouts[RESAMPLE]);
		commandList.SetPipelineState(m_pipelines[RESAMPLE]);
		commandList.SetComputeDescriptorTable(0, m_samplerTable);

		commandList.Barrier(numBarriers, barriers);
		m_pyramid.Texture2D::Blit(commandList, 8, 8, 1, m_uavSrvTables[TABLE_COPY][0], 1);
	}

	// Generate Mips
	numBarriers = 0;
	numBarriers = m_pyramid.Texture2D::GenerateMips(commandList, barriers,
		8, 8, 1, ResourceState::PIXEL_SHADER_RESOURCE | ResourceState::NON_PIXEL_SHADER_RESOURCE,
		m_pipelineLayouts[RESAMPLE], m_pipelines[RESAMPLE], m_uavSrvTables[TABLE_RESAMPLE].data(),
		1, m_samplerTable, 0, numBarriers, nullptr, 0, 1, numPasses);
	commandList.Barrier(numBarriers, barriers);

	// Up sampling
	numBarriers = 0;
	commandList.SetGraphicsPipelineLayout(m_pipelineLayouts[UP_SAMPLE_G]);
	commandList.SetPipelineState(m_pipelines[UP_SAMPLE_G]);
	commandList.SetGraphicsDescriptorTable(0, m_samplerTable);

	GaussianConstants cb = { focus, sigma };
	for (auto i = 0ui8; i < numPasses; ++i)
	{
		const auto c = numPasses - i;
		cb.Level = c - 1;
		commandList.SetGraphics32BitConstants(2, SizeOfInUint32(GaussianConstants), &cb);
		numBarriers = m_pyramid.Blit(commandList, barriers, cb.Level, c, ResourceState::PIXEL_SHADER_RESOURCE,
			m_uavSrvTables[TABLE_RESAMPLE][c], 1, numBarriers);
	}
}

Texture2D& Filter::GetResult()
{
	return m_filtered[TABLE_UP_SAMPLE];
}

Texture2D& Filter::GetResultG()
{
	return m_pyramid;
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

	// Up sampling
	{
		Util::PipelineLayout utilPipelineLayout;
		utilPipelineLayout.SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout.SetRange(1, DescriptorType::SRV, 2, 0);
		utilPipelineLayout.SetRange(1, DescriptorType::UAV, 1, 0, 0,
			DescriptorRangeFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		utilPipelineLayout.SetConstants(2, SizeOfInUint32(GaussianConstants), 0);
		X_RETURN(m_pipelineLayouts[UP_SAMPLE], utilPipelineLayout.GetPipelineLayout(
			m_pipelineLayoutCache, PipelineLayoutFlag::NONE, L"UpSamplingLayout"), false);
	}

	// Up sampling blended
	{
		Util::PipelineLayout utilPipelineLayout;
		utilPipelineLayout.SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout.SetRange(1, DescriptorType::SRV, 1, 0);
		utilPipelineLayout.SetConstants(2, SizeOfInUint32(GaussianConstants), 0);
		utilPipelineLayout.SetShaderStage(0, Shader::PS);
		utilPipelineLayout.SetShaderStage(1, Shader::PS);
		utilPipelineLayout.SetShaderStage(2, Shader::PS);
		X_RETURN(m_pipelineLayouts[UP_SAMPLE_G], utilPipelineLayout.GetPipelineLayout(
			m_pipelineLayoutCache, PipelineLayoutFlag::NONE, L"UpSamplingBlendedLayout"), false);
	}

	return true;
}

bool Filter::createPipelines(Format rtFormat)
{
	// Resampling
	{
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::CS, RESAMPLE, L"CSResample.cso"), false);

		Compute::State state;
		state.SetPipelineLayout(m_pipelineLayouts[RESAMPLE]);
		state.SetShader(m_shaderPool.GetShader(Shader::Stage::CS, RESAMPLE));
		X_RETURN(m_pipelines[RESAMPLE], state.GetPipeline(m_computePipelineCache, L"Resampling"), false);
	}

	// Up sampling
	{
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::CS, UP_SAMPLE, L"CSUpSample.cso"), false);

		Compute::State state;
		state.SetPipelineLayout(m_pipelineLayouts[UP_SAMPLE]);
		state.SetShader(m_shaderPool.GetShader(Shader::Stage::CS, UP_SAMPLE));
		X_RETURN(m_pipelines[UP_SAMPLE], state.GetPipeline(m_computePipelineCache, L"UpSampling"), false);
	}

	// Up sampling blended
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
		X_RETURN(m_pipelines[UP_SAMPLE_G], state.GetPipeline(m_graphicsPipelineCache, L"UpSamplingBlended"), false);
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
			m_pyramid.GetUAV()
		};
		Util::DescriptorTable utilUavSrvTable;
		utilUavSrvTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		X_RETURN(m_uavSrvTables[TABLE_COPY][0], utilUavSrvTable.GetCbvSrvUavTable(m_descriptorTableCache), false);
	}
	{
		const Descriptor descriptors[] =
		{
			m_source->GetSRV(),
			m_filtered[TABLE_DOWN_SAMPLE].GetUAV()
		};
		Util::DescriptorTable utilUavSrvTable;
		utilUavSrvTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		X_RETURN(m_uavSrvTables[TABLE_COPY][1], utilUavSrvTable.GetCbvSrvUavTable(m_descriptorTableCache), false);
	}

	const uint8_t numPasses = m_numMips > 0 ? m_numMips - 1 : 0;
	m_uavSrvTables[TABLE_DOWN_SAMPLE].resize(m_numMips);
	m_uavSrvTables[TABLE_UP_SAMPLE].resize(m_numMips);
	m_uavSrvTables[TABLE_RESAMPLE].resize(m_numMips);
	for (auto i = 0ui8; i < numPasses; ++i)
	{
		// Get UAV and SRVs
		{
			const Descriptor descriptors[] =
			{
				m_filtered[TABLE_DOWN_SAMPLE].GetSRVLevel(i),
				m_filtered[TABLE_DOWN_SAMPLE].GetUAV(i + 1)
			};
			Util::DescriptorTable utilUavSrvTable;
			utilUavSrvTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
			X_RETURN(m_uavSrvTables[TABLE_DOWN_SAMPLE][i], utilUavSrvTable.GetCbvSrvUavTable(m_descriptorTableCache), false);
		}

		{
			const auto coarser = numPasses - i;
			const auto current = coarser - 1;
			const Descriptor descriptors[] =
			{
				m_filtered[TABLE_DOWN_SAMPLE].GetSRVLevel(current),
				m_filtered[TABLE_UP_SAMPLE].GetSRVLevel(coarser),
				m_filtered[TABLE_UP_SAMPLE].GetUAV(current)
			};
			Util::DescriptorTable utilUavSrvTable;
			utilUavSrvTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
			X_RETURN(m_uavSrvTables[TABLE_UP_SAMPLE][i], utilUavSrvTable.GetCbvSrvUavTable(m_descriptorTableCache), false);
		}

		// Get UAV and SRVs
		{
			const Descriptor descriptors[] =
			{
				m_pyramid.GetSRVLevel(i),
				m_pyramid.GetUAV(i + 1)
			};
			Util::DescriptorTable utilUavSrvTable;
			utilUavSrvTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
			X_RETURN(m_uavSrvTables[TABLE_RESAMPLE][i], utilUavSrvTable.GetCbvSrvUavTable(m_descriptorTableCache), false);
		}
	}

	// Get UAV and SRVs for the final-time down sampling
	{
		const Descriptor descriptors[] =
		{
			m_filtered[TABLE_DOWN_SAMPLE].GetSRVLevel(numPasses - 1),
			m_filtered[TABLE_UP_SAMPLE].GetUAV(numPasses)
		};
		Util::DescriptorTable utilUavSrvTable;
		utilUavSrvTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		X_RETURN(m_uavSrvTables[TABLE_DOWN_SAMPLE][numPasses], utilUavSrvTable.GetCbvSrvUavTable(m_descriptorTableCache), false);
	}

	{
		Util::DescriptorTable utilUavSrvTable;
		utilUavSrvTable.SetDescriptors(0, 1, &m_pyramid.GetSRVLevel(numPasses));
		X_RETURN(m_uavSrvTables[TABLE_RESAMPLE][numPasses], utilUavSrvTable.GetCbvSrvUavTable(m_descriptorTableCache), false);
	}

	// Get UAV and SRVs for direct Gaussian
	{
		const Descriptor descriptors[] =
		{
			m_filtered[TABLE_DOWN_SAMPLE].GetSRV(),
			m_filtered[TABLE_UP_SAMPLE].GetUAV()
		};
		Util::DescriptorTable utilUavSrvTable;
		utilUavSrvTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		X_RETURN(m_uavSrvTables[TABLE_UP_SAMPLE][numPasses], utilUavSrvTable.GetCbvSrvUavTable(m_descriptorTableCache), false);
	}

	// Create the sampler table
	Util::DescriptorTable samplerTable;
	const auto sampler = LINEAR_CLAMP;
	samplerTable.SetSamplers(0, 1, &sampler, m_descriptorTableCache);
	X_RETURN(m_samplerTable, samplerTable.GetSamplerTable(m_descriptorTableCache), false);

	return true;
}
