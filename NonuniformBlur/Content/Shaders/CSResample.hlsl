//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

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
void main(uint2 DTid : SV_DispatchThreadID )
{
	float2 dim;
	g_txDest.GetDimensions(dim.x, dim.y);

	const float2 tex = (DTid + 0.5) / dim;

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

	g_txDest[DTid] = result;
}
