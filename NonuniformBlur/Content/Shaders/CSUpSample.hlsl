//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

#include "CSMipGaussian.hlsli"

//--------------------------------------------------------------------------------------
// Constant buffer
//--------------------------------------------------------------------------------------
cbuffer cb
{
	float2	g_focus;
	float	g_sigma;
	uint	g_levelData;
};

//--------------------------------------------------------------------------------------
// Textures
//--------------------------------------------------------------------------------------
Texture2D			g_txSource;
Texture2D			g_txCoarser;
RWTexture2D<float4>	g_txDest;

//--------------------------------------------------------------------------------------
// Texture samplers
//--------------------------------------------------------------------------------------
SamplerState	g_smpLinear;

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
	const float4 src = g_txSource.SampleLevel(g_smpLinear, tex, 0);
	const float4 coarser = g_txCoarser.SampleLevel(g_smpLinear, tex, 0);

	// Compute deviation
	const float2 r = (2.0 * tex - 1.0) - g_focus;
	const float s = saturate(dot(r, r) + 0.25);
	const float sigma = g_sigma * s;
	const float sigma2 = sigma * sigma;

	// Decode mip level and total number of levels
	const uint level = g_levelData & 0xffff;
	const uint numLevels = g_levelData >> 16;

	// Gaussian-approximating Haar coefficients (weights of box filters)
#if 1
	float wsum = 0.0, weight = 0.0;
	for (uint i = level; i < numLevels; ++i)
	{
		// Compute next term
		const float g = GaussianBasis(sigma2, i);
		const float w = (1 << (i << 2)) * g;
		weight = i == level ? w : weight;
		wsum += w;
	}

	weight = wsum > 0.0 ? weight / wsum : 1.0;
#else
	float2 g = { GaussianBasis(sigma2, level), 0.0 };
	float wsum = 0.0, weight = 0.0;
	for (uint i = level; i < numLevels; ++i)
	{
		// Compute next term
		g.y = GaussianBasis(sigma2, i + 1.0);

		const float w = MipGaussianWeight(i, g);
		weight = i == level ? w : weight;
		wsum += w;

		// For next iteration
		g.x = g.y;
	}

	weight = wsum > 0.0 ? weight / wsum : 1.0;
#endif

	g_txDest[DTid] = lerp(coarser, src, weight);
}
