//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "MipGaussian.hlsli"

//--------------------------------------------------------------------------------------
// Textures
//--------------------------------------------------------------------------------------
Texture2D			g_txCoarser;
RWTexture2D<float4>	g_txDest;

//--------------------------------------------------------------------------------------
// Compute shader
//--------------------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void main(uint2 DTid : SV_DispatchThreadID)
{
	float2 imageSize;
	g_txDest.GetDimensions(imageSize.x, imageSize.y);

	// Fetch the color of the current level and the resolved color at the coarser level
	const float2 tex = (DTid + 0.5) / imageSize;
	const float4 src = g_txDest[DTid];
	const float4 coarser = g_txCoarser.SampleLevel(g_smpLinear, tex, 0.0);

	// Gaussian-approximating Haar coefficients (weights of box filters)
	const float weight = MipGaussianBlendWeight(tex);

	g_txDest[DTid] = lerp(coarser, src, weight);
}
