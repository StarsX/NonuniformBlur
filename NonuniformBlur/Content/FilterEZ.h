//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#pragma once

#include "Helper/XUSG-EZ.h"

class FilterEZ
{
public:
	enum PipelineType
	{
		HYBRID,
		GRAPHICS,
		COMPUTE,

		NUM_PIPE_TYPE
	};

	FilterEZ();
	virtual ~FilterEZ();

	bool Init(XUSG::CommandList* pCommandList, std::vector<XUSG::Resource::uptr>& uploaders,
		XUSG::Format rtFormat, const char* fileName, bool typedUAV);

	void UpdateFrame(DirectX::XMFLOAT2 focus, float sigma, uint8_t frameIndex);
	void Process(XUSG::EZ::CommandList* pCommandList, uint8_t frameIndex, PipelineType pipelineType);

	XUSG::Resource* GetResult() const;
	void GetImageSize(uint32_t& width, uint32_t& height) const;

	static const uint8_t FrameCount = 3;

protected:
	enum ShaderIndex : uint8_t
	{
		VS_SCREEN_QUAD,

		PS_BLIT_2D,
		PS_UP_SAMPLE,
		PS_UP_SAMPLE_BLEND,

		CS_BLIT_2D,
		CS_UP_SAMPLE,
		CS_UP_SAMPLE_INPLACE,

		NUM_SHADER
	};

	bool loadImage(XUSG::CommandList* pCommandList, const char* fileName,
		XUSG::Texture* pTexture, XUSG::Resource* pUploader, const wchar_t* name);
	bool createShaders();

	void generateMipsGraphics(XUSG::EZ::CommandList* pCommandList);
	void generateMipsCompute(XUSG::EZ::CommandList* pCommandList);

	void upsampleGraphics(XUSG::EZ::CommandList* pCommandList, uint8_t frameIndex);
	void upsampleCompute(XUSG::EZ::CommandList* pCommandList, uint8_t frameIndex);

	XUSG::ShaderLib::uptr				m_shaderLib;
	XUSG::Blob m_shaders[NUM_SHADER];

	XUSG::Texture::uptr					m_source;
	XUSG::RenderTarget::uptr			m_filtered;

	XUSG::ConstantBuffer::uptr			m_cbPerFrame;

	DirectX::XMUINT2					m_imageSize;

	bool								m_typedUAV;
};
