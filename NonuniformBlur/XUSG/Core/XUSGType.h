//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

#pragma once

#define H_RETURN(x, o, m, r)	{ const auto hr = x; if (FAILED(hr)) { o << m << endl; return r; } }
#define V_RETURN(x, o, r)		H_RETURN(x, o, HrToString(hr), r)

#define M_RETURN(x, o, m, r)	if (x) { o << m << endl; return r; }
#define F_RETURN(x, o, h, r)	M_RETURN(x, o, HrToString(h), r)

#define C_RETURN(x, r)			if (x) return r
#define N_RETURN(x, r)			C_RETURN(!(x), r)
#define X_RETURN(x, f, r)		{ x = f; N_RETURN(x, r); }

namespace XUSG
{
	template <typename T>
	class com_ptr :
		public Microsoft::WRL::ComPtr<T>
	{
	public:
		using element_type = T;

		com_ptr() : Microsoft::WRL::ComPtr<T>::ComPtr() {}
		com_ptr(decltype(__nullptr) null) : Microsoft::WRL::ComPtr<T>::ComPtr(null) {}

		template<class U>
		com_ptr(U* other) : Microsoft::WRL::ComPtr<T>::ComPtr(other) {}
		com_ptr(const Microsoft::WRL::ComPtr<T>& other) : Microsoft::WRL::ComPtr<T>::ComPtr(other) {}

		template<class U>
		com_ptr(const Microsoft::WRL::ComPtr<U>& other, typename Microsoft::WRL::Details::EnableIf<__is_convertible_to(U*, T*), void*>::type* t = 0) :
			Microsoft::WRL::ComPtr<T>::ComPtr(other, t) {}
		com_ptr(Microsoft::WRL::ComPtr<T>&& other) : Microsoft::WRL::ComPtr<T>::ComPtr(other) {}

		template<class U>
		com_ptr(Microsoft::WRL::ComPtr<U>&& other, typename Microsoft::WRL::Details::EnableIf<__is_convertible_to(U*, T*), void*>::type* t = 0) :
			Microsoft::WRL::ComPtr<T>::ComPtr(other, t) {}

		T* get() const { return Microsoft::WRL::ComPtr<T>::Get(); }
	};

	// Device and blobs
	using BlobType = ID3DBlob;
	using Blob = com_ptr<BlobType>;
	using Device = com_ptr<ID3D12Device>;
	using SwapChain = com_ptr<IDXGISwapChain3>;

	// Command lists related
	using GraphicsCommandList = com_ptr<ID3D12GraphicsCommandList>;
	using CommandAllocator = com_ptr<ID3D12CommandAllocator>;
	using CommandQueue = com_ptr<ID3D12CommandQueue>;
	using Fence = com_ptr<ID3D12Fence>;

	// Resources related
	using Resource = com_ptr<ID3D12Resource>;
	using VertexBufferView = D3D12_VERTEX_BUFFER_VIEW;
	using IndexBufferView = D3D12_INDEX_BUFFER_VIEW;
	using StreamOutBufferView = D3D12_STREAM_OUTPUT_BUFFER_VIEW;
	using Sampler = D3D12_SAMPLER_DESC;

	using ResourceState = D3D12_RESOURCE_STATES;
	using ResourceBarrier = CD3DX12_RESOURCE_BARRIER;
	using BarrierFlags = D3D12_RESOURCE_BARRIER_FLAGS;

	// Descriptors related
	using DescriptorPool = com_ptr<ID3D12DescriptorHeap>;
	using Descriptor = CD3DX12_CPU_DESCRIPTOR_HANDLE;
	using DescriptorView = CD3DX12_GPU_DESCRIPTOR_HANDLE;
	using DescriptorTable = std::shared_ptr<DescriptorView>;
	using RenderTargetTable = std::shared_ptr<Descriptor>;

	// Pipeline layouts related
	using PipelineLayout = com_ptr<ID3D12RootSignature>;
	using DescriptorRangeList = std::vector<CD3DX12_DESCRIPTOR_RANGE1>;

	// Input layouts related
	using InputElementTable = std::vector<D3D12_INPUT_ELEMENT_DESC>;
	struct InputLayoutDesc : D3D12_INPUT_LAYOUT_DESC
	{
		InputElementTable elements;
	};
	using InputLayout = std::shared_ptr<InputLayoutDesc>;

	// Primitive related
	using PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE;
	using PrimitiveTopology = D3D12_PRIMITIVE_TOPOLOGY;

	// Format and resources related
	using Format = DXGI_FORMAT;
	using MemoryType = D3D12_HEAP_TYPE;
	using ResourceFlags = D3D12_RESOURCE_FLAGS;
	using SubresourceData = D3D12_SUBRESOURCE_DATA;
	using ClearFlags = D3D12_CLEAR_FLAGS;
	using TextureCopyLocation = CD3DX12_TEXTURE_COPY_LOCATION;
	using Viewport = CD3DX12_VIEWPORT;
	using RectRange = CD3DX12_RECT;
	using BoxRange = CD3DX12_BOX;
	using TiledResourceCoord = CD3DX12_TILED_RESOURCE_COORDINATE;
	using TileRegionSize = D3D12_TILE_REGION_SIZE;
	using TileCopyFlags = D3D12_TILE_COPY_FLAGS;

	// Pipeline layouts related
	struct RootParameter : CD3DX12_ROOT_PARAMETER1
	{
		DescriptorRangeList ranges;
	};
	using DescriptorTableLayout = std::shared_ptr<RootParameter>;

	using Pipeline = com_ptr<ID3D12PipelineState>;

	// Shaders related
	namespace Shader
	{
		using ByteCode = CD3DX12_SHADER_BYTECODE;
		using Reflection = com_ptr<ID3D12ShaderReflection>;
	}

	// Graphics pipelines related
	namespace Graphics
	{
		using PipelineDesc = D3D12_GRAPHICS_PIPELINE_STATE_DESC;

		using Blend = std::shared_ptr<D3D12_BLEND_DESC>;
		using Rasterizer = std::shared_ptr < D3D12_RASTERIZER_DESC>;
		using DepthStencil = std::shared_ptr < D3D12_DEPTH_STENCIL_DESC>;
	}

	// Compute pipelines related
	namespace Compute
	{
		using PipelineDesc = D3D12_COMPUTE_PIPELINE_STATE_DESC;
	}
}
