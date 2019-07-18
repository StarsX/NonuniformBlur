//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

#include "DXFrameworkHelper.h"
#include "XUSGResource.h"

#define REMOVE_PACKED_UAV	ResourceFlags(~0x8000)

using namespace std;
using namespace XUSG;

Format MapToPackedFormat(Format& format)
{
	auto formatUAV = DXGI_FORMAT_R32_UINT;

	switch (format)
	{
	case DXGI_FORMAT_R10G10B10A2_UNORM:
	case DXGI_FORMAT_R10G10B10A2_UINT:
		format = DXGI_FORMAT_R10G10B10A2_TYPELESS;
		break;
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
	case DXGI_FORMAT_R8G8B8A8_UINT:
	case DXGI_FORMAT_R8G8B8A8_SNORM:
	case DXGI_FORMAT_R8G8B8A8_SINT:
		format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
		break;
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
		break;
	case DXGI_FORMAT_B8G8R8X8_UNORM:
	case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
		format = DXGI_FORMAT_B8G8R8X8_TYPELESS;
		break;
	case DXGI_FORMAT_R16G16_FLOAT:
	case DXGI_FORMAT_R16G16_UNORM:
	case DXGI_FORMAT_R16G16_UINT:
	case DXGI_FORMAT_R16G16_SNORM:
	case DXGI_FORMAT_R16G16_SINT:
		format = DXGI_FORMAT_R16G16_TYPELESS;
		break;
	default:
		formatUAV = format;
	}

	return formatUAV;
}

//--------------------------------------------------------------------------------------
// Constant buffer
//--------------------------------------------------------------------------------------

ConstantBuffer::ConstantBuffer() :
	m_device(nullptr),
	m_resource(nullptr),
	m_cbvPools(0),
	m_cbvs(0),
	m_cbvOffsets(0),
	m_pDataBegin(nullptr)
{
}

ConstantBuffer::~ConstantBuffer()
{
	if (m_resource) Unmap();
}

bool ConstantBuffer::Create(const Device& device, uint64_t byteWidth, uint32_t numCBVs,
	const uint32_t* offsets, MemoryType memoryType, const wchar_t* name)
{
	M_RETURN(!device, cerr, "The device is NULL.", false);
	m_device = device;

	// Instanced CBVs
	vector<uint32_t> offsetList;
	if (!offsets)
	{
		auto numBytes = 0u;
		// CB size is required to be D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT-byte aligned.
		auto cbvSize = static_cast<uint32_t>(byteWidth / numCBVs);
		cbvSize = CalculateConstantBufferByteSize(cbvSize);
		offsetList.resize(numCBVs);

		for (auto& offset : offsetList)
		{
			offset = numBytes;
			numBytes += cbvSize;
		}

		offsets = offsetList.data();
		byteWidth = cbvSize * numCBVs;
	}

	const auto strideCbv = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	V_RETURN(m_device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(memoryType),
		D3D12_HEAP_FLAG_NONE,
		&CD3DX12_RESOURCE_DESC::Buffer(byteWidth, D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE),
		memoryType == D3D12_HEAP_TYPE_DEFAULT ?
		D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER :
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&m_resource)), clog, false);
	if (name) m_resource->SetName((wstring(name) + L".Resource").c_str());

	// Describe and create a constant buffer view.
	D3D12_CONSTANT_BUFFER_VIEW_DESC desc;

	m_cbvs.resize(numCBVs);
	m_cbvOffsets.resize(numCBVs);
	const auto maxSize = static_cast<uint32_t>(byteWidth);
	for (auto i = 0u; i < numCBVs; ++i)
	{
		const auto& offset = offsets[i];
		desc.BufferLocation = m_resource->GetGPUVirtualAddress() + offset;
		desc.SizeInBytes = (i + 1 >= numCBVs ? maxSize : offsets[i + 1]) - offset;

		m_cbvOffsets[i] = offset;

		// Create a constant buffer view
		m_cbvs[i] = allocateCbvPool(name);
		m_device->CreateConstantBufferView(&desc, m_cbvs[i]);
	}

	return true;
}

bool ConstantBuffer::Upload(const CommandList& commandList, Resource& uploader,
	const void* pData, size_t size, uint32_t i)
{
	const auto offset = m_cbvOffsets.empty() ? 0 : m_cbvOffsets[i];
	SubresourceData subresourceData;
	subresourceData.pData = pData;
	subresourceData.RowPitch = static_cast<uint32_t>(size);
	subresourceData.SlicePitch = static_cast<uint32_t>(m_resource->GetDesc().Width);

	// Create the GPU upload buffer.
	if (!uploader)
	{
		const auto cbSize = static_cast<uint32_t>(size) + offset;
		V_RETURN(m_device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
			D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(CalculateConstantBufferByteSize(cbSize)),
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&uploader)), clog, false);
	}

	// Copy data to the intermediate upload heap and then schedule a copy 
	// from the upload heap to the buffer.
	commandList.Barrier(1, &ResourceBarrier::Transition(m_resource.get(),
		D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_COPY_DEST));
	M_RETURN(UpdateSubresources(const_cast<CommandList&>(commandList).GetCommandList().get(),
		m_resource.get(), uploader.get(), offset, 0, 1, &subresourceData) <= 0,
		clog, "Failed to upload the resource.", false);
	commandList.Barrier(1, &ResourceBarrier::Transition(m_resource.get(),
		D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER));

	return true;
}

void* ConstantBuffer::Map(uint32_t i)
{
	if (m_pDataBegin == nullptr)
	{
		// Map and initialize the constant buffer. We don't unmap this until the
		// app closes. Keeping things mapped for the lifetime of the resource is okay.
		CD3DX12_RANGE readRange(0, 0);	// We do not intend to read from this resource on the CPU.
		V_RETURN(m_resource->Map(0, &readRange, &m_pDataBegin), cerr, false);
	}

	return &reinterpret_cast<uint8_t*>(m_pDataBegin)[m_cbvOffsets[i]];
}

void ConstantBuffer::Unmap()
{
	if (m_pDataBegin)
	{
		m_resource->Unmap(0, nullptr);
		m_pDataBegin = nullptr;
	}
}

const Resource& ConstantBuffer::GetResource() const
{
	return m_resource;
}

Descriptor ConstantBuffer::GetCBV(uint32_t i) const
{
	return m_cbvs.size() > i ? m_cbvs[i] : Descriptor(D3D12_DEFAULT);
}

Descriptor ConstantBuffer::allocateCbvPool(const wchar_t* name)
{
	m_cbvPools.push_back(DescriptorPool());
	auto& cbvPool = m_cbvPools.back();

	D3D12_DESCRIPTOR_HEAP_DESC desc = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1 };
	V_RETURN(m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&cbvPool)), cerr, Descriptor(D3D12_DEFAULT));
	if (name) cbvPool->SetName((wstring(name) + L".CbvPool").c_str());

	return Descriptor(cbvPool->GetCPUDescriptorHandleForHeapStart());
}

//--------------------------------------------------------------------------------------
// Resource base
//--------------------------------------------------------------------------------------

ResourceBase::ResourceBase() :
	m_device(nullptr),
	m_resource(nullptr),
	m_srvUavPools(0),
	m_srvs(0),
	m_states()
{
}

ResourceBase::~ResourceBase()
{
}

uint32_t ResourceBase::SetBarrier(ResourceBarrier* pBarriers, ResourceState dstState,
	uint32_t numBarriers, uint32_t subresource, BarrierFlags flags)
{
	const auto& state = m_states[subresource == 0xffffffff ? 0 : subresource];
	if (state != dstState || dstState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
		pBarriers[numBarriers++] = Transition(dstState, subresource, flags);

	return numBarriers;
}

const Resource& ResourceBase::GetResource() const
{
	return m_resource;
}

Descriptor ResourceBase::GetSRV(uint32_t i) const
{
	return m_srvs.size() > i ? m_srvs[i] : Descriptor(D3D12_DEFAULT);
}

ResourceBarrier ResourceBase::Transition(ResourceState dstState,
	uint32_t subresource, BarrierFlags flags)
{
	ResourceState srcState;
	if (subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
	{
		srcState = m_states[0];
		if (flags != D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY)
			for (auto& state : m_states) state = dstState;
	}
	else
	{
		srcState = m_states[subresource];
		m_states[subresource] = flags == D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY ? srcState : dstState;
	}

	return srcState == dstState && dstState == D3D12_RESOURCE_STATE_UNORDERED_ACCESS ?
		ResourceBarrier::UAV(m_resource.get()) :
		ResourceBarrier::Transition(m_resource.get(), srcState, dstState, subresource, flags);
}

ResourceState ResourceBase::GetResourceState(uint32_t i) const
{
	return m_states[i];
}

void ResourceBase::setDevice(const Device& device)
{
	m_device = device;
}

Descriptor ResourceBase::allocateSrvUavPool()
{
	m_srvUavPools.push_back(DescriptorPool());
	auto& srvUavPool = m_srvUavPools.back();

	D3D12_DESCRIPTOR_HEAP_DESC desc = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1 };
	V_RETURN(m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&srvUavPool)), cerr, Descriptor(D3D12_DEFAULT));
	if (!m_name.empty()) srvUavPool->SetName((m_name + L".SrvUavPool").c_str());

	return Descriptor(srvUavPool->GetCPUDescriptorHandleForHeapStart());
}

//--------------------------------------------------------------------------------------
// 2D Texture
//--------------------------------------------------------------------------------------

Texture2D::Texture2D() :
	ResourceBase(),
	m_counter(nullptr),
	m_uavs(0),
	m_srvLevels(0)
{
}

Texture2D::~Texture2D()
{
}

bool Texture2D::Create(const Device& device, uint32_t width, uint32_t height, Format format,
	uint32_t arraySize, ResourceFlags resourceFlags, uint8_t numMips, uint8_t sampleCount,
	MemoryType memoryType, ResourceState state, bool isCubeMap, const wchar_t* name)
{
	M_RETURN(!device, cerr, "The device is NULL.", false);
	setDevice(device);

	if (name) m_name = name;

	const auto isPacked = (resourceFlags & BIND_PACKED_UAV) == BIND_PACKED_UAV;
	resourceFlags &= REMOVE_PACKED_UAV;

	const auto hasSRV = !(resourceFlags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
	const bool hasUAV = resourceFlags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	// Map formats
	auto formatResource = format;
	const auto formatUAV = isPacked ? MapToPackedFormat(formatResource) : format;

	// Setup the texture description.
	const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(formatResource, width, height, arraySize,
		numMips, sampleCount, 0, resourceFlags);

	// Determine initial state
	m_states.resize(arraySize * numMips);
	if (state) for (auto& initState : m_states) initState = state;
	else for (auto& initState : m_states)
	{
		initState = hasSRV ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON;
		initState = hasUAV ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : initState;
	}

	V_RETURN(m_device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(memoryType),
		D3D12_HEAP_FLAG_NONE, &desc, m_states[0], nullptr, IID_PPV_ARGS(&m_resource)), clog, false);
	if (!m_name.empty()) m_resource->SetName((m_name + L".Resource").c_str());

	// Create SRV
	if (hasSRV) N_RETURN(CreateSRVs(arraySize, format, numMips, sampleCount, isCubeMap), false);

	// Create UAVs
	if (hasUAV) N_RETURN(CreateUAVs(arraySize, formatUAV, numMips), false);

	// Create SRV for each level
	if (hasSRV && hasUAV) N_RETURN(CreateSRVLevels(arraySize, numMips, format, sampleCount, isCubeMap), false);

	return true;
}

bool Texture2D::Upload(const CommandList& commandList, Resource& uploader,
	SubresourceData* pSubresourceData, uint32_t numSubresources,
	ResourceState dstState, uint32_t i)
{
	N_RETURN(pSubresourceData, false);

	// Create the GPU upload buffer.
	if (!uploader)
	{
		const auto uploadBufferSize = GetRequiredIntermediateSize(m_resource.get(), i, numSubresources);
		V_RETURN(m_device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
			D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize),
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&uploader)), clog, false);
		if (!m_name.empty()) uploader->SetName((m_name + L".UploaderResource").c_str());
	}

	// Copy data to the intermediate upload heap and then schedule a copy 
	// from the upload heap to the Texture2D.
	ResourceBarrier barrier;
	dstState = dstState ? dstState : m_states[0];
	auto numBarriers = SetBarrier(&barrier, D3D12_RESOURCE_STATE_COPY_DEST);
	commandList.Barrier(numBarriers, &barrier);
	M_RETURN(UpdateSubresources(const_cast<CommandList&>(commandList).GetCommandList().get(),
		m_resource.get(), uploader.get(), 0, i, numSubresources, pSubresourceData) <= 0,
		clog, "Failed to upload the resource.", false);
	numBarriers = SetBarrier(&barrier, dstState);
	commandList.Barrier(numBarriers, &barrier);

	return true;
}

bool Texture2D::Upload(const CommandList& commandList, Resource& uploader,
	const void* pData, uint8_t stride, ResourceState dstState)
{
	const auto desc = m_resource->GetDesc();

	SubresourceData subresourceData;
	subresourceData.pData = pData;
	subresourceData.RowPitch = stride * static_cast<uint32_t>(desc.Width);
	subresourceData.SlicePitch = subresourceData.RowPitch * desc.Height;

	return Upload(commandList, uploader, &subresourceData, 1, dstState);
}

bool Texture2D::CreateSRVs(uint32_t arraySize, Format format, uint8_t numMips,
	uint8_t sampleCount, bool isCubeMap)
{
	// Setup the description of the shader resource view.
	D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
	desc.Format = format ? format : m_resource->GetDesc().Format;
	desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

	auto mipLevel = 0ui8;
	m_srvs.resize(sampleCount > 1 ? 1 : (max)(numMips, 1ui8));

	for (auto& descriptor : m_srvs)
	{
		if (isCubeMap)
		{
			assert(arraySize % 6 == 0);
			if (arraySize > 6)
			{
				desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
				desc.TextureCubeArray.MipLevels = numMips - mipLevel;
				desc.TextureCubeArray.MostDetailedMip = mipLevel++;
				desc.TextureCubeArray.NumCubes = arraySize / 6;
			}
			else
			{
				desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
				desc.TextureCube.MipLevels = numMips - mipLevel;
				desc.TextureCube.MostDetailedMip = mipLevel++;
			}
		}
		else if (arraySize > 1)
		{
			if (sampleCount > 1)
			{
				desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
				desc.Texture2DMSArray.ArraySize = arraySize;
			}
			else
			{
				desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
				desc.Texture2DArray.ArraySize = arraySize;
				desc.Texture2DArray.MipLevels = numMips - mipLevel;
				desc.Texture2DArray.MostDetailedMip = mipLevel++;
			}
		}
		else
		{
			if (sampleCount > 1)
				desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
			else
			{
				desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				desc.Texture2D.MipLevels = numMips - mipLevel;
				desc.Texture2D.MostDetailedMip = mipLevel++;
			}
		}

		// Create a shader resource view
		descriptor = allocateSrvUavPool();
		N_RETURN(descriptor.ptr, false);
		m_device->CreateShaderResourceView(m_resource.get(), &desc, descriptor);
	}

	return true;
}

bool Texture2D::CreateSRVLevels(uint32_t arraySize, uint8_t numMips, Format format,
	uint8_t sampleCount, bool isCubeMap)
{
	if (numMips > 1)
	{
		// Setup the description of the shader resource view.
		D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
		desc.Format = format ? format : m_resource->GetDesc().Format;
		desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

		auto mipLevel = 0ui8;
		m_srvLevels.resize(numMips);

		for (auto& descriptor : m_srvLevels)
		{
			// Setup the description of the shader resource view.
			if (isCubeMap)
			{
				assert(arraySize % 6 == 0);
				if (arraySize > 6)
				{
					desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
					desc.TextureCubeArray.MostDetailedMip = mipLevel++;
					desc.TextureCubeArray.MipLevels = 1;
					desc.TextureCubeArray.NumCubes = arraySize / 6;
				}
				else
				{
					desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
					desc.TextureCube.MostDetailedMip = mipLevel++;
					desc.TextureCube.MipLevels = 1;
				}
			}
			else if (arraySize > 1)
			{
				desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
				desc.Texture2DArray.MostDetailedMip = mipLevel++;
				desc.Texture2DArray.MipLevels = 1;
			}
			else
			{
				desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				desc.Texture2D.MostDetailedMip = mipLevel++;
				desc.Texture2D.MipLevels = 1;
			}

			// Create a shader resource view
			descriptor = allocateSrvUavPool();
			N_RETURN(descriptor.ptr, false);
			m_device->CreateShaderResourceView(m_resource.get(), &desc, descriptor);
		}
	}

	return true;
}

bool Texture2D::CreateUAVs(uint32_t arraySize, Format format, uint8_t numMips)
{
	// Setup the description of the unordered access view.
	D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
	desc.Format = format ? format : m_resource->GetDesc().Format;

	auto mipLevel = 0ui8;
	m_uavs.resize((max)(numMips, 1ui8));

	for (auto& descriptor : m_uavs)
	{
		// Setup the description of the unordered access view.
		if (arraySize > 1)
		{
			desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
			desc.Texture2DArray.ArraySize = arraySize;
			desc.Texture2DArray.MipSlice = mipLevel++;
		}
		else
		{
			desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			desc.Texture2D.MipSlice = mipLevel++;
		}

		// Create an unordered access view
		descriptor = allocateSrvUavPool();
		N_RETURN(descriptor.ptr, false);
		m_device->CreateUnorderedAccessView(m_resource.get(), m_counter.get(), &desc, descriptor);
	}

	return true;
}

uint32_t Texture2D::SetBarrier(ResourceBarrier* pBarriers, ResourceState dstState,
	uint32_t numBarriers, uint32_t subresource, BarrierFlags flags)
{
	return ResourceBase::SetBarrier(pBarriers, dstState, numBarriers, subresource, flags);
}

uint32_t Texture2D::SetBarrier(ResourceBarrier* pBarriers, uint8_t mipLevel, ResourceState dstState,
	uint32_t numBarriers, uint32_t slice, BarrierFlags flags)
{
	const auto& desc = m_resource->GetDesc();
	const auto subresource = D3D12CalcSubresource(mipLevel, slice, 0, desc.MipLevels, desc.DepthOrArraySize);

	return SetBarrier(pBarriers, dstState, numBarriers, subresource, flags);
}

void Texture2D::Blit(const CommandList& commandList, uint32_t groupSizeX, uint32_t groupSizeY,
	uint32_t groupSizeZ, const DescriptorTable& uavSrvTable, uint32_t uavSrvSlot, uint8_t mipLevel,
	const DescriptorTable& srvTable, uint32_t srvSlot, const DescriptorTable& samplerTable,
	uint32_t samplerSlot, const Pipeline& pipeline)
{
	// Set pipeline layout and descriptor tables
	if (uavSrvTable) commandList.SetComputeDescriptorTable(uavSrvSlot, uavSrvTable);
	if (srvTable) commandList.SetComputeDescriptorTable(srvSlot, srvTable);
	if (samplerTable) commandList.SetComputeDescriptorTable(samplerSlot, samplerTable);

	// Set pipeline
	if (pipeline) commandList.SetPipelineState(pipeline);

	// Dispatch
	const auto& desc = m_resource->GetDesc();
	const auto width = (max)(static_cast<uint32_t>(desc.Width >> mipLevel), 1u);
	const auto height = (max)(desc.Height >> mipLevel, 1u);
	commandList.Dispatch(DIV_UP(width, groupSizeX), DIV_UP(height, groupSizeY),
		DIV_UP(desc.DepthOrArraySize, groupSizeZ));
}

uint32_t Texture2D::Blit(const CommandList& commandList, ResourceBarrier* pBarriers,
	uint32_t groupSizeX, uint32_t groupSizeY, uint32_t groupSizeZ, uint8_t mipLevel,
	int8_t srcMipLevel, ResourceState srcState, const DescriptorTable& uavSrvTable,
	uint32_t uavSrvSlot, uint32_t numBarriers, const DescriptorTable& srvTable,
	uint32_t srvSlot, uint32_t baseSlice, uint32_t numSlices)
{
	const auto prevBarriers = numBarriers;
	if (!numSlices) numSlices = m_resource->GetDesc().DepthOrArraySize - baseSlice;

	if (!mipLevel && srcMipLevel <= mipLevel)
		numBarriers = SetBarrier(pBarriers, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, numBarriers);
	else for (auto i = 0u; i < numSlices; ++i)
	{
		const auto j = baseSlice + i;
		numBarriers = SetBarrier(pBarriers, mipLevel, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, numBarriers, j);
		numBarriers = SetBarrier(pBarriers, srcMipLevel, srcState, numBarriers, j);
	}

	if (numBarriers > prevBarriers)
	{
		commandList.Barrier(numBarriers, pBarriers);
		numBarriers = 0;
	}

	Blit(commandList, groupSizeX, groupSizeY, groupSizeZ, uavSrvTable, uavSrvSlot, mipLevel, srvTable, srvSlot);

	return numBarriers;
}

uint32_t Texture2D::GenerateMips(const CommandList& commandList, ResourceBarrier* pBarriers, uint32_t groupSizeX,
	uint32_t groupSizeY, uint32_t groupSizeZ, ResourceState dstState, const PipelineLayout& pipelineLayout,
	const Pipeline& pipeline, const DescriptorTable* pUavSrvTables, uint32_t uavSrvSlot, const DescriptorTable& samplerTable,
	uint32_t samplerSlot, uint32_t numBarriers, const DescriptorTable* pSrvTables, uint32_t srvSlot, uint8_t baseMip,
	uint8_t numMips, uint32_t baseSlice, uint32_t numSlices)
{
	commandList.SetComputePipelineLayout(pipelineLayout);
	commandList.SetComputeDescriptorTable(samplerSlot, samplerTable);
	commandList.SetPipelineState(pipeline);

	if (!(numMips || baseMip || numSlices || baseSlice))
		numBarriers = SetBarrier(pBarriers, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, numBarriers);

	const auto& desc = m_resource->GetDesc();
	if (!numSlices) numSlices = desc.DepthOrArraySize - baseSlice;
	if (!numMips) numMips = desc.MipLevels - baseMip;

	for (auto i = 0ui8; i < numMips; ++i)
	{
		const auto j = baseMip + i;
		const auto prevBarriers = numBarriers;

		if (j > 0) for (auto k = 0u; k < numSlices; ++k)
		{
			const auto n = baseSlice + k;
			numBarriers = SetBarrier(pBarriers, j, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, numBarriers, n);
			numBarriers = SetBarrier(pBarriers, j - 1, dstState, numBarriers, n);
		}

		commandList.Barrier(numBarriers, pBarriers);
		numBarriers = 0;

		Blit(commandList, groupSizeX, groupSizeY, groupSizeZ, pUavSrvTables[i], uavSrvSlot, j,
			pSrvTables ? pSrvTables[i] : nullptr, srvSlot);
	}

	const auto m = baseMip + numMips - 1;
	for (auto i = 0u; i < numSlices; ++i)
		numBarriers = SetBarrier(pBarriers, m, dstState, numBarriers, baseSlice + i);

	return numBarriers;
}

Descriptor Texture2D::GetUAV(uint8_t i) const
{
	return m_uavs.size() > i ? m_uavs[i] : Descriptor(D3D12_DEFAULT);
}

Descriptor Texture2D::GetSRVLevel(uint8_t i) const
{
	return m_srvLevels.size() > i ? m_srvLevels[i] : Descriptor(D3D12_DEFAULT);
}

//--------------------------------------------------------------------------------------
// Render target
//--------------------------------------------------------------------------------------

RenderTarget::RenderTarget() :
	Texture2D(),
	m_rtvPools(0),
	m_rtvs(0)
{
}

RenderTarget::~RenderTarget()
{
}

bool RenderTarget::Create(const Device& device, uint32_t width, uint32_t height, Format format,
	uint32_t arraySize, ResourceFlags resourceFlags, uint8_t numMips, uint8_t sampleCount,
	ResourceState state, const float* pClearColor, bool isCubeMap, const wchar_t* name)
{
	N_RETURN(create(device, width, height, arraySize, format, numMips, sampleCount,
		resourceFlags, state, pClearColor, isCubeMap, name), false);

	// Setup the description of the render target view.
	D3D12_RENDER_TARGET_VIEW_DESC desc = {};
	desc.Format = format;

	numMips = (max)(numMips, 1ui8);
	m_rtvs.resize(arraySize);
	for (auto i = 0u; i < arraySize; ++i)
	{
		auto mipLevel = 0ui8;
		m_rtvs[i].resize(numMips);

		for (auto& descriptor : m_rtvs[i])
		{
			// Setup the description of the render target view.
			if (arraySize > 1)
			{
				if (sampleCount > 1)
				{
					desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
					desc.Texture2DMSArray.FirstArraySlice = i;
					desc.Texture2DMSArray.ArraySize = 1;
				}
				else
				{
					desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
					desc.Texture2DArray.FirstArraySlice = i;
					desc.Texture2DArray.ArraySize = 1;
					desc.Texture2DArray.MipSlice = mipLevel++;
				}
			}
			else
			{
				if (sampleCount > 1)
					desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
				else
				{
					desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
					desc.Texture2D.MipSlice = mipLevel++;
				}
			}

			// Create a render target view
			descriptor = allocateRtvPool();
			N_RETURN(descriptor.ptr, false);
			m_device->CreateRenderTargetView(m_resource.get(), &desc, descriptor);
		}
	}

	return true;
}

bool RenderTarget::CreateArray(const Device& device, uint32_t width, uint32_t height,
	uint32_t arraySize, Format format, ResourceFlags resourceFlags, uint8_t numMips,
	uint8_t sampleCount, ResourceState state, const float* pClearColor, bool isCubeMap,
	const wchar_t* name)
{
	N_RETURN(create(device, width, height, arraySize, format, numMips, sampleCount,
		resourceFlags, state, pClearColor, isCubeMap, name), false);

	// Setup the description of the render target view.
	D3D12_RENDER_TARGET_VIEW_DESC desc = {};
	desc.Format = format;

	m_rtvs.resize(1);
	m_rtvs[0].resize((max)(numMips, 1ui8));

	auto mipLevel = 0ui8;
	for (auto& descriptor : m_rtvs[0])
	{
		// Setup the description of the render target view.
		if (sampleCount > 1)
		{
			desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
			desc.Texture2DMSArray.ArraySize = arraySize;
		}
		else
		{
			desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
			desc.Texture2DArray.ArraySize = arraySize;
			desc.Texture2DArray.MipSlice = mipLevel++;
		}

		// Create a render target view
		descriptor = allocateRtvPool();
		N_RETURN(descriptor.ptr, false);
		m_device->CreateRenderTargetView(m_resource.get(), &desc, descriptor);
	}

	return true;
}

bool RenderTarget::CreateFromSwapChain(const Device& device, const SwapChain& swapChain, uint32_t bufferIdx)
{
	M_RETURN(!device, cerr, "The device is NULL.", false);
	setDevice(device);

	m_name = L"SwapChain[" + to_wstring(bufferIdx) + L"]";

	// Determine initial state
	m_states.resize(1);
	m_states[0] = D3D12_RESOURCE_STATE_PRESENT;

	// Get resource
	V_RETURN(swapChain->GetBuffer(bufferIdx, IID_PPV_ARGS(&m_resource)), cerr, false);

	// Create RTV
	m_rtvs.resize(1);
	m_rtvs[0].resize(1);
	m_rtvs[0][0] = allocateRtvPool();
	N_RETURN(m_rtvs[0][0].ptr, false);
	m_device->CreateRenderTargetView(m_resource.get(), nullptr, m_rtvs[0][0]);

	return true;
}

void RenderTarget::Blit(const CommandList& commandList, const DescriptorTable& srcSrvTable,
	uint32_t srcSlot, uint8_t mipLevel, uint32_t baseSlice, uint32_t numSlices,
	const DescriptorTable& samplerTable, uint32_t samplerSlot, const Pipeline& pipeline)
{
	// Set render target
	const auto& desc = m_resource->GetDesc();
	if (numSlices == 0) numSlices = desc.DepthOrArraySize - baseSlice;
	vector<Descriptor> rtvs(numSlices);
	for (auto i = 0u; i < numSlices; ++i) rtvs[i] = GetRTV(baseSlice + i, mipLevel);
	const auto rtvTable = make_shared<Descriptor>(*rtvs.data());
	commandList.OMSetRenderTargets(numSlices, rtvTable, nullptr);

	// Set pipeline layout and descriptor tables
	if (srcSrvTable) commandList.SetGraphicsDescriptorTable(srcSlot, srcSrvTable);
	if (samplerTable) commandList.SetGraphicsDescriptorTable(samplerSlot, samplerTable);

	// Set pipeline
	if (pipeline) commandList.SetPipelineState(pipeline);

	// Set viewport
	const auto width = (max)(static_cast<uint32_t>(desc.Width >> mipLevel), 1u);
	const auto height = (max)(desc.Height >> mipLevel, 1u);
	const Viewport viewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
	const RectRange rect(0, 0, width, height);
	commandList.RSSetViewports(1, &viewport);
	commandList.RSSetScissorRects(1, &rect);

	// Draw quad
	commandList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	commandList.Draw(3, 1, 0, 0);
}

uint32_t RenderTarget::Blit(const CommandList& commandList, ResourceBarrier* pBarriers, uint8_t mipLevel,
	int8_t srcMipLevel, ResourceState srcState, const DescriptorTable& srcSrvTable, uint32_t srcSlot,
	uint32_t numBarriers, uint32_t baseSlice, uint32_t numSlices)
{
	const auto prevBarriers = numBarriers;
	if (!numSlices) numSlices = m_resource->GetDesc().DepthOrArraySize - baseSlice;

	if (!mipLevel && srcMipLevel <= mipLevel)
		numBarriers = SetBarrier(pBarriers, D3D12_RESOURCE_STATE_RENDER_TARGET, numBarriers);
	else for (auto i = 0u; i < numSlices; ++i)
	{
		const auto j = baseSlice + i;
		numBarriers = SetBarrier(pBarriers, mipLevel, D3D12_RESOURCE_STATE_RENDER_TARGET, numBarriers, j);
		numBarriers = SetBarrier(pBarriers, srcMipLevel, srcState, numBarriers, j);
	}

	if (numBarriers > prevBarriers)
	{
		commandList.Barrier(numBarriers, pBarriers);
		numBarriers = 0;
	}

	Blit(commandList, srcSrvTable, srcSlot, mipLevel, baseSlice, numSlices);

	return numBarriers;
}

uint32_t RenderTarget::GenerateMips(const CommandList& commandList, ResourceBarrier* pBarriers,
	ResourceState dstState, const PipelineLayout& pipelineLayout, const Pipeline& pipeline,
	const DescriptorTable* pSrcSrvTables, uint32_t srcSlot, const DescriptorTable& samplerTable,
	uint32_t samplerSlot, uint32_t numBarriers, uint8_t baseMip, uint8_t numMips,
	uint32_t baseSlice, uint32_t numSlices)
{
	commandList.SetGraphicsPipelineLayout(pipelineLayout);
	commandList.SetGraphicsDescriptorTable(samplerSlot, samplerTable);
	commandList.SetPipelineState(pipeline);

	if (!(numMips || baseMip || numSlices || baseSlice))
		numBarriers = SetBarrier(pBarriers, D3D12_RESOURCE_STATE_RENDER_TARGET, numBarriers);

	const auto& desc = m_resource->GetDesc();
	if (!numSlices) numSlices = desc.DepthOrArraySize - baseSlice;
	if (!numMips) numMips = desc.MipLevels - baseMip;

	for (auto i = 0ui8; i < numMips; ++i)
	{
		const auto j = baseMip + i;
		const auto prevBarriers = numBarriers;

		if (j > 0) for (auto k = 0u; k < numSlices; ++k)
		{
			const auto n = baseSlice + k;
			numBarriers = SetBarrier(pBarriers, j, D3D12_RESOURCE_STATE_RENDER_TARGET, numBarriers, n);
			numBarriers = SetBarrier(pBarriers, j - 1, dstState, numBarriers, n);
		}

		commandList.Barrier(numBarriers, pBarriers);
		numBarriers = 0;

		Blit(commandList, pSrcSrvTables[i], srcSlot, j, baseSlice, numSlices);
	}

	const auto m = baseMip + numMips - 1;
	for (auto i = 0u; i < numSlices; ++i)
		numBarriers = SetBarrier(pBarriers, m, dstState, numBarriers, baseSlice + i);

	return numBarriers;
}

Descriptor RenderTarget::GetRTV(uint32_t slice, uint8_t mipLevel) const
{
	return m_rtvs.size() > slice && m_rtvs[slice].size() > mipLevel ?
		m_rtvs[slice][mipLevel] : Descriptor(D3D12_DEFAULT);
}

uint32_t RenderTarget::GetArraySize() const
{
	return static_cast<uint32_t>(m_rtvs.size());
}

uint8_t RenderTarget::GetNumMips(uint32_t slice) const
{
	return m_rtvs.size() > slice ? static_cast<uint8_t>(m_rtvs[slice].size()) : 0;
}

bool RenderTarget::create(const Device& device, uint32_t width, uint32_t height,
	uint32_t arraySize, Format format, uint8_t numMips, uint8_t sampleCount,
	ResourceFlags resourceFlags, ResourceState state, const float* pClearColor,
	bool isCubeMap, const wchar_t* name)
{
	M_RETURN(!device, cerr, "The device is NULL.", false);
	setDevice(device);

	if (name) m_name = name;

	const auto isPacked = (resourceFlags & BIND_PACKED_UAV) == BIND_PACKED_UAV;
	resourceFlags &= REMOVE_PACKED_UAV;

	const auto hasSRV = !(resourceFlags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
	const bool hasUAV = resourceFlags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	// Map formats
	auto formatResource = format;
	const auto formatUAV = isPacked ? MapToPackedFormat(formatResource) : format;

	// Setup the texture description.
	const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(formatResource, width, height, arraySize,
		numMips, sampleCount, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | resourceFlags);

	// Determine initial state
	m_states.resize(arraySize * numMips);
	for (auto& initState : m_states) initState = state ? state : D3D12_RESOURCE_STATE_RENDER_TARGET;

	// Optimized clear value
	D3D12_CLEAR_VALUE clearValue = { format };
	if (pClearColor) memcpy(clearValue.Color, pClearColor, sizeof(clearValue.Color));

	// Create the render target texture.
	V_RETURN(m_device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE, &desc, m_states[0], &clearValue, IID_PPV_ARGS(&m_resource)), clog, false);
	if (!m_name.empty()) m_resource->SetName((m_name + L".Resource").c_str());

	// Create SRV
	if (hasSRV)
	{
		N_RETURN(CreateSRVs(arraySize, format, numMips, sampleCount, isCubeMap), false);
		N_RETURN(CreateSRVLevels(arraySize, numMips, format, sampleCount, isCubeMap), false);
	}

	// Create UAV
	if (hasUAV) N_RETURN(CreateUAVs(arraySize, formatUAV, numMips), false);

	return true;
}

Descriptor RenderTarget::allocateRtvPool()
{
	m_rtvPools.push_back(DescriptorPool());
	auto& rtvPool = m_rtvPools.back();

	D3D12_DESCRIPTOR_HEAP_DESC desc = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
	V_RETURN(m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&rtvPool)), cerr, Descriptor(D3D12_DEFAULT));
	if (!m_name.empty()) rtvPool->SetName((m_name + L".RtvPool").c_str());

	return Descriptor(rtvPool->GetCPUDescriptorHandleForHeapStart());
}

//--------------------------------------------------------------------------------------
// Depth stencil
//--------------------------------------------------------------------------------------

DepthStencil::DepthStencil() :
	Texture2D(),
	m_dsvPools(),
	m_dsvs(0),
	m_readOnlyDsvs(0),
	m_stencilSrv(D3D12_DEFAULT)
{
}

DepthStencil::~DepthStencil()
{
}

bool DepthStencil::Create(const Device& device, uint32_t width, uint32_t height, Format format,
	ResourceFlags resourceFlags, uint32_t arraySize, uint8_t numMips, uint8_t sampleCount,
	ResourceState state, float clearDepth, uint8_t clearStencil, bool isCubeMap,
	const wchar_t* name)
{
	bool hasSRV;
	Format formatStencil;
	N_RETURN(create(device, width, height, arraySize, numMips, sampleCount, format, resourceFlags,
		state, clearDepth, clearStencil, hasSRV, formatStencil, isCubeMap, name), false);

	// Setup the description of the depth stencil view.
	D3D12_DEPTH_STENCIL_VIEW_DESC desc = {};
	desc.Format = format;

	numMips = (max)(numMips, 1ui8);
	m_dsvs.resize(arraySize);
	m_readOnlyDsvs.resize(arraySize);

	for (auto i = 0u; i < arraySize; ++i)
	{
		auto mipLevel = 0ui8;
		m_dsvs[i].resize(numMips);
		m_readOnlyDsvs[i].resize(numMips);

		for (auto j = 0ui8; j < numMips; ++j)
		{
			auto& dsv = m_dsvs[i][j];
			auto& readOnlyDsv = m_readOnlyDsvs[i][j];

			// Setup the description of the depth stencil view.
			if (arraySize > 1)
			{
				if (sampleCount > 1)
				{
					desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
					desc.Texture2DMSArray.FirstArraySlice = i;
					desc.Texture2DMSArray.ArraySize = 1;
				}
				else
				{
					desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
					desc.Texture2DArray.FirstArraySlice = i;
					desc.Texture2DArray.ArraySize = 1;
					desc.Texture2DArray.MipSlice = j;
				}
			}
			else
			{
				if (sampleCount > 1)
					desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
				else
				{
					desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
					desc.Texture2D.MipSlice = j;
				}
			}

			// Create a depth stencil view
			dsv = allocateDsvPool();
			N_RETURN(dsv.ptr, false);
			m_device->CreateDepthStencilView(m_resource.get(), &desc, dsv);

			// Read-only depth stencil
			if (hasSRV)
			{
				// Setup the description of the depth stencil view.
				desc.Flags = formatStencil ? D3D12_DSV_FLAG_READ_ONLY_DEPTH |
					D3D12_DSV_FLAG_READ_ONLY_STENCIL : D3D12_DSV_FLAG_READ_ONLY_DEPTH;

				// Create a depth stencil view
				readOnlyDsv = allocateDsvPool();
				N_RETURN(readOnlyDsv.ptr, false);
				m_device->CreateDepthStencilView(m_resource.get(), &desc, readOnlyDsv);
			}
			else readOnlyDsv = dsv;
		}
	}

	return true;
}

bool DepthStencil::CreateArray(const Device& device, uint32_t width, uint32_t height, uint32_t arraySize,
	Format format, ResourceFlags resourceFlags, uint8_t numMips, uint8_t sampleCount, ResourceState state,
	float clearDepth, uint8_t clearStencil, bool isCubeMap, const wchar_t* name)
{
	bool hasSRV;
	Format formatStencil;
	N_RETURN(create(device, width, height, arraySize, numMips, sampleCount, format, resourceFlags,
		state, clearDepth, clearStencil, hasSRV, formatStencil, isCubeMap, name), false);

	// Setup the description of the depth stencil view.
	D3D12_DEPTH_STENCIL_VIEW_DESC desc = {};
	desc.Format = format;

	numMips = (max)(numMips, 1ui8);
	m_dsvs.resize(1);
	m_dsvs[0].resize(numMips);
	m_readOnlyDsvs.resize(1);
	m_readOnlyDsvs[0].resize(numMips);

	for (auto i = 0ui8; i < numMips; ++i)
	{
		auto& dsv = m_dsvs[0][i];
		auto& readOnlyDsv = m_readOnlyDsvs[0][i];

		// Setup the description of the depth stencil view.
		if (arraySize > 1)
		{
			if (sampleCount > 1)
			{
				desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
				desc.Texture2DMSArray.ArraySize = arraySize;
			}
			else
			{
				desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
				desc.Texture2DArray.ArraySize = arraySize;
				desc.Texture2DArray.MipSlice = i;
			}
		}
		else
		{
			if (sampleCount > 1)
				desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
			else
			{
				desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
				desc.Texture2D.MipSlice = i;
			}
		}

		// Create a depth stencil view
		dsv = allocateDsvPool();
		N_RETURN(dsv.ptr, false);
		m_device->CreateDepthStencilView(m_resource.get(), &desc, dsv);

		// Read-only depth stencil
		if (hasSRV)
		{
			// Setup the description of the depth stencil view.
			desc.Flags = formatStencil ? D3D12_DSV_FLAG_READ_ONLY_DEPTH |
				D3D12_DSV_FLAG_READ_ONLY_STENCIL : D3D12_DSV_FLAG_READ_ONLY_DEPTH;

			// Create a depth stencil view
			readOnlyDsv = allocateDsvPool();
			N_RETURN(readOnlyDsv.ptr, false);
			m_device->CreateDepthStencilView(m_resource.get(), &desc, readOnlyDsv);
		}
		else readOnlyDsv = dsv;
	}

	return true;
}

Descriptor DepthStencil::GetDSV(uint32_t slice, uint8_t mipLevel) const
{
	return m_dsvs.size() > slice && m_dsvs[slice].size() > mipLevel ?
		m_dsvs[slice][mipLevel] : Descriptor(D3D12_DEFAULT);
}

Descriptor DepthStencil::GetReadOnlyDSV(uint32_t slice, uint8_t mipLevel) const
{
	return m_readOnlyDsvs.size() > slice && m_readOnlyDsvs[slice].size() > mipLevel ?
		m_readOnlyDsvs[slice][mipLevel] : Descriptor(D3D12_DEFAULT);
}

const Descriptor& DepthStencil::GetStencilSRV() const
{
	return m_stencilSrv;
}

Format DepthStencil::GetDSVFormat() const
{
	return m_dsvFormat;
}

uint32_t DepthStencil::GetArraySize() const
{
	return static_cast<uint32_t>(m_dsvs.size());
}

uint8_t DepthStencil::GetNumMips() const
{
	return static_cast<uint8_t>(m_dsvs.size());
}

bool DepthStencil::create(const Device& device, uint32_t width, uint32_t height, uint32_t arraySize,
	uint8_t numMips, uint8_t sampleCount, Format& format, ResourceFlags resourceFlags, ResourceState state,
	float clearDepth, uint8_t clearStencil, bool& hasSRV, Format& formatStencil, bool isCubeMap,
	const wchar_t* name)
{
	M_RETURN(!device, cerr, "The device is NULL.", false);
	setDevice(device);

	if (name) m_name = name;

	hasSRV = !(resourceFlags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);

	// Map formats
	auto formatResource = format;
	auto formatDepth = DXGI_FORMAT_UNKNOWN;
	formatStencil = DXGI_FORMAT_UNKNOWN;

	if (hasSRV)
	{
		switch (format)
		{
		case DXGI_FORMAT_D24_UNORM_S8_UINT:
			formatResource = DXGI_FORMAT_R24G8_TYPELESS;
			formatDepth = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
			formatStencil = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
			break;
		case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
			formatResource = DXGI_FORMAT_R32G8X24_TYPELESS;
			formatDepth = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
			formatStencil = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
			break;
		case DXGI_FORMAT_D16_UNORM:
			formatResource = DXGI_FORMAT_R16_TYPELESS;
			formatDepth = DXGI_FORMAT_R16_UNORM;
			break;
		case DXGI_FORMAT_D32_FLOAT:
			formatResource = DXGI_FORMAT_R32_TYPELESS;
			formatDepth = DXGI_FORMAT_R32_FLOAT;
			break;
		default:
			format = DXGI_FORMAT_D24_UNORM_S8_UINT;
			formatResource = DXGI_FORMAT_R24G8_TYPELESS;
			formatDepth = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
		}
	}

	m_dsvFormat = format;

	// Setup the render depth stencil description.
	{
		const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(formatResource, width, height, arraySize,
			numMips, sampleCount, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | resourceFlags);

		// Determine initial state
		m_states.resize(arraySize * numMips);
		for (auto& initState : m_states) initState = state ? state : D3D12_RESOURCE_STATE_DEPTH_WRITE;

		// Optimized clear value
		D3D12_CLEAR_VALUE clearValue = { format };
		clearValue.DepthStencil.Depth = clearDepth;
		clearValue.DepthStencil.Stencil = clearStencil;

		// Create the depth stencil texture.
		V_RETURN(m_device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
			D3D12_HEAP_FLAG_NONE, &desc, m_states[0], &clearValue, IID_PPV_ARGS(&m_resource)), clog, false);
		if (!m_name.empty()) m_resource->SetName((m_name + L".Resource").c_str());
	}

	if (hasSRV)
	{
		// Create SRV
		if (hasSRV) N_RETURN(CreateSRVs(arraySize, formatDepth, numMips, sampleCount, isCubeMap), false);

		// Has stencil
		if (formatStencil)
		{
			// Setup the description of the shader resource view.
			D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
			desc.Format = formatStencil;
			desc.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(1, 4, 4, 4);

			if (isCubeMap)
			{
				assert(arraySize % 6 == 0);
				if (arraySize > 6)
				{
					desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
					desc.TextureCubeArray.MipLevels = numMips;
					desc.TextureCubeArray.NumCubes = arraySize / 6;
				}
				else
				{
					desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
					desc.TextureCube.MipLevels = numMips;
				}
			}
			else if (arraySize > 1)
			{
				if (sampleCount > 1)
				{
					desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
					desc.Texture2DMSArray.ArraySize = arraySize;
				}
				else
				{
					desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
					desc.Texture2DArray.ArraySize = arraySize;
					desc.Texture2DArray.MipLevels = numMips;
				}
			}
			else
			{
				if (sampleCount > 1)
					desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
				else
				{
					desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
					desc.Texture2D.MipLevels = numMips;
				}
			}

			// Create a shader resource view
			m_stencilSrv = allocateSrvUavPool();
			N_RETURN(m_stencilSrv.ptr, false);
			m_device->CreateShaderResourceView(m_resource.get(), &desc, m_stencilSrv);
		}
	}

	return true;
}

Descriptor DepthStencil::allocateDsvPool()
{
	m_dsvPools.push_back(DescriptorPool());
	auto& dsvPool = m_dsvPools.back();

	D3D12_DESCRIPTOR_HEAP_DESC desc = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1 };
	V_RETURN(m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&dsvPool)), cerr, Descriptor(D3D12_DEFAULT));
	if (!m_name.empty()) dsvPool->SetName((m_name + L".DsvPool").c_str());

	return Descriptor(dsvPool->GetCPUDescriptorHandleForHeapStart());
}

//--------------------------------------------------------------------------------------
// 3D Texture
//--------------------------------------------------------------------------------------

Texture3D::Texture3D() :
	ResourceBase(),
	m_counter(nullptr),
	m_uavs(0),
	m_srvLevels(0)
{
}

Texture3D::~Texture3D()
{
}

bool Texture3D::Create(const Device& device, uint32_t width, uint32_t height,
	uint32_t depth, Format format, ResourceFlags resourceFlags, uint8_t numMips,
	MemoryType memoryType, ResourceState state, const wchar_t* name)
{
	M_RETURN(!device, cerr, "The device is NULL.", false);
	setDevice(device);

	if (name) m_name = name;

	const auto isPacked = (resourceFlags & BIND_PACKED_UAV) == BIND_PACKED_UAV;
	resourceFlags &= REMOVE_PACKED_UAV;

	const auto hasSRV = !(resourceFlags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
	const bool hasUAV = resourceFlags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	// Map formats
	auto formatResource = format;
	const auto formatUAV = isPacked ? MapToPackedFormat(formatResource) : format;

	// Setup the texture description.
	const auto desc = CD3DX12_RESOURCE_DESC::Tex3D(formatResource, width, height, depth,
		numMips, resourceFlags);

	// Determine initial state
	m_states.resize(numMips);
	if (state) for (auto& initState : m_states) initState = state;
	else for (auto& initState : m_states)
	{
		initState = hasSRV ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON;
		initState = hasUAV ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : initState;
	}

	V_RETURN(m_device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(memoryType),
		D3D12_HEAP_FLAG_NONE, &desc, m_states[0], nullptr, IID_PPV_ARGS(&m_resource)), clog, false);
	if (!m_name.empty()) m_resource->SetName((m_name + L".Resource").c_str());

	// Create SRV
	if (hasSRV) N_RETURN(CreateSRVs(format, numMips), false);

	// Create UAV
	if (hasUAV) N_RETURN(CreateUAVs(formatUAV, numMips), false);

	// Create SRV for each level
	if (hasSRV && hasUAV) N_RETURN(CreateSRVLevels(numMips, format), false);

	return true;
}

bool Texture3D::CreateSRVs(Format format, uint8_t numMips)
{
	// Setup the description of the shader resource view.
	D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
	desc.Format = format ? format : m_resource->GetDesc().Format;
	desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;

	auto mipLevel = 0ui8;
	m_srvs.resize((max)(numMips, 1ui8));

	for (auto& descriptor : m_srvs)
	{
		// Setup the description of the shader resource view.
		desc.Texture3D.MipLevels = numMips - mipLevel;
		desc.Texture3D.MostDetailedMip = mipLevel++;

		// Create a shader resource view
		descriptor = allocateSrvUavPool();
		N_RETURN(descriptor.ptr, false);
		m_device->CreateShaderResourceView(m_resource.get(), &desc, descriptor);
	}

	return true;
}

bool Texture3D::CreateSRVLevels(uint8_t numMips, Format format)
{
	if (numMips > 1)
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
		desc.Format = format ? format : m_resource->GetDesc().Format;
		desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;

		auto mipLevel = 0ui8;
		m_srvLevels.resize(numMips);

		for (auto& descriptor : m_srvLevels)
		{
			// Setup the description of the shader resource view.
			desc.Texture3D.MostDetailedMip = mipLevel++;
			desc.Texture3D.MipLevels = 1;

			// Create a shader resource view
			descriptor = allocateSrvUavPool();
			N_RETURN(descriptor.ptr, false);
			m_device->CreateShaderResourceView(m_resource.get(), &desc, descriptor);
		}
	}

	return true;
}

bool Texture3D::CreateUAVs(Format format, uint8_t numMips)
{
	const auto txDesc = m_resource->GetDesc();

	// Setup the description of the unordered access view.
	D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
	desc.Format = format ? format : txDesc.Format;
	desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;

	auto mipLevel = 0ui8;
	m_uavs.resize(numMips);

	for (auto& descriptor : m_uavs)
	{
		// Setup the description of the unordered access view.
		desc.Texture3D.WSize = txDesc.DepthOrArraySize >> mipLevel;
		desc.Texture3D.MipSlice = mipLevel++;

		// Create an unordered access view
		descriptor = allocateSrvUavPool();
		N_RETURN(descriptor.ptr, false);
		m_device->CreateUnorderedAccessView(m_resource.get(), m_counter.get(), &desc, descriptor);
	}

	return true;
}

Descriptor Texture3D::GetUAV(uint8_t i) const
{
	return m_uavs.size() > i ? m_uavs[i] : Descriptor(D3D12_DEFAULT);
}

Descriptor Texture3D::GetSRVLevel(uint8_t i) const
{
	return m_srvLevels.size() > i ? m_srvLevels[i] : Descriptor(D3D12_DEFAULT);
}

//--------------------------------------------------------------------------------------
// Raw buffer
//--------------------------------------------------------------------------------------

RawBuffer::RawBuffer() :
	ResourceBase(),
	m_counter(nullptr),
	m_uavs(0),
	m_srvOffsets(0),
	m_pDataBegin(nullptr)
{
}

RawBuffer::~RawBuffer()
{
	if (m_resource) Unmap();
}

bool RawBuffer::Create(const Device& device, uint64_t byteWidth, ResourceFlags resourceFlags,
	MemoryType memoryType, ResourceState state, uint32_t numSRVs, const uint32_t* firstSRVElements,
	uint32_t numUAVs, const uint32_t* firstUAVElements, const wchar_t* name)
{
	const auto hasSRV = !(resourceFlags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
	const bool hasUAV = resourceFlags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	numSRVs = hasSRV ? numSRVs : 0;
	numUAVs = hasUAV ? numUAVs : 0;

	// Create buffer
	N_RETURN(create(device, byteWidth, resourceFlags, memoryType, state, numSRVs, numUAVs, name), false);

	// Create SRV
	if (numSRVs > 0) N_RETURN(CreateSRVs(byteWidth, firstSRVElements, numSRVs), false);

	// Create UAV
	if (numUAVs > 0) N_RETURN(CreateUAVs(byteWidth, firstUAVElements, numUAVs), false);

	return true;
}

bool RawBuffer::Upload(const CommandList& commandList, Resource& uploader,
	const void* pData, size_t size, ResourceState dstState, uint32_t i)
{
	const auto offset = m_srvOffsets.empty() ? 0 : m_srvOffsets[i];
	D3D12_SUBRESOURCE_DATA subresourceData;
	subresourceData.pData = pData;
	subresourceData.RowPitch = static_cast<uint32_t>(size);
	subresourceData.SlicePitch = static_cast<uint32_t>(m_resource->GetDesc().Width);

	// Create the GPU upload buffer.
	if (!uploader)
	{
		V_RETURN(m_device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
			D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(offset + size),
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&uploader)), clog, false);
		if (!m_name.empty()) uploader->SetName((m_name + L".UploaderResource").c_str());
	}

	// Copy data to the intermediate upload heap and then schedule a copy 
	// from the upload heap to the buffer.
	ResourceBarrier barrier;
	dstState = dstState ? dstState : m_states[0];
	auto numBarriers = SetBarrier(&barrier, D3D12_RESOURCE_STATE_COPY_DEST);
	commandList.Barrier(numBarriers, &barrier);
	M_RETURN(UpdateSubresources(const_cast<CommandList&>(commandList).GetCommandList().get(),
		m_resource.get(), uploader.get(), offset, 0, 1, &subresourceData) <= 0,
		clog, "Failed to upload the resource.", false);
	numBarriers = SetBarrier(&barrier, dstState);
	commandList.Barrier(numBarriers, &barrier);

	return true;
}

bool RawBuffer::CreateSRVs(uint64_t byteWidth, const uint32_t* firstElements,
	uint32_t numDescriptors)
{
	D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
	desc.Format = DXGI_FORMAT_R32_TYPELESS;
	desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;

	const uint32_t stride = sizeof(uint32_t);
	const auto numElements = static_cast<uint32_t>(byteWidth / stride);

	m_srvOffsets.resize(numDescriptors);
	m_srvs.resize(numDescriptors);
	for (auto i = 0u; i < numDescriptors; ++i)
	{
		const auto firstElement = firstElements ? firstElements[i] : 0;
		desc.Buffer.FirstElement = firstElement;
		desc.Buffer.NumElements = (!firstElements || i + 1 >= numDescriptors ?
			numElements : firstElements[i + 1]) - firstElement;

		m_srvOffsets[i] = stride * firstElement;

		// Create a shader resource view
		m_srvs[i] = allocateSrvUavPool();
		N_RETURN(m_srvs[i].ptr, false);
		m_device->CreateShaderResourceView(m_resource.get(), &desc, m_srvs[i]);
	}

	return true;
}

bool RawBuffer::CreateUAVs(uint64_t byteWidth, const uint32_t* firstElements,
	uint32_t numDescriptors)
{
	D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
	desc.Format = DXGI_FORMAT_R32_TYPELESS;
	desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;

	const uint32_t numElements = static_cast<uint32_t>(byteWidth / sizeof(uint32_t));

	m_uavs.resize(numDescriptors);
	for (auto i = 0u; i < numDescriptors; ++i)
	{
		const auto firstElement = firstElements ? firstElements[i] : 0;
		desc.Buffer.FirstElement = firstElement;
		desc.Buffer.NumElements = (!firstElements || i + 1 >= numDescriptors ?
			numElements : firstElements[i + 1]) - firstElement;

		// Create an unordered access view
		m_uavs[i] = allocateSrvUavPool();
		N_RETURN(m_uavs[i].ptr, false);
		m_device->CreateUnorderedAccessView(m_resource.get(), m_counter.get(), &desc, m_uavs[i]);
	}

	return true;
}

Descriptor RawBuffer::GetUAV(uint32_t i) const
{
	return m_uavs.size() > i ? m_uavs[i] : Descriptor(D3D12_DEFAULT);
}

void* RawBuffer::Map(uint32_t i)
{
	if (m_pDataBegin == nullptr)
	{
		// Map and initialize the buffer.
		CD3DX12_RANGE readRange(0, 0);	// We do not intend to read from this resource on the CPU.
		V_RETURN(m_resource->Map(0, &readRange, &m_pDataBegin), cerr, false);
	}

	return &reinterpret_cast<uint8_t*>(m_pDataBegin)[m_srvOffsets[i]];
}

void RawBuffer::Unmap()
{
	if (m_pDataBegin)
	{
		m_resource->Unmap(0, nullptr);
		m_pDataBegin = nullptr;
	}
}

bool RawBuffer::create(const Device& device, uint64_t byteWidth, ResourceFlags resourceFlags,
	MemoryType memoryType, ResourceState state, uint32_t numSRVs, uint32_t numUAVs,
	const wchar_t* name)
{
	M_RETURN(!device, cerr, "The device is NULL.", false);
	setDevice(device);

	if (name) m_name = name;

	// Setup the buffer description.
	const auto desc = CD3DX12_RESOURCE_DESC::Buffer(byteWidth, resourceFlags);

	// Determine initial state
	m_states.resize(1);
	auto& initState = m_states[0];
	if (state) initState = state;
	else
	{
		initState = numSRVs > 0 ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON;
		initState = numUAVs > 0 ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : initState;
	}

	V_RETURN(m_device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(memoryType),
		D3D12_HEAP_FLAG_NONE, &desc, initState, nullptr, IID_PPV_ARGS(&m_resource)), clog, false);
	if (!m_name.empty()) m_resource->SetName((m_name + L".Resource").c_str());

	return true;
}

//--------------------------------------------------------------------------------------
// Structured buffer
//--------------------------------------------------------------------------------------

StructuredBuffer::StructuredBuffer() :
	RawBuffer()
{
}

StructuredBuffer::~StructuredBuffer()
{
}

bool StructuredBuffer::Create(const Device& device, uint32_t numElements, uint32_t stride,
	ResourceFlags resourceFlags, MemoryType memoryType, ResourceState state, uint32_t numSRVs,
	const uint32_t* firstSRVElements, uint32_t numUAVs, const uint32_t* firstUAVElements,
	const wchar_t* name)
{
	const auto hasSRV = !(resourceFlags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
	const bool hasUAV = resourceFlags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	numSRVs = hasSRV ? numSRVs : 0;
	numUAVs = hasUAV ? numUAVs : 0;

	// Create buffer
	N_RETURN(create(device, stride * numElements, resourceFlags,
		memoryType, state, numSRVs, numUAVs, name), false);

	// Create SRV
	if (numSRVs > 0) N_RETURN(CreateSRVs(numElements, stride, firstSRVElements, numSRVs), false);

	// Create UAV
	if (numUAVs > 0) N_RETURN(CreateUAVs(numElements, stride, firstUAVElements, numUAVs), false);

	return true;
}

bool StructuredBuffer::CreateSRVs(uint32_t numElements, uint32_t stride,
	const uint32_t* firstElements, uint32_t numDescriptors)
{
	D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
	desc.Format = m_resource->GetDesc().Format;
	desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	desc.Buffer.StructureByteStride = stride;

	m_srvOffsets.resize(numDescriptors);
	m_srvs.resize(numDescriptors);
	for (auto i = 0u; i < numDescriptors; ++i)
	{
		const auto firstElement = firstElements ? firstElements[i] : 0;
		desc.Buffer.FirstElement = firstElement;
		desc.Buffer.NumElements = (!firstElements || i + 1 >= numDescriptors ?
			numElements : firstElements[i + 1]) - firstElement;

		m_srvOffsets[i] = stride * firstElement;

		// Create a shader resource view
		m_srvs[i] = allocateSrvUavPool();
		N_RETURN(m_srvs[i].ptr, false);
		m_device->CreateShaderResourceView(m_resource.get(), &desc, m_srvs[i]);
	}

	return true;
}

bool StructuredBuffer::CreateUAVs(uint32_t numElements, uint32_t stride,
	const uint32_t* firstElements, uint32_t numDescriptors)
{
	D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
	desc.Format = m_resource->GetDesc().Format;
	desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	desc.Buffer.StructureByteStride = stride;

	m_uavs.resize(numDescriptors);
	for (auto i = 0u; i < numDescriptors; ++i)
	{
		const auto firstElement = firstElements ? firstElements[i] : 0;
		desc.Buffer.FirstElement = firstElement;
		desc.Buffer.NumElements = (!firstElements || i + 1 >= numDescriptors ?
			numElements : firstElements[i + 1]) - firstElement;

		// Create an unordered access view
		m_uavs[i] = allocateSrvUavPool();
		N_RETURN(m_uavs[i].ptr, false);
		m_device->CreateUnorderedAccessView(m_resource.get(), m_counter.get(), &desc, m_uavs[i]);
	}

	return true;
}

//--------------------------------------------------------------------------------------
// Typed buffer
//--------------------------------------------------------------------------------------

TypedBuffer::TypedBuffer() :
	RawBuffer()
{
}

TypedBuffer::~TypedBuffer()
{
}

bool TypedBuffer::Create(const Device& device, uint32_t numElements, uint32_t stride,
	Format format, ResourceFlags resourceFlags, MemoryType memoryType, ResourceState state,
	uint32_t numSRVs, const uint32_t* firstSRVElements,
	uint32_t numUAVs, const uint32_t* firstUAVElements,
	const wchar_t* name)
{
	M_RETURN(!device, cerr, "The device is NULL.", false);
	setDevice(device);

	const auto isPacked = (resourceFlags & BIND_PACKED_UAV) == BIND_PACKED_UAV;
	resourceFlags &= REMOVE_PACKED_UAV;

	const auto hasSRV = !(resourceFlags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
	const bool hasUAV = resourceFlags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	numSRVs = hasSRV ? numSRVs : 0;
	numUAVs = hasUAV ? numUAVs : 0;

	// Map formats
	auto formatResource = format;
	const auto formatUAV = isPacked ? MapToPackedFormat(formatResource) : format;

	// Create buffer
	N_RETURN(create(device, stride * numElements, resourceFlags,
		memoryType, state, numSRVs, numUAVs, name), false);

	// Create SRV
	if (numSRVs > 0) N_RETURN(CreateSRVs(numElements, format, stride, firstSRVElements, numSRVs), false);

	// Create UAV
	if (numUAVs > 0) N_RETURN(CreateUAVs(numElements, formatUAV, stride, firstUAVElements, numUAVs), false);

	return true;
}

bool TypedBuffer::CreateSRVs(uint32_t numElements, Format format, uint32_t stride,
	const uint32_t* firstElements, uint32_t numDescriptors)
{
	D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
	desc.Format = format ? format : m_resource->GetDesc().Format;
	desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;

	m_srvOffsets.resize(numDescriptors);
	m_srvs.resize(numDescriptors);
	for (auto i = 0u; i < numDescriptors; ++i)
	{
		const auto firstElement = firstElements ? firstElements[i] : 0;
		desc.Buffer.FirstElement = firstElement;
		desc.Buffer.NumElements = (!firstElements || i + 1 >= numDescriptors ?
			numElements : firstElements[i + 1]) - firstElement;

		m_srvOffsets[i] = stride * firstElement;

		// Create a shader resource view
		m_srvs[i] = allocateSrvUavPool();
		N_RETURN(m_srvs[i].ptr, false);
		m_device->CreateShaderResourceView(m_resource.get(), &desc, m_srvs[i]);
	}

	return true;
}

bool TypedBuffer::CreateUAVs(uint32_t numElements, Format format, uint32_t stride,
	const uint32_t* firstElements, uint32_t numDescriptors)
{
	D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
	desc.Format = format ? format : m_resource->GetDesc().Format;
	desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;

	m_uavs.resize(numDescriptors);
	for (auto i = 0u; i < numDescriptors; ++i)
	{
		const auto firstElement = firstElements ? firstElements[i] : 0;
		desc.Buffer.FirstElement = firstElement;
		desc.Buffer.NumElements = (!firstElements || i + 1 >= numDescriptors ?
			numElements : firstElements[i + 1]) - firstElement;

		// Create an unordered access view
		m_uavs[i] = allocateSrvUavPool();
		N_RETURN(m_uavs[i].ptr, false);
		m_device->CreateUnorderedAccessView(m_resource.get(), m_counter.get(), &desc, m_uavs[i]);
	}

	return true;
}

//--------------------------------------------------------------------------------------
// Vertex buffer
//--------------------------------------------------------------------------------------

VertexBuffer::VertexBuffer() :
	StructuredBuffer(),
	m_vbvs(0)
{
}

VertexBuffer::~VertexBuffer()
{
}

bool VertexBuffer::Create(const Device& device, uint32_t numVertices, uint32_t stride,
	ResourceFlags resourceFlags, MemoryType memoryType, ResourceState state,
	uint32_t numVBVs, const uint32_t* firstVertices,
	uint32_t numSRVs, const uint32_t* firstSRVElements,
	uint32_t numUAVs, const uint32_t* firstUAVElements,
	const wchar_t* name)
{
	const auto hasSRV = !(resourceFlags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
	const bool hasUAV = resourceFlags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	// Determine initial state
	if (state == 0)
	{
		state = hasSRV ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE :
			D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
		state = hasUAV ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : state;
	}

	N_RETURN(StructuredBuffer::Create(device, numVertices, stride, resourceFlags, memoryType,
		state, numSRVs, firstSRVElements, numUAVs, firstUAVElements, name), false);

	// Create vertex buffer view
	m_vbvs.resize(numVBVs);
	for (auto i = 0u; i < numVBVs; ++i)
	{
		const auto firstVertex = firstVertices ? firstVertices[i] : 0;
		m_vbvs[i].BufferLocation = m_resource->GetGPUVirtualAddress() + stride * firstVertex;
		m_vbvs[i].StrideInBytes = stride;
		m_vbvs[i].SizeInBytes = stride * ((!firstVertices || i + 1 >= numVBVs ?
			numVertices : firstVertices[i + 1]) - firstVertex);
	}

	return true;
}

bool VertexBuffer::CreateAsRaw(const Device& device, uint32_t numVertices, uint32_t stride,
	ResourceFlags resourceFlags, MemoryType memoryType, ResourceState state,
	uint32_t numVBVs, const uint32_t* firstVertices,
	uint32_t numSRVs, const uint32_t* firstSRVElements,
	uint32_t numUAVs, const uint32_t* firstUAVElements,
	const wchar_t* name)
{
	const auto hasSRV = !(resourceFlags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
	const bool hasUAV = resourceFlags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	// Determine initial state
	if (state == 0)
	{
		state = hasSRV ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE :
			D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
		state = hasUAV ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : state;
	}

	N_RETURN(RawBuffer::Create(device, stride * numVertices, resourceFlags, memoryType,
		state, numSRVs, firstSRVElements, numUAVs, firstUAVElements, name), false);

	// Create vertex buffer view
	m_vbvs.resize(numVBVs);
	for (auto i = 0u; i < numVBVs; ++i)
	{
		const auto firstVertex = firstVertices ? firstVertices[i] : 0;
		m_vbvs[i].BufferLocation = m_resource->GetGPUVirtualAddress() + stride * firstVertex;
		m_vbvs[i].StrideInBytes = stride;
		m_vbvs[i].SizeInBytes = stride * ((!firstVertices || i + 1 >= numVBVs ?
			numVertices : firstVertices[i + 1]) - firstVertex);
	}

	return true;
}

VertexBufferView VertexBuffer::GetVBV(uint32_t i) const
{
	return m_vbvs.size() > i ? m_vbvs[i] : VertexBufferView();
}

//--------------------------------------------------------------------------------------
// Index buffer
//--------------------------------------------------------------------------------------

IndexBuffer::IndexBuffer() :
	TypedBuffer(),
	m_ibvs(0)
{
}

IndexBuffer::~IndexBuffer()
{
}

bool IndexBuffer::Create(const Device& device, uint64_t byteWidth, Format format,
	ResourceFlags resourceFlags, MemoryType memoryType, ResourceState state,
	uint32_t numIBVs, const uint32_t* offsets,
	uint32_t numSRVs, const uint32_t* firstSRVElements,
	uint32_t numUAVs, const uint32_t* firstUAVElements,
	const wchar_t* name)
{
	setDevice(device);

	const auto hasSRV = !(resourceFlags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
	const bool hasUAV = resourceFlags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	assert(format == DXGI_FORMAT_R32_UINT || format == DXGI_FORMAT_R16_UINT);
	const uint32_t stride = format == DXGI_FORMAT_R32_UINT ? sizeof(uint32_t) : sizeof(uint16_t);

	// Determine initial state
	state = state ? state : (hasSRV ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE :
		D3D12_RESOURCE_STATE_INDEX_BUFFER);

	const auto numElements = static_cast<uint32_t>(byteWidth / stride);
	N_RETURN(TypedBuffer::Create(device, numElements, stride, format, resourceFlags,
		memoryType, state, numSRVs, firstSRVElements, numUAVs, firstUAVElements, name), false);

	// Create index buffer view
	m_ibvs.resize(numIBVs);
	const auto maxSize = static_cast<uint32_t>(byteWidth);
	for (auto i = 0u; i < numIBVs; ++i)
	{
		const auto offset = offsets ? offsets[i] : 0;
		m_ibvs[i].BufferLocation = m_resource->GetGPUVirtualAddress() + offset;
		m_ibvs[i].SizeInBytes = (!offsets || i + 1 >= numIBVs ?
			maxSize : offsets[i + 1]) - offset;
		m_ibvs[i].Format = format;
	}

	return true;
}

IndexBufferView IndexBuffer::GetIBV(uint32_t i) const
{
	return m_ibvs.size() > i ? m_ibvs[i] : IndexBufferView();
}
