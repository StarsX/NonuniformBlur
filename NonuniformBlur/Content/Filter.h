//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
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

	bool Init(XUSG::CommandList* pCommandList, std::vector<XUSG::Resource>& uploaders,
		XUSG::Format rtFormat, const wchar_t* fileName, bool typedUAV);

	void Process(const XUSG::CommandList* pCommandList, DirectX::XMFLOAT2 focus,
		float sigma, PipelineType pipelineType);

	XUSG::ResourceBase& GetResult();
	void GetImageSize(uint32_t& width, uint32_t& height) const;

protected:
	enum PipelineIndex : uint8_t
	{
		RESAMPLE_GRAPHICS,
		RESAMPLE_COMPUTE,
		UP_SAMPLE_BLEND,
		UP_SAMPLE_INPLACE,
		UP_SAMPLE_GRAPHICS,
		UP_SAMPLE_COMPUTE,

		NUM_PIPELINE
	};

	enum UavTableType : uint8_t
	{
		UAV_TABLE_TYPED,
		UAV_TABLE_PACKED,

		NUM_UAV_TABLE_TYPE
	};

	bool createPipelineLayouts();
	bool createPipelines(XUSG::Format rtFormat);
	bool createDescriptorTables();

	uint32_t generateMipsGraphics(const XUSG::CommandList* pCommandList, XUSG::ResourceBarrier* pBarriers);
	uint32_t generateMipsCompute(const XUSG::CommandList* pCommandList, XUSG::ResourceBarrier* pBarriers);

	void upsampleGraphics(const XUSG::CommandList* pCommandList, XUSG::ResourceBarrier* pBarriers,
		uint32_t numBarriers, DirectX::XMFLOAT2 focus, float sigma);
	void upsampleCompute(const XUSG::CommandList* pCommandList, XUSG::ResourceBarrier* pBarriers,
		uint32_t numBarriers, DirectX::XMFLOAT2 focus, float sigma);

	XUSG::Device m_device;

	XUSG::ShaderPool::uptr				m_shaderPool;
	XUSG::Graphics::PipelineCache::uptr	m_graphicsPipelineCache;
	XUSG::Compute::PipelineCache::uptr	m_computePipelineCache;
	XUSG::PipelineLayoutCache::uptr		m_pipelineLayoutCache;
	XUSG::DescriptorTableCache::uptr	m_descriptorTableCache;

	XUSG::PipelineLayout	m_pipelineLayouts[NUM_PIPELINE];
	XUSG::Pipeline			m_pipelines[NUM_PIPELINE];

	std::vector<XUSG::DescriptorTable>	m_uavTables[NUM_UAV_TABLE_TYPE];
	std::vector<XUSG::DescriptorTable>	m_srvTables;
	XUSG::DescriptorTable				m_samplerTable;

	std::shared_ptr<XUSG::ResourceBase>	m_source;
	XUSG::RenderTarget::sptr			m_filtered;

	DirectX::XMUINT2					m_imageSize;
	uint8_t								m_numMips;

	bool								m_typedUAV;
};
