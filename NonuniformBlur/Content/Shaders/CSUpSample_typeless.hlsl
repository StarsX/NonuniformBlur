//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
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
	float2 imageSize;
	g_txDest.GetDimensions(imageSize.x, imageSize.y);

	// Fetch the color of the current level and the resolved color at the coarser level
	const float2 uv = (DTid + 0.5) / imageSize;
	const float4 src = D3DX_R8G8B8A8_UNORM_to_FLOAT4(g_txDest[DTid]);
	const float4 coarser = g_txCoarser.SampleLevel(g_smpLinear, uv, 0.0);

	// Gaussian-approximating Haar coefficients (weights of box filters)
	const float weight = MipGaussianBlendWeight(uv);

	g_txDest[DTid] = D3DX_FLOAT4_to_R8G8B8A8_UNORM(lerp(coarser, src, weight));
}
