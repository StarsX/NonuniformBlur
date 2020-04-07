//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

//--------------------------------------------------------------------------------------
// Textures
//--------------------------------------------------------------------------------------
Texture2D		g_txSource;

//--------------------------------------------------------------------------------------
// Texture samplers
//--------------------------------------------------------------------------------------
SamplerState	g_smpLinear;

//--------------------------------------------------------------------------------------
// Compute shader
//--------------------------------------------------------------------------------------
float4 Resample(float2 tex)
{
#ifdef _HIGH_QUALITY_
	float4 srcs[5];
	srcs[0] = g_txSource.SampleLevel(g_smpLinear, tex, 0.0);
	srcs[1] = g_txSource.SampleLevel(g_smpLinear, tex, 0.0, int2(-1, 0));
	srcs[2] = g_txSource.SampleLevel(g_smpLinear, tex, 0.0, int2(1, 0));
	srcs[3] = g_txSource.SampleLevel(g_smpLinear, tex, 0.0, int2(0, -1));
	srcs[4] = g_txSource.SampleLevel(g_smpLinear, tex, 0.0, int2(0, 1));

	float4 result = srcs[0] * 2.0;
	[unroll]
	for (uint i = 1; i < 5; ++i) result += srcs[i];
	result /= 6.0;
#else
	const float4 result = g_txSource.SampleLevel(g_smpLinear, tex, 0.0);
#endif

	return result;
}
