#pragma once

#include "raytracer.h"

#include "material.hlsli"
#include "pbr.h"

struct path_tracer : dx_raytracer
{
    void initialize();
    raytracing_object_type defineObjectType(const ref<raytracing_blas>& blas, const std::vector<ref<pbr_material>>& materials);

    void finish();

    void render(dx_command_list* cl, const raytracing_tlas& tlas,
        const ref<dx_texture>& output,
        const common_material_info& materialInfo) override;



    const uint32 maxRecursionDepth = 8;
    const uint32 maxPayloadSize = 5 * sizeof(float); // Radiance-payload is 1 x float3, 2 x uint.



    // Parameters.

    uint32 numAveragedFrames = 0;
    uint32 recursionDepth = 3; // [0, maxRecursionDepth - 1].

    bool useThinLensCamera = false;
    float fNumber = 32.f;
    float focalLength = 1.f;

    bool useRealMaterials = false;
    bool enableDirectLighting = false;
    float lightIntensityScale = 1.f;
    float pointLightRadius = 0.1f;



private:
    struct shader_data // This struct is 32 bytes large, which together with the 32 byte shader identifier is a nice multiple of the required 32-byte-alignment of the binding table entries.
    {
        pbr_material_cb materialCB;
        dx_cpu_descriptor_handle resources; // Vertex buffer, index buffer, pbr textures.
    };

    struct global_resources
    {
        union
        {
            struct
            {
                // Must be first.
                dx_cpu_descriptor_handle output;


                dx_cpu_descriptor_handle tlas;
                dx_cpu_descriptor_handle sky;
            };

            dx_cpu_descriptor_handle resources[3];
        };

        dx_cpu_descriptor_handle cpuBase;
        dx_gpu_descriptor_handle gpuBase;
    };

    dx_pushable_resource_descriptor_heap descriptorHeap;

    uint32 instanceContributionToHitGroupIndex = 0;
    uint32 numRayTypes;

    raytracing_binding_table<shader_data> bindingTable;

    global_resources globalResources[NUM_BUFFERED_FRAMES];
};
