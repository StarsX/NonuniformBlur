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

	Filter(const XUSG::Device::sptr& device);
	virtual ~Filter();

	bool Init(XUSG::CommandList* pCommandList, std::vector<XUSG::Resource::uptr>& uploaders,
		XUSG::Format rtFormat, const wchar_t* fileName, bool typedUAV);

	void UpdateFrame(DirectX::XMFLOAT2 focus, float sigma, uint8_t frameIndex);
	void Process(const XUSG::CommandList* pCommandList, uint8_t frameIndex, PipelineType pipelineType);

	XUSG::Resource* GetResult();
	void GetImageSize(uint32_t& width, uint32_t& height) const;

	static const uint8_t FrameCount = 3;

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
		uint32_t numBarriers, uint8_t frameIndex);
	void upsampleCompute(const XUSG::CommandList* pCommandList, XUSG::ResourceBarrier* pBarriers,
		uint32_t numBarriers, uint8_t frameIndex);

	XUSG::Device::sptr m_device;

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

	XUSG::ShaderResource::sptr			m_source;
	XUSG::RenderTarget::uptr			m_filtered;

	XUSG::ConstantBuffer::uptr			m_cbPerFrame;

	DirectX::XMUINT2					m_imageSize;

	bool								m_typedUAV;
};
