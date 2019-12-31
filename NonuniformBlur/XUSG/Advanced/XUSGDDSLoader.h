//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#pragma once

#include "Core/XUSGResource.h"

namespace XUSG
{
	namespace DDS
	{
		enum AlphaMode : uint8_t
		{
			ALPHA_MODE_UNKNOWN,
			ALPHA_MODE_STRAIGHT,
			ALPHA_MODE_PREMULTIPLIED,
			ALPHA_MODE_OPAQUE,
			ALPHA_MODE_CUSTOM
		};

		class Loader
		{
		public:
			Loader();
			virtual ~Loader();

			bool CreateTextureFromMemory(const Device& device, const CommandList& commandList, const uint8_t* ddsData,
				size_t ddsDataSize, size_t maxsize, bool forceSRGB, std::shared_ptr<ResourceBase>& texture,
				Resource& uploader, AlphaMode* alphaMode = nullptr, ResourceState state = ResourceState::COMMON);

			bool CreateTextureFromFile(const Device& device, const CommandList& commandList, const wchar_t* fileName,
				size_t maxsize, bool forceSRGB, std::shared_ptr<ResourceBase>& texture, Resource& uploader,
				AlphaMode* alphaMode = nullptr, ResourceState state = ResourceState::COMMON);

			static size_t BitsPerPixel(Format fmt);
		};
	}
}
