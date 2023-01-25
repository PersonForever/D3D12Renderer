#include "pch.h"
#include "heightmap_collider.h"


void terrain_collider_context::update(vec3 minCorner, float amplitudeScale)
{
	this->minCorner = minCorner;
	this->invAmplitudeScale = 1.f / amplitudeScale;
	this->heightScale = amplitudeScale / UINT16_MAX;
}

void heightmap_collider::setHeights(uint16* heights)
{
	this->heights = heights;

	uint32 numSegments = numVerticesPerDim - 1;
	uint32 numMips = log2(numSegments) + 1;

	mips.resize(numMips);




	{
		uint32 readStride = numVerticesPerDim;

		auto& mip = mips.front();
		mip.resize(numSegments * numSegments);
		for (uint32 z = 0; z < numSegments; ++z)
		{
			for (uint32 x = 0; x < numSegments; ++x)
			{
				uint32 aIndex = (readStride * z + x);
				uint32 bIndex = (readStride * (z + 1) + x);
				uint32 cIndex = (readStride * z + x + 1);
				uint32 dIndex = (readStride * (z + 1) + x + 1);

				uint16 a = heights[aIndex];
				uint16 b = heights[bIndex];
				uint16 c = heights[cIndex];
				uint16 d = heights[dIndex];

				heightmap_min_max v =
				{
					min(a, min(b, min(c, d))),
					max(a, max(b, max(c, d))),
				};

				mip[numSegments * z + x] = v;
			}
		}
	}

	for (uint32 i = 1; i < numMips; ++i)
	{
		uint32 readStride = numSegments;

		numSegments >>= 1;

		auto& readMip = mips[i - 1];
		auto& writeMip = mips[i];
		writeMip.resize(numSegments * numSegments);
		for (uint32 z = 0; z < numSegments; ++z)
		{
			for (uint32 x = 0; x < numSegments; ++x)
			{
				uint32 x0 = x * 2;
				uint32 x1 = x * 2 + 1;
				uint32 z0 = z * 2;
				uint32 z1 = z * 2 + 1;

				heightmap_min_max a = readMip[readStride * z0 + x0];
				heightmap_min_max b = readMip[readStride * z1 + x0];
				heightmap_min_max c = readMip[readStride * z0 + x1];
				heightmap_min_max d = readMip[readStride * z1 + x1];

				heightmap_min_max v =
				{
					min(a.min, min(b.min, min(c.min, d.min))),
					max(a.max, max(b.max, max(c.max, d.max))),
				};

				writeMip[numSegments * z + x] = v;
			}
		}
	}
}
