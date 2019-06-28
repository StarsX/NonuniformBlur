//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

#define PI 3.141592654

float GaussianExp(float sigma2, float mip)
{
	return -exp2(2.0 * mip - 1.0) / (PI * sigma2);
};

float GaussianBasis(float sigma2, float mip)
{
	return mip < 0.0 ? 0.0 : exp(GaussianExp(sigma2, mip));
};

float MipGaussianWeight(uint mip, float2 g)
{
	return (1 << (2 * mip)) * (g.x - g.y);
}

float MipWeight(float sigma2, uint mip, int mipStep = 1)
{
	const float2 g =
	{
		GaussianBasis(sigma2, mip),
		GaussianBasis(sigma2, mipStep > 0 ? mip + mipStep : -1)
	};

	return (1 << (2 * mip)) * (g.x - g.y);
};
