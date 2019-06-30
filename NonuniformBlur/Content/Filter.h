//--------------------------------------------------------------------------------------
// By XU, Tianchen
//--------------------------------------------------------------------------------------

#pragma once

#include "DXFramework.h"
#include "Core/XUSG.h"

class Filter
{
public:
	Filter(const XUSG::Device &device);
	virtual ~Filter();

	bool Init(const XUSG::CommandList &commandList, uint32_t width, uint32_t height,
		std::shared_ptr<XUSG::ResourceBase> &source, std::vector<XUSG::Resource> &uploaders,
		const wchar_t *fileName = L"Lenna.dds");

	void Process(const XUSG::CommandList &commandList, DirectX::XMFLOAT2 focus, float sigma);
	void ProcessG(const XUSG::CommandList &commandList, DirectX::XMFLOAT2 focus, float sigma);

	XUSG::Texture2D &GetResult();

	static const uint32_t FrameCount = 3;

protected:
	enum PipelineIndex : uint8_t
	{
		RESAMPLE,
		UP_SAMPLE,
		GAUSSIAN,

		NUM_PIPELINE
	};

	enum UavSrvTableIndex : uint8_t
	{
		TABLE_DOWN_SAMPLE,
		TABLE_UP_SAMPLE,

		NUM_UAV_SRV
	};

	bool createPipelineLayouts();
	bool createPipelines();
	bool createDescriptorTables();

	XUSG::Device m_device;

	XUSG::ShaderPool				m_shaderPool;
	XUSG::Compute::PipelineCache	m_computePipelineCache;
	XUSG::PipelineLayoutCache		m_pipelineLayoutCache;
	XUSG::DescriptorTableCache		m_descriptorTableCache;

	XUSG::PipelineLayout	m_pipelineLayouts[NUM_PIPELINE];
	XUSG::Pipeline			m_pipelines[NUM_PIPELINE];

	std::vector<XUSG::DescriptorTable> m_uavSrvTables[NUM_UAV_SRV];
	XUSG::DescriptorTable	m_samplerTable;

	XUSG::Texture2D			m_filtered[NUM_UAV_SRV];

	uint8_t					m_numMips;
};
