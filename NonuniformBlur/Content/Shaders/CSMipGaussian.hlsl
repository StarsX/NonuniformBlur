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
	uint	g_numLevels;
};

//--------------------------------------------------------------------------------------
// Textures
//--------------------------------------------------------------------------------------
Texture2D			g_txSource;
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

	float4 srcs[12];
	const float2 tex = (DTid + 0.5) / dim;
	for (uint i = 0; i < g_numLevels; ++i)
		srcs[i] = g_txSource.SampleLevel(g_smpLinear, tex, i);

	// Compute deviation
	const float2 r = (2.0 * tex - 1.0) - g_focus;
	const float s = saturate(dot(r, r) + 0.25);
	const float sigma = g_sigma * s;
	const float sigma2 = sigma * sigma;

	float wsum = 0.0;
	float4 result = 0.0;
	for (i = 0; i < g_numLevels; ++i)
	{
		const float w = MipGaussianWeight(sigma2, i);
		result += srcs[i] * w;
		wsum += w;
	}

	g_txDest[DTid] = wsum > 0.0 ? result / wsum : srcs[0];
}
