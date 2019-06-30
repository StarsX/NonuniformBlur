#include "stdafx.h"
#include "Filter.h"
#include "Advanced/XUSGDDSLoader.h"

using namespace std;
using namespace DirectX;
using namespace XUSG;

Filter::Filter(const Device &device) :
	m_device(device),
	m_numMips(11)
{
	m_computePipelineCache.SetDevice(device);
	m_descriptorTableCache.SetDevice(device);
	m_pipelineLayoutCache.SetDevice(device);
}

Filter::~Filter()
{
}

bool Filter::Init(const CommandList &commandList, uint32_t width, uint32_t height,
	shared_ptr<ResourceBase> &source, vector<Resource>& uploaders, const wchar_t *fileName)
{
	// Load input image
	{
		DDS::Loader textureLoader;
		DDS::AlphaMode alphaMode;

		uploaders.push_back(nullptr);
		N_RETURN(textureLoader.CreateTextureFromFile(m_device, commandList, fileName,
			8192, true, source, uploaders.back(), &alphaMode), false);
	}

	// Create resources and pipelines
	const auto viewportSize = static_cast<float>((max)(width, height));
	m_numMips = static_cast<uint8_t>(log2f(viewportSize) + 1.0f);

	for (auto &image : m_filtered)
		image.Create(m_device, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 1,
			D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, m_numMips);

	N_RETURN(createPipelineLayouts(), false);
	N_RETURN(createPipelines(), false);
	N_RETURN(createDescriptorTables(), false);

	// Copy source
	{
		const TextureCopyLocation dst(m_filtered[TABLE_DOWN_SAMPLE].GetResource().get(), 0);
		const TextureCopyLocation src(source->GetResource().get(), 0);

		ResourceBarrier barriers[2];
		auto numBarriers = m_filtered[TABLE_DOWN_SAMPLE].SetBarrier(barriers, D3D12_RESOURCE_STATE_COPY_DEST, 0, 0);
		numBarriers = source->SetBarrier(barriers, D3D12_RESOURCE_STATE_COPY_SOURCE, numBarriers, 0);
		commandList.Barrier(numBarriers, barriers);

		commandList.CopyTextureRegion(dst, 0, 0, 0, src);

		numBarriers = m_filtered[TABLE_DOWN_SAMPLE].SetBarrier(barriers,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, 0, 0);
		commandList.Barrier(numBarriers, barriers);
	}

	return true;
}

void Filter::Process(const CommandList &commandList, DirectX::XMFLOAT2 focus, float sigma)
{
	const uint8_t numPasses = m_numMips > 0 ? m_numMips - 1 : 0;
	const uint32_t width = static_cast<uint32_t>(m_filtered[TABLE_DOWN_SAMPLE].GetResource()->GetDesc().Width);
	const auto height = m_filtered[TABLE_DOWN_SAMPLE].GetResource()->GetDesc().Height;

	// Set Descriptor pools
	const DescriptorPool descriptorPools[] =
	{
		m_descriptorTableCache.GetDescriptorPool(CBV_SRV_UAV_POOL),
		m_descriptorTableCache.GetDescriptorPool(SAMPLER_POOL)
	};
	commandList.SetDescriptorPools(static_cast<uint32_t>(size(descriptorPools)), descriptorPools);

	// Generate Mips
	commandList.SetComputePipelineLayout(m_pipelineLayouts[RESAMPLE]);
	commandList.SetPipelineState(m_pipelines[RESAMPLE]);
	commandList.SetComputeDescriptorTable(0, m_samplerTable);

	ResourceBarrier barriers[2];
	auto numBarriers = 0u;
	for (auto i = 0ui8; i + 1 < numPasses; ++i)
	{
		const auto j = i + 1;
		numBarriers = m_filtered[TABLE_DOWN_SAMPLE].SetBarrier(barriers, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, numBarriers, j);
		commandList.Barrier(numBarriers, barriers);

		commandList.SetComputeDescriptorTable(1, m_uavSrvTables[TABLE_DOWN_SAMPLE][i]);
		commandList.Dispatch((max)((width >> j) / 8, 1u), (max)((height >> j) / 8, 1u), 1);
		
		numBarriers = m_filtered[TABLE_DOWN_SAMPLE].SetBarrier(barriers, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, 0, j);
	}

	if (numPasses > 0)
	{
		numBarriers = m_filtered[TABLE_UP_SAMPLE].SetBarrier(barriers, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, numBarriers, numPasses);
		commandList.Barrier(numBarriers, barriers);

		commandList.SetComputeDescriptorTable(1, m_uavSrvTables[TABLE_DOWN_SAMPLE][numPasses]);
		commandList.Dispatch(1, 1, 1);
	}

	// Up sampling
	commandList.SetComputePipelineLayout(m_pipelineLayouts[UP_SAMPLE]);
	commandList.SetPipelineState(m_pipelines[UP_SAMPLE]);
	commandList.SetComputeDescriptorTable(0, m_samplerTable);

	struct G
	{
		XMFLOAT2	Focus;
		float		Sigma;
		uint32_t	Level;
	} cb = { focus, sigma };

	for (auto i = 0ui8; i < numPasses; ++i)
	{
		const auto c = numPasses - i;
		const auto j = c - 1;
		//const auto w = computeWeight(j);
		numBarriers = m_filtered[TABLE_UP_SAMPLE].SetBarrier(barriers, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, 0, c);
		numBarriers = m_filtered[TABLE_UP_SAMPLE].SetBarrier(barriers, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, numBarriers, j);
		commandList.Barrier(numBarriers, barriers);

		cb.Level = j;
		commandList.SetComputeDescriptorTable(1, m_uavSrvTables[TABLE_UP_SAMPLE][i]);
		commandList.SetCompute32BitConstants(2, 4, &cb);
		commandList.Dispatch((max)((width >> j) / 8, 1u), (max)((height >> j) / 8, 1u), 1);
	}
}

void Filter::ProcessG(const CommandList &commandList)
{
	const uint8_t numPasses = m_numMips > 0 ? m_numMips - 1 : 0;
	const uint32_t width = static_cast<uint32_t>(m_filtered[TABLE_DOWN_SAMPLE].GetResource()->GetDesc().Width);
	const auto height = m_filtered[TABLE_DOWN_SAMPLE].GetResource()->GetDesc().Height;

	// Set Descriptor pools
	const DescriptorPool descriptorPools[] =
	{
		m_descriptorTableCache.GetDescriptorPool(CBV_SRV_UAV_POOL),
		m_descriptorTableCache.GetDescriptorPool(SAMPLER_POOL)
	};
	commandList.SetDescriptorPools(static_cast<uint32_t>(size(descriptorPools)), descriptorPools);

	// Generate Mips
	commandList.SetComputePipelineLayout(m_pipelineLayouts[RESAMPLE]);
	commandList.SetPipelineState(m_pipelines[RESAMPLE]);
	commandList.SetComputeDescriptorTable(0, m_samplerTable);

	ResourceBarrier barriers[2];
	auto numBarriers = 0u;
	for (auto i = 0ui8; i < numPasses; ++i)
	{
		const auto j = i + 1;
		numBarriers = m_filtered[TABLE_DOWN_SAMPLE].SetBarrier(barriers, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, numBarriers, j);
		commandList.Barrier(numBarriers, barriers);

		commandList.SetComputeDescriptorTable(1, m_uavSrvTables[TABLE_DOWN_SAMPLE][i]);
		commandList.Dispatch((max)((width >> j) / 8, 1u), (max)((height >> j) / 8, 1u), 1);

		numBarriers = m_filtered[TABLE_DOWN_SAMPLE].SetBarrier(barriers, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, 0, j);
	}

	numBarriers = m_filtered[TABLE_UP_SAMPLE].SetBarrier(barriers, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, numBarriers, 0);
	commandList.Barrier(numBarriers, barriers);

	// Gaussian
	struct G
	{
		float		Sigma;
		uint32_t	NumLevels;
	} cb = { 24.0f, m_numMips };
	commandList.SetComputePipelineLayout(m_pipelineLayouts[GAUSSIAN]);
	commandList.SetPipelineState(m_pipelines[GAUSSIAN]);
	commandList.SetComputeDescriptorTable(0, m_samplerTable);
	commandList.SetComputeDescriptorTable(1, m_uavSrvTables[TABLE_UP_SAMPLE][numPasses]);
	commandList.SetCompute32BitConstants(2, 2, &cb);
	commandList.Dispatch((max)(width / 8, 1u), (max)(height / 8, 1u), 1);
}

Texture2D &Filter::GetResult()
{
	return m_filtered[TABLE_UP_SAMPLE];
}

bool Filter::createPipelineLayouts()
{
	// Resampling
	{
		Util::PipelineLayout utilPipelineLayout;
		utilPipelineLayout.SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout.SetRange(1, DescriptorType::SRV, 1, 0);
		utilPipelineLayout.SetRange(1, DescriptorType::UAV, 1, 0, 0,
			D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE);
		X_RETURN(m_pipelineLayouts[RESAMPLE], utilPipelineLayout.GetPipelineLayout(
			m_pipelineLayoutCache, D3D12_ROOT_SIGNATURE_FLAG_NONE, L"ResamplingLayout"), false);
	}

	// Up sampling
	{
		Util::PipelineLayout utilPipelineLayout;
		utilPipelineLayout.SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout.SetRange(1, DescriptorType::SRV, 2, 0);
		utilPipelineLayout.SetRange(1, DescriptorType::UAV, 1, 0, 0,
			D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE);
		utilPipelineLayout.SetConstants(2, 4, 0);
		X_RETURN(m_pipelineLayouts[UP_SAMPLE], utilPipelineLayout.GetPipelineLayout(
			m_pipelineLayoutCache, D3D12_ROOT_SIGNATURE_FLAG_NONE, L"UpSamplingLayout"), false);
	}

	// Gaussian
	{
		Util::PipelineLayout utilPipelineLayout;
		utilPipelineLayout.SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout.SetRange(1, DescriptorType::SRV, 1, 0);
		utilPipelineLayout.SetRange(1, DescriptorType::UAV, 1, 0, 0,
			D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE);
		utilPipelineLayout.SetConstants(2, 2, 0);
		X_RETURN(m_pipelineLayouts[GAUSSIAN], utilPipelineLayout.GetPipelineLayout(
			m_pipelineLayoutCache, D3D12_ROOT_SIGNATURE_FLAG_NONE, L"GaussianLayout"), false);
	}

	return true;
}

bool Filter::createPipelines()
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

	// Gaussian
	{
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::CS, GAUSSIAN, L"CSMipGaussian.cso"), false);

		Compute::State state;
		state.SetPipelineLayout(m_pipelineLayouts[GAUSSIAN]);
		state.SetShader(m_shaderPool.GetShader(Shader::Stage::CS, GAUSSIAN));
		X_RETURN(m_pipelines[GAUSSIAN], state.GetPipeline(m_computePipelineCache, L"GAUSSIAN"), false);
	}

	return true;
}

bool Filter::createDescriptorTables()
{
	const uint8_t numPasses = m_numMips > 0 ? m_numMips - 1 : 0;
	m_uavSrvTables[TABLE_DOWN_SAMPLE].resize(m_numMips);
	m_uavSrvTables[TABLE_UP_SAMPLE].resize(m_numMips);
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
	}

	if (numPasses > 0)
	{
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
			const Descriptor descriptors[] =
			{
				m_filtered[TABLE_DOWN_SAMPLE].GetSRV(),
				m_filtered[TABLE_UP_SAMPLE].GetUAV()
			};
			Util::DescriptorTable utilUavSrvTable;
			utilUavSrvTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
			X_RETURN(m_uavSrvTables[TABLE_UP_SAMPLE][numPasses], utilUavSrvTable.GetCbvSrvUavTable(m_descriptorTableCache), false);
		}
	}

	// Create the sampler table
	Util::DescriptorTable samplerTable;
	const auto sampler = LINEAR_CLAMP;
	samplerTable.SetSamplers(0, 1, &sampler, m_descriptorTableCache);
	X_RETURN(m_samplerTable, samplerTable.GetSamplerTable(m_descriptorTableCache), false);

	return true;
}

float Filter::computeWeight(uint32_t mip) const
{
	const auto sigma = 24.0;//0.84089642f;

	const auto GaussianExp = [](double sigma2, double mip)
	{
		return -exp2(2.0 * mip - 1.0) / (XM_PI * sigma2);
	};

	const auto GaussianBasis = [GaussianExp](double sigma2, double mip)
	{
		return mip < 0.0 ? 0.0 : exp(GaussianExp(sigma2, mip));
	};

	const auto MipWeight = [GaussianBasis](double fSigma2, uint32_t mip, double mipStep = 1.0)
	{
		const double g[] =
		{
			GaussianBasis(fSigma2, mip),
			GaussianBasis(fSigma2, mipStep > 0 ? mip + mipStep : -1)
		};

		return static_cast<float>((1 << (2 * mip)) * (g[0] - g[1]));
	};

	auto sum = 0.0f, weight = 0.0f;
	for (auto i = mip; i < m_numMips; ++i)
	{
		const auto w = MipWeight(sigma * sigma, i);
		weight = i == mip ? w : weight;
		sum += w;
	}

	return sum > 0.0f ? weight / sum : 1.0f;
	//return mip == 6 ? 1.0f : 0.0f;
	//return mip < 6 ? 0.005f : 1.0f;//0.25f / (m_numMips - mip) : 1.0f;
}
