//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

#define PI 3.141592654

float GaussianExp(float sigma2, uint level)
{
	return -(1 << (level << 1)) / (2.0 * PI * sigma2);
};

float GaussianBasis(float sigma2, uint level)
{
	return level < 0.0 ? 0.0 : exp(GaussianExp(sigma2, level));
};

float MipGaussianWeight(float sigma2, uint level)
{
	const float g = GaussianBasis(sigma2, level);

	return (1 << (level << 2)) * g;
}
