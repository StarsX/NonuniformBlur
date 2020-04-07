//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#define MAX_LEVEL_COUNT	12
#define PI 3.141592654

//--------------------------------------------------------------------------------------
// Constant buffer
//--------------------------------------------------------------------------------------
cbuffer cb
{
	float2	g_focus;
	float	g_sigma;
	uint	g_level;
};

//--------------------------------------------------------------------------------------
// Texture samplers
//--------------------------------------------------------------------------------------
SamplerState	g_smpLinear;

//--------------------------------------------------------------------------------------
// Calculate Gaussian exponent
//--------------------------------------------------------------------------------------
float GaussianExp(float sigma2, uint level)
{
	return -(1 << (level << 1)) / (2.0 * PI * sigma2);
};

//--------------------------------------------------------------------------------------
// Calculate Gaussian basis
//--------------------------------------------------------------------------------------
float GaussianBasis(float sigma2, uint level)
{
	return level < 0.0 ? 0.0 : exp(GaussianExp(sigma2, level));
};

//--------------------------------------------------------------------------------------
// Calculate MIP Gaussian weight
//--------------------------------------------------------------------------------------
float MipGaussianWeight(float sigma2, uint level)
{
	const float g = GaussianBasis(sigma2, level);

	return (1 << (level << 2)) * g;
}

//--------------------------------------------------------------------------------------
// Calculate blending weight
//--------------------------------------------------------------------------------------
float MipGaussianBlendWeight(float2 tex)
{
	// Compute deviation
	float sigma = g_sigma;
	if (asuint(g_focus.x) != 0xffffffff)
	{
		const float2 r = (2.0 * tex - 1.0) - g_focus;
		sigma *= dot(r, r);
	}
	const float sigma2 = sigma * sigma;

	// Gaussian-approximating Haar coefficients (weights of box filters)
#ifdef _PREINTEGRATED_
	const float c = 2.0 * PI * sigma2;
	//const float numerator = pow(16.0, g_level) * log(4.0);
	//const float denorminator = c * (pow(4.0, g_level) + c);
	//const float numerator = pow(2.0, g_level * 4.0) * log(4.0);
	//const float denorminator = c * (pow(2.0, g_level * 2.0) + c);
	//const float numerator = (1 << (g_level * 4)) * log(4.0);
	//const float denorminator = c * ((1 << (g_level * 2)) + c);
	const float numerator = (1 << (g_level << 2)) * log(4.0);
	const float denorminator = c * ((1 << (g_level << 1)) + c);

	return saturate(numerator / denorminator);
#else
	float wsum = 0.0, weight = 0.0;
	for (uint i = g_level; i < MAX_LEVEL_COUNT; ++i)
	{
		const float w = MipGaussianWeight(sigma2, i);
		weight = i == g_level ? w : weight;
		wsum += w;
	}

	return wsum > 0.0 ? weight / wsum : 1.0;
#endif
}
