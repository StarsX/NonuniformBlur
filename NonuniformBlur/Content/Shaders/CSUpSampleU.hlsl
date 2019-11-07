//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

#include "MipGaussian.hlsli"
#include "D3DX_DXGIFormatConvert.inl"

//--------------------------------------------------------------------------------------
// Textures
//--------------------------------------------------------------------------------------
Texture2D			g_txCoarser;
RWTexture2D<uint>	g_txDest;

//--------------------------------------------------------------------------------------
// Compute shader
//--------------------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void main(uint2 DTid : SV_DispatchThreadID)
{
	float2 dim;
	g_txDest.GetDimensions(dim.x, dim.y);

	// Fetch the color of the current level and the resolved color at the coarser level
	const float2 tex = (DTid + 0.5) / dim;
	const float4 src = D3DX_B8G8R8A8_UNORM_to_FLOAT4(g_txDest[DTid]);
	const float4 coarser = g_txCoarser.SampleLevel(g_smpLinear, tex, 0.0);

	const float weight = MipGaussianBlendWeight(tex);

	g_txDest[DTid] = D3DX_FLOAT4_to_B8G8R8A8_UNORM(lerp(coarser, src, weight));
}
