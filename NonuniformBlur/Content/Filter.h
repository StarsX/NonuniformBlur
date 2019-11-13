//--------------------------------------------------------------------------------------
// By XU, Tianchen
//--------------------------------------------------------------------------------------

#pragma once

#include "DXFramework.h"
#include "Core/XUSG.h"

class Filter
{
public:
	enum PipelineType
	{
		HYBRID,
		GRAPHICS,
		COMPUTE,

		NUM_PIPE_TYPE
	};

	Filter(const XUSG::Device& device);
	virtual ~Filter();

	bool Init(const XUSG::CommandList& commandList, std::vector<XUSG::Resource>& uploaders,
		XUSG::Format rtFormat, const wchar_t* fileName, bool typedUAV);

	void Process(const XUSG::CommandList& commandList, DirectX::XMFLOAT2 focus,
		float sigma, PipelineType pipelineType);

	XUSG::ResourceBase& GetResult();
	void GetImageSize(uint32_t& width, uint32_t& height) const;

protected:
	enum PipelineIndex : uint8_t
	{
		RESAMPLE_G,
		RESAMPLE_C,
		UP_SAMPLE_G,
		UP_SAMPLE_C,
		FINAL_G,
		FINAL_C,

		NUM_PIPELINE
	};

	bool createPipelineLayouts();
	bool createPipelines(XUSG::Format rtFormat, bool typedUAV);
	bool createDescriptorTables();

	uint32_t generateMipsGraphics(const XUSG::CommandList& commandList, XUSG::ResourceBarrier* pBarriers);
	uint32_t generateMipsCompute(const XUSG::CommandList& commandList, XUSG::ResourceBarrier* pBarriers);

	void upsampleGraphics(const XUSG::CommandList& commandList, XUSG::ResourceBarrier* pBarriers,
		uint32_t numBarriers, DirectX::XMFLOAT2 focus, float sigma);
	void upsampleCompute(const XUSG::CommandList& commandList, XUSG::ResourceBarrier* pBarriers,
		uint32_t numBarriers, DirectX::XMFLOAT2 focus, float sigma);

	XUSG::Device m_device;

	XUSG::ShaderPool				m_shaderPool;
	XUSG::Graphics::PipelineCache	m_graphicsPipelineCache;
	XUSG::Compute::PipelineCache	m_computePipelineCache;
	XUSG::PipelineLayoutCache		m_pipelineLayoutCache;
	XUSG::DescriptorTableCache		m_descriptorTableCache;

	XUSG::PipelineLayout	m_pipelineLayouts[NUM_PIPELINE];
	XUSG::Pipeline			m_pipelines[NUM_PIPELINE];

	std::vector<XUSG::DescriptorTable> m_uavTables;
	std::vector<XUSG::DescriptorTable> m_srvTables;
	XUSG::DescriptorTable	m_samplerTable;

	std::shared_ptr<XUSG::ResourceBase> m_source;
	XUSG::RenderTarget		m_filtered;

	DirectX::XMUINT2		m_imageSize;
	uint8_t					m_numMips;
};
