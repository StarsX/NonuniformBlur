//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#pragma once

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

	Filter();
	virtual ~Filter();

	bool Init(XUSG::CommandList* pCommandList, const XUSG::DescriptorTableLib::sptr& descriptorTableLib,
		std::vector<XUSG::Resource::uptr>& uploaders, XUSG::Format rtFormat, const char* fileName, bool typedUAV);

	void UpdateFrame(DirectX::XMFLOAT2 focus, float sigma, uint8_t frameIndex);
	void Process(XUSG::CommandList* pCommandList, uint8_t frameIndex, PipelineType pipelineType);

	XUSG::Resource* GetResult() const;
	void GetImageSize(uint32_t& width, uint32_t& height) const;

	static const uint8_t FrameCount = 3;

protected:
	enum PipelineIndex : uint8_t
	{
		BLIT_GRAPHICS,
		BLIT_COMPUTE,
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

	bool loadImage(XUSG::CommandList* pCommandList, const char* fileName,
		XUSG::Texture* pTexture, XUSG::Resource* pUploader, const wchar_t* name);
	bool createPipelineLayouts();
	bool createPipelines(XUSG::Format rtFormat);
	bool createDescriptorTables();

	uint32_t generateMipsGraphics(XUSG::CommandList* pCommandList, XUSG::ResourceBarrier* pBarriers);
	uint32_t generateMipsCompute(XUSG::CommandList* pCommandList, XUSG::ResourceBarrier* pBarriers);

	void upsampleGraphics(XUSG::CommandList* pCommandList, XUSG::ResourceBarrier* pBarriers,
		uint32_t numBarriers, uint8_t frameIndex);
	void upsampleCompute(XUSG::CommandList* pCommandList, XUSG::ResourceBarrier* pBarriers,
		uint32_t numBarriers, uint8_t frameIndex);

	XUSG::ShaderLib::uptr				m_shaderLib;
	XUSG::Graphics::PipelineLib::uptr	m_graphicsPipelineLib;
	XUSG::Compute::PipelineLib::uptr	m_computePipelineLib;
	XUSG::PipelineLayoutLib::uptr		m_pipelineLayoutLib;
	XUSG::DescriptorTableLib::sptr		m_descriptorTableLib;

	XUSG::PipelineLayout	m_pipelineLayouts[NUM_PIPELINE];
	XUSG::Pipeline			m_pipelines[NUM_PIPELINE];

	std::vector<XUSG::DescriptorTable>	m_uavTables[NUM_UAV_TABLE_TYPE];
	std::vector<XUSG::DescriptorTable>	m_srvTables;
	XUSG::DescriptorTable				m_samplerTable;

	XUSG::Texture::uptr					m_source;
	XUSG::RenderTarget::uptr			m_filtered;

	XUSG::ConstantBuffer::uptr			m_cbPerFrame;

	DirectX::XMUINT2					m_imageSize;

	bool								m_typedUAV;
};
