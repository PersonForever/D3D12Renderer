#pragma once

#include "geometry/mesh.h"
#include "shadow_map_cache.h"

struct particle_draw_info
{
	ref<dx_buffer> particleBuffer;
	ref<dx_buffer> aliveList;
	ref<dx_buffer> commandBuffer;
	uint32 aliveListOffset;
	uint32 commandBufferOffset;
	uint32 rootParameterOffset;
};

template <typename render_data_t>
struct particle_render_command
{
	dx_vertex_buffer_group_view vertexBuffer;
	dx_index_buffer_view indexBuffer;
	particle_draw_info drawInfo;

	render_data_t data;
};




