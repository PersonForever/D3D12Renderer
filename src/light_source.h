#pragma once

#include "math.h"
#include "camera.h"

#include "light_source.hlsli"

#define SHADOW_MAP_NEGATIVE_Z_OFFSET 1000.f

#define MAX_NUM_POINT_LIGHTS_PER_FRAME 4096
#define MAX_NUM_SPOT_LIGHTS_PER_FRAME 4096

#define MAX_NUM_SPOT_LIGHT_SHADOW_PASSES 16

struct directional_light
{
	vec3 direction;
	uint32 numShadowCascades;

	vec3 color;
	float intensity; // Final radiance is color * intensity.

	vec4 cascadeDistances;
	vec4 bias;

	mat4 vp[MAX_NUM_SUN_SHADOW_CASCADES];

	vec4 blendDistances;


	// 'preventRotationalShimmering' uses bounding spheres instead of bounding boxes. 
	// This prevents shimmering along shadow edges, when the camera rotates.
	// It slightly reduces shadow map resolution though.
	void updateMatrices(const render_camera& camera, bool preventRotationalShimmering = true);
};

mat4 getSpotLightViewProjectionMatrix(const spot_light_cb& sl);

