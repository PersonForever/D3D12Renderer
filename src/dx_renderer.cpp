#include "pch.h"
#include "dx_renderer.h"
#include "dx_command_list.h"
#include "dx_pipeline.h"
#include "geometry.h"
#include "imgui.h"
#include "texture.h"
#include "texture_preprocessing.h"

#include "outline_rs.hlsl"
#include "present_rs.hlsl"
#include "sky_rs.hlsl"
#include "light_culling_rs.hlsl"

#include "camera.hlsl"


static dx_texture whiteTexture;
static dx_buffer pointLightBuffer[NUM_BUFFERED_FRAMES];
static dx_buffer spotLightBuffer[NUM_BUFFERED_FRAMES];

static dx_render_target sunShadowRenderTarget[MAX_NUM_SUN_SHADOW_CASCADES];
static dx_texture sunShadowCascadeTextures[MAX_NUM_SUN_SHADOW_CASCADES];

static dx_pipeline textureSkyPipeline;
static dx_pipeline proceduralSkyPipeline;
static dx_pipeline presentPipeline;
static dx_pipeline modelPipeline;
static dx_pipeline modelDepthOnlyPipeline;
static dx_pipeline modelShadowPipeline; // Only different from depth-only pipeline in the depth format.
static dx_pipeline outlinePipeline;
static dx_pipeline flatUnlitPipeline;
static dx_pipeline blitPipeline;

static dx_pipeline worldSpaceFrustaPipeline;
static dx_pipeline lightCullingPipeline;

static dx_mesh gizmoMesh;
static dx_mesh positionOnlyMesh;

static union
{
	struct
	{
		submesh_info noneGizmoSubmesh;
		submesh_info translationGizmoSubmesh;
		submesh_info rotationGizmoSubmesh;
		submesh_info scaleGizmoSubmesh;
	};

	submesh_info gizmoSubmeshes[4];
};

static submesh_info cubeMesh;
static submesh_info sphereMesh;


static quat gizmoRotations[] =
{
	quat(vec3(0.f, 0.f, -1.f), deg2rad(90.f)),
	quat::identity,
	quat(vec3(1.f, 0.f, 0.f), deg2rad(90.f)),
};

static vec4 gizmoColors[] =
{
	vec4(1.f, 0.f, 0.f, 1.f),
	vec4(0.f, 1.f, 0.f, 1.f),
	vec4(0.f, 0.f, 1.f, 1.f),
};


static dx_texture brdfTex;
static tonemap_cb tonemap = defaultTonemapParameters();



static DXGI_FORMAT screenFormat;

static const DXGI_FORMAT hdrFormat[] = { DXGI_FORMAT_R8G8B8A8_UNORM };
static const DXGI_FORMAT hdrDepthFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
static const DXGI_FORMAT shadowDepthFormat = DXGI_FORMAT_D32_FLOAT;


void dx_renderer::initializeCommon(DXGI_FORMAT screenFormat)
{
	::screenFormat = screenFormat;

	uint8 white[] = { 255, 255, 255, 255 };
	whiteTexture = createTexture(white, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM);

	initializeTexturePreprocessing();

	for (uint32 i = 0; i < NUM_BUFFERED_FRAMES; ++i)
	{
		pointLightBuffer[i] = createUploadBuffer(sizeof(point_light_cb), MAX_NUM_POINT_LIGHTS_PER_FRAME, 0);
		spotLightBuffer[i] = createUploadBuffer(sizeof(spot_light_cb), MAX_NUM_SPOT_LIGHTS_PER_FRAME, 0);
	}


	for (uint32 i = 0; i < MAX_NUM_SUN_SHADOW_CASCADES; ++i)
	{
		sunShadowCascadeTextures[i] = createDepthTexture(SUN_SHADOW_DIMENSIONS, SUN_SHADOW_DIMENSIONS, shadowDepthFormat, 1, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
		sunShadowRenderTarget[i].pushDepthStencilAttachment(sunShadowCascadeTextures[i]);
	}


	// Sky.
	{
		auto desc = CREATE_GRAPHICS_PIPELINE
			.inputLayout(inputLayout_position)
			.renderTargets(hdrFormat, arraysize(hdrFormat))
			.depthSettings(false, false)
			.cullFrontFaces();

		proceduralSkyPipeline = createReloadablePipeline(desc, { "sky_vs", "sky_procedural_ps" });
		textureSkyPipeline = createReloadablePipeline(desc, { "sky_vs", "sky_texture_ps" });
	}

	// Model.
	{
		auto desc = CREATE_GRAPHICS_PIPELINE
			.renderTargets(0, 0, hdrDepthFormat)
			.inputLayout(inputLayout_position_uv_normal_tangent);

		modelDepthOnlyPipeline = createReloadablePipeline(desc, { "model_vs" }, "model_vs"); // The depth-only RS is baked into the vertex shader.

		desc
			.renderTargets(0, 0, shadowDepthFormat);

		modelShadowPipeline = createReloadablePipeline(desc, { "model_vs" }, "model_vs"); // The depth-only RS is baked into the vertex shader.

		desc
			.renderTargets(hdrFormat, arraysize(hdrFormat), hdrDepthFormat)
			.stencilSettings(D3D12_COMPARISON_FUNC_ALWAYS, D3D12_STENCIL_OP_REPLACE, D3D12_STENCIL_OP_REPLACE) // Mark areas in stencil, for example for outline.
			.depthSettings(true, false, D3D12_COMPARISON_FUNC_EQUAL);

		modelPipeline = createReloadablePipeline(desc, { "model_vs", "model_ps" });
	}

	// Outline.
	{
		auto desc = CREATE_GRAPHICS_PIPELINE
			.inputLayout(inputLayout_position_uv_normal_tangent)
			.renderTargets(hdrFormat, arraysize(hdrFormat), hdrDepthFormat)
			.stencilSettings(D3D12_COMPARISON_FUNC_NOT_EQUAL);

		outlinePipeline = createReloadablePipeline(desc, { "outline_vs", "outline_ps" }, "outline_vs");
	}

	// Flat unlit.
	{
		auto desc = CREATE_GRAPHICS_PIPELINE
			.inputLayout(inputLayout_position)
			.renderTargets(hdrFormat, arraysize(hdrFormat), hdrDepthFormat)
			.wireframe();

		flatUnlitPipeline = createReloadablePipeline(desc, { "flat_unlit_vs", "flat_unlit_ps" }, "flat_unlit_vs");
	}

	// Present.
	{
		auto desc = CREATE_GRAPHICS_PIPELINE
			.renderTargets(&screenFormat, 1)
			.depthSettings(false, false);

		presentPipeline = createReloadablePipeline(desc, { "fullscreen_triangle_vs", "present_ps" });
	}

	// Blit.
	{
		auto desc = CREATE_GRAPHICS_PIPELINE
		.renderTargets(&screenFormat, 1)
		.depthSettings(false, false);

		blitPipeline = createReloadablePipeline(desc, { "fullscreen_triangle_vs", "blit_ps" });
	}

	// Light culling.
	{
		worldSpaceFrustaPipeline = createReloadablePipeline("world_space_tiled_frusta_cs");
		lightCullingPipeline = createReloadablePipeline("light_culling_cs");
	}

	createAllReloadablePipelines();



	{
		cpu_mesh mesh(mesh_creation_flags_with_positions);
		cubeMesh = mesh.pushCube(1.f);
		sphereMesh = mesh.pushSphere(10, 10, 1.f);
		positionOnlyMesh = mesh.createDXMesh();
	}

	{
		cpu_mesh mesh(mesh_creation_flags_with_positions | mesh_creation_flags_with_uvs | mesh_creation_flags_with_normals | mesh_creation_flags_with_tangents);
		float shaftLength = 2.f;
		float headLength = 0.4f;
		float radius = 0.06f;
		float headRadius = 0.13f;
		translationGizmoSubmesh = mesh.pushArrow(6, radius, headRadius, shaftLength, headLength);
		rotationGizmoSubmesh = mesh.pushTorus(6, 64, shaftLength, radius);
		scaleGizmoSubmesh = mesh.pushMace(6, radius, headRadius, shaftLength, headLength);
		gizmoMesh = mesh.createDXMesh();
	}

	{
		dx_command_list* cl = dxContext.getFreeRenderCommandList();
		brdfTex = integrateBRDF(cl);
		dxContext.executeCommandList(cl);
	}
}

void dx_renderer::initialize(uint32 windowWidth, uint32 windowHeight)
{
	this->windowWidth = windowWidth;
	this->windowHeight = windowHeight;

	recalculateViewport(false);

	// HDR render target.
	{
		depthBuffer = createDepthTexture(renderWidth, renderHeight, hdrDepthFormat);
		hdrColorTexture = createTexture(0, renderWidth, renderHeight, hdrFormat[0], true);

		hdrRenderTarget.pushColorAttachment(hdrColorTexture);
		hdrRenderTarget.pushDepthStencilAttachment(depthBuffer);
	}

	// Frame result.
	{
		frameResult = createTexture(0, windowWidth, windowHeight, screenFormat, true);

		windowRenderTarget.pushColorAttachment(frameResult);
	}
}

void dx_renderer::beginFrameCommon()
{

	checkForChangedPipelines();
}

void dx_renderer::beginFrame(uint32 windowWidth, uint32 windowHeight)
{
	pointLights = 0;
	spotLights = 0;
	numPointLights = 0;
	numSpotLights = 0;

	if (this->windowWidth != windowWidth || this->windowHeight != windowHeight)
	{
		this->windowWidth = windowWidth;
		this->windowHeight = windowHeight;

		// Frame result.
		{
			resizeTexture(frameResult, windowWidth, windowHeight);
			windowRenderTarget.notifyOnTextureResize(windowWidth, windowHeight);
		}

		recalculateViewport(true);
	}

	geometryRenderPass.reset();
	sunShadowRenderPass.reset();
}

pbr_environment dx_renderer::createEnvironment(const char* filename, uint32 skyResolution, uint32 environmentResolution, uint32 irradianceResolution)
{
	pbr_environment environment;

	dx_texture equiSky = loadTextureFromFile(filename,
		texture_load_flags_noncolor | texture_load_flags_cache_to_dds | texture_load_flags_allocate_full_mipchain);

	dxContext.renderQueue.waitForOtherQueue(dxContext.copyQueue);
	dx_command_list* cl = dxContext.getFreeRenderCommandList();
	generateMipMapsOnGPU(cl, equiSky);
	environment.sky = equirectangularToCubemap(cl, equiSky, skyResolution, 0, DXGI_FORMAT_R16G16B16A16_FLOAT);
	environment.environment = prefilterEnvironment(cl, environment.sky, environmentResolution);
	environment.irradiance = cubemapToIrradiance(cl, environment.sky, irradianceResolution);
	dxContext.executeCommandList(cl);

	dxContext.retireObject(equiSky.resource);

	return environment;
}

void dx_renderer::recalculateViewport(bool resizeTextures)
{
	if (aspectRatioMode == aspect_ratio_free)
	{
		windowViewport = { 0.f, 0.f, (float)windowWidth, (float)windowHeight, 0.f, 1.f };
	}
	else
	{
		const float targetAspect = aspectRatioMode == aspect_ratio_fix_16_9 ? (16.f / 9.f) : (16.f / 10.f);

		float aspect = (float)windowWidth / (float)windowHeight;
		if (aspect > targetAspect)
		{
			float width = windowHeight * targetAspect;
			float widthOffset = (windowWidth - width) * 0.5f;
			windowViewport = { widthOffset, 0.f, width, (float)windowHeight, 0.f, 1.f };
		}
		else
		{
			float height = windowWidth / targetAspect;
			float heightOffset = (windowHeight - height) * 0.5f;
			windowViewport = { 0.f, heightOffset, (float)windowWidth, height, 0.f, 1.f };
		}
	}

	renderWidth = (uint32)windowViewport.Width;
	renderHeight = (uint32)windowViewport.Height;

	if (resizeTextures)
	{
		resizeTexture(hdrColorTexture, renderWidth, renderHeight);
		resizeTexture(depthBuffer, renderWidth, renderHeight);
		hdrRenderTarget.notifyOnTextureResize(renderWidth, renderHeight);
	}

	allocateLightCullingBuffers();
}

void dx_renderer::allocateLightCullingBuffers()
{
	lightCullingBuffers.numTilesX = bucketize(renderWidth, LIGHT_CULLING_TILE_SIZE);
	lightCullingBuffers.numTilesY = bucketize(renderHeight, LIGHT_CULLING_TILE_SIZE);

	bool firstAllocation = lightCullingBuffers.lightGrid.resource == nullptr;

	if (firstAllocation)
	{
		lightCullingBuffers.lightGrid = createTexture(0, lightCullingBuffers.numTilesX, lightCullingBuffers.numTilesY,
			DXGI_FORMAT_R32G32B32A32_UINT, false, true);
		lightCullingBuffers.lightIndexCounter = createBuffer(sizeof(uint32), 2, 0, true, true);
		lightCullingBuffers.pointLightIndexList = createBuffer(sizeof(uint32),
			lightCullingBuffers.numTilesX * lightCullingBuffers.numTilesY * MAX_NUM_LIGHTS_PER_TILE, 0, true);
		lightCullingBuffers.spotLightIndexList = createBuffer(sizeof(uint32),
			lightCullingBuffers.numTilesX * lightCullingBuffers.numTilesY * MAX_NUM_LIGHTS_PER_TILE, 0, true);
		lightCullingBuffers.tiledFrusta = createBuffer(sizeof(light_culling_view_frustum), lightCullingBuffers.numTilesX * lightCullingBuffers.numTilesY, 0, true);
	}
	else
	{
		resizeTexture(lightCullingBuffers.lightGrid, lightCullingBuffers.numTilesX, lightCullingBuffers.numTilesY);
		resizeBuffer(lightCullingBuffers.pointLightIndexList, lightCullingBuffers.numTilesX * lightCullingBuffers.numTilesY * MAX_NUM_LIGHTS_PER_TILE);
		resizeBuffer(lightCullingBuffers.spotLightIndexList, lightCullingBuffers.numTilesX * lightCullingBuffers.numTilesY * MAX_NUM_LIGHTS_PER_TILE);
		resizeBuffer(lightCullingBuffers.tiledFrusta, lightCullingBuffers.numTilesX * lightCullingBuffers.numTilesY);
	}
}

void dx_renderer::setCamera(const render_camera& camera)
{
	this->camera.viewProj = camera.viewProj;
	this->camera.view = camera.view;
	this->camera.proj = camera.proj;
	this->camera.invViewProj = camera.invViewProj;
	this->camera.invView = camera.invView;
	this->camera.invProj = camera.invProj;
	this->camera.position = vec4(camera.position, 1.f);
	this->camera.forward = vec4(camera.rotation * vec3(0.f, 0.f, -1.f), 0.f);
	this->camera.projectionParams = vec4(camera.nearPlane, camera.farPlane, camera.farPlane / camera.nearPlane, 1.f - camera.farPlane / camera.nearPlane);
	this->camera.screenDims = vec2((float)renderWidth, (float)renderHeight);
	this->camera.invScreenDims = vec2(1.f / renderWidth, 1.f / renderHeight);
}

void dx_renderer::setEnvironment(const pbr_environment& environment)
{
	this->environment.sky = environment.sky.defaultSRV;
	this->environment.environment = environment.environment.defaultSRV;
	this->environment.irradiance = environment.irradiance.defaultSRV;
}

void dx_renderer::setSun(const directional_light& light)
{
	sun.cascadeDistances = light.cascadeDistances;
	sun.bias = light.bias;
	sun.direction = light.direction;
	sun.blendArea = light.blendArea;
	sun.radiance = light.radiance;
	sun.numShadowCascades = light.numShadowCascades;

	memcpy(sun.vp, light.vp, sizeof(mat4) * light.numShadowCascades);
}

void dx_renderer::setPointLights(const point_light_cb* lights, uint32 numLights)
{
	pointLights = lights;
	numPointLights = numLights;
}

void dx_renderer::setSpotLights(const spot_light_cb* lights, uint32 numLights)
{
	spotLights = lights;
	numSpotLights = numLights;
}

void dx_renderer::endFrame(dx_command_list* cl, float dt, bool mainWindow)
{
	static bool showLightVolumes = false;

	bool aspectRatioModeChanged = false;

	if (mainWindow)
	{
		DXGI_QUERY_VIDEO_MEMORY_INFO memoryInfo;
		checkResult(dxContext.adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memoryInfo));

		ImGui::Begin("Settings");
		ImGui::Text("%f ms, %u FPS", dt, (uint32)(1.f / dt));

		aspectRatioModeChanged = ImGui::Dropdown("Aspect ratio", aspectRatioNames, aspect_ratio_mode_count, (uint32&)aspectRatioMode);
		ImGui::Checkbox("Show light volumes", &showLightVolumes);
		ImGui::Text("Video memory available: %uMB", (uint32)BYTE_TO_MB(memoryInfo.Budget));
		ImGui::Text("Video memory used: %uMB", (uint32)BYTE_TO_MB(memoryInfo.CurrentUsage));

		if (ImGui::TreeNode("Tonemapping"))
		{
			ImGui::PlotLines("Tone map",
				[](void* data, int idx)
				{
					float t = idx * 0.01f;
					tonemap_cb& aces = *(tonemap_cb*)data;

					return filmicTonemapping(t, aces);
				},
				&tonemap, 100, 0, 0, 0.f, 1.f, ImVec2(100.f, 100.f));

			ImGui::SliderFloat("[ACES] Shoulder strength", &tonemap.A, 0.f, 1.f);
			ImGui::SliderFloat("[ACES] Linear strength", &tonemap.B, 0.f, 1.f);
			ImGui::SliderFloat("[ACES] Linear angle", &tonemap.C, 0.f, 1.f);
			ImGui::SliderFloat("[ACES] Toe strength", &tonemap.D, 0.f, 1.f);
			ImGui::SliderFloat("[ACES] Tone numerator", &tonemap.E, 0.f, 1.f);
			ImGui::SliderFloat("[ACES] Toe denominator", &tonemap.F, 0.f, 1.f);
			ImGui::SliderFloat("[ACES] Linear white", &tonemap.linearWhite, 0.f, 100.f);
			ImGui::SliderFloat("[ACES] Exposure", &tonemap.exposure, -3.f, 3.f);

			ImGui::TreePop();
		}

		ImGui::End();

		if (aspectRatioModeChanged)
		{
			recalculateViewport(true);
		}
	}

	auto cameraCBV = dxContext.uploadDynamicConstantBuffer(camera);
	auto sunCBV = dxContext.uploadDynamicConstantBuffer(sun);

	//dx_command_list* cl = dxContext.getFreeRenderCommandList();

	barrier_batcher(cl)
		.transition(hdrColorTexture.resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET)
		.transition(frameResult.resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET)
		.transition(sunShadowCascadeTextures, MAX_NUM_SUN_SHADOW_CASCADES, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);


	cl->setRenderTarget(hdrRenderTarget);
	cl->setViewport(hdrRenderTarget.viewport);
	cl->clearDepthAndStencil(hdrRenderTarget.dsvHandle);

	cl->setPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);



	// ----------------------------------------
	// SKY PASS
	// ----------------------------------------

	cl->setPipelineState(*textureSkyPipeline.pipeline);
	cl->setGraphicsRootSignature(*textureSkyPipeline.rootSignature);

	cl->setGraphics32BitConstants(SKY_RS_VP, sky_cb{ camera.proj * createSkyViewMatrix(camera.view) });
	cl->setDescriptorHeapSRV(SKY_RS_TEX, 0, environment.sky);

	cl->setVertexBuffer(0, positionOnlyMesh.vertexBuffer);
	cl->setIndexBuffer(positionOnlyMesh.indexBuffer);
	cl->drawIndexed(cubeMesh.numTriangles * 3, 1, cubeMesh.firstTriangle * 3, cubeMesh.baseVertex, 0);




	// ----------------------------------------
	// DEPTH-ONLY PASS
	// ----------------------------------------

	cl->setPipelineState(*modelDepthOnlyPipeline.pipeline);
	cl->setGraphicsRootSignature(*modelDepthOnlyPipeline.rootSignature);

	for (const geometry_render_pass::draw_call& dc : geometryRenderPass.drawCalls)
	{
		const mat4& m = dc.transform;
		const submesh_info& submesh = dc.submesh;
		const dx_mesh* mesh = dc.mesh;

		cl->setGraphics32BitConstants(MODEL_RS_MVP, transform_cb{ camera.viewProj * m, m });

		cl->setVertexBuffer(0, mesh->vertexBuffer);
		cl->setIndexBuffer(mesh->indexBuffer);
		cl->drawIndexed(submesh.numTriangles * 3, 1, submesh.firstTriangle * 3, submesh.baseVertex, 0);
	}

#if 0
	// Gizmos.
	if (gizmoType != gizmo_type_none)
	{
		cl->setVertexBuffer(0, gizmoMesh.vertexBuffer);
		cl->setIndexBuffer(gizmoMesh.indexBuffer);

		for (uint32 i = 0; i < 3; ++i)
		{
			mat4 m = createModelMatrix(meshTransform.position, gizmoRotations[i]);
			cl->setGraphics32BitConstants(MODEL_RS_MVP, transform_cb{ camera.viewProj * m, m });
			cl->drawIndexed(gizmoSubmeshes[gizmoType].numTriangles * 3, 1, gizmoSubmeshes[gizmoType].firstTriangle * 3, gizmoSubmeshes[gizmoType].baseVertex, 0);
		}
	}
#endif



	// ----------------------------------------
	// LIGHT CULLING
	// ----------------------------------------

	if (numPointLights || numSpotLights)
	{
		if (numPointLights)
		{
			point_light_cb* pls = (point_light_cb*)mapBuffer(pointLightBuffer[dxContext.bufferedFrameID]);
			memcpy(pls, pointLights, sizeof(point_light_cb) * numPointLights);
			unmapBuffer(pointLightBuffer[dxContext.bufferedFrameID]);
		}
		if (numSpotLights)
		{
			spot_light_cb* sls = (spot_light_cb*)mapBuffer(spotLightBuffer[dxContext.bufferedFrameID]);
			memcpy(sls, spotLights, sizeof(spot_light_cb) * numSpotLights);
			unmapBuffer(spotLightBuffer[dxContext.bufferedFrameID]);
		}


		// Tiled frusta.
		cl->setPipelineState(*worldSpaceFrustaPipeline.pipeline);
		cl->setComputeRootSignature(*worldSpaceFrustaPipeline.rootSignature);
		cl->setComputeDynamicConstantBuffer(WORLD_SPACE_TILED_FRUSTA_RS_CAMERA, cameraCBV);
		cl->setCompute32BitConstants(WORLD_SPACE_TILED_FRUSTA_RS_CB, frusta_cb{ lightCullingBuffers.numTilesX, lightCullingBuffers.numTilesY });
		cl->setRootComputeUAV(WORLD_SPACE_TILED_FRUSTA_RS_FRUSTA_UAV, lightCullingBuffers.tiledFrusta);
		cl->dispatch(bucketize(lightCullingBuffers.numTilesX, 16), bucketize(lightCullingBuffers.numTilesY, 16));

		// Light culling.
		cl->clearUAV(lightCullingBuffers.lightIndexCounter, 0.f);
		//cl->uavBarrier(lightCullingBuffers.lightIndexCounter); // Is this necessary?
		cl->setPipelineState(*lightCullingPipeline.pipeline);
		cl->setComputeRootSignature(*lightCullingPipeline.rootSignature);
		cl->setComputeDynamicConstantBuffer(LIGHT_CULLING_RS_CAMERA, cameraCBV);
		cl->setCompute32BitConstants(LIGHT_CULLING_RS_CB, light_culling_cb{ lightCullingBuffers.numTilesX, numPointLights, numSpotLights });
		cl->setDescriptorHeapSRV(LIGHT_CULLING_RS_SRV_UAV, 0, depthBuffer);
		cl->setDescriptorHeapSRV(LIGHT_CULLING_RS_SRV_UAV, 1, pointLightBuffer[dxContext.bufferedFrameID]);
		cl->setDescriptorHeapSRV(LIGHT_CULLING_RS_SRV_UAV, 2, spotLightBuffer[dxContext.bufferedFrameID]);
		cl->setDescriptorHeapSRV(LIGHT_CULLING_RS_SRV_UAV, 3, lightCullingBuffers.tiledFrusta);
		cl->setDescriptorHeapUAV(LIGHT_CULLING_RS_SRV_UAV, 4, lightCullingBuffers.lightIndexCounter);
		cl->setDescriptorHeapUAV(LIGHT_CULLING_RS_SRV_UAV, 5, lightCullingBuffers.pointLightIndexList);
		cl->setDescriptorHeapUAV(LIGHT_CULLING_RS_SRV_UAV, 6, lightCullingBuffers.spotLightIndexList);
		cl->setDescriptorHeapUAV(LIGHT_CULLING_RS_SRV_UAV, 7, lightCullingBuffers.lightGrid);
		cl->dispatch(lightCullingBuffers.numTilesX, lightCullingBuffers.numTilesY);
	}



	// ----------------------------------------
	// SHADOW MAP PASS
	// ----------------------------------------

	cl->setPipelineState(*modelShadowPipeline.pipeline);
	cl->setGraphicsRootSignature(*modelShadowPipeline.rootSignature);

	for (uint32 i = 0; i < sun.numShadowCascades; ++i)
	{
		cl->setRenderTarget(sunShadowRenderTarget[i]);
		cl->setViewport(sunShadowRenderTarget[i].viewport);
		cl->clearDepth(sunShadowRenderTarget[i].dsvHandle);

		for (uint32 cascade = 0; cascade <= i; ++cascade)
		{
			for (const sun_shadow_render_pass::draw_call& dc : sunShadowRenderPass.drawCalls[cascade])
			{
				const mat4& m = dc.transform;
				const submesh_info& submesh = dc.submesh;
				const dx_mesh* mesh = dc.mesh;

				cl->setGraphics32BitConstants(MODEL_RS_MVP, transform_cb{ sun.vp[i] * m, m });

				cl->setVertexBuffer(0, mesh->vertexBuffer);
				cl->setIndexBuffer(mesh->indexBuffer);
				cl->drawIndexed(submesh.numTriangles * 3, 1, submesh.firstTriangle * 3, submesh.baseVertex, 0);
			}
		}
	}

	barrier_batcher(cl)
		.transition(sunShadowCascadeTextures, MAX_NUM_SUN_SHADOW_CASCADES, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);


	// ----------------------------------------
	// LIGHT PASS
	// ----------------------------------------

	cl->setRenderTarget(hdrRenderTarget);
	cl->setViewport(hdrRenderTarget.viewport);

	// Models.
	cl->setPipelineState(*modelPipeline.pipeline);
	cl->setGraphicsRootSignature(*modelPipeline.rootSignature);
	cl->setDescriptorHeapSRV(MODEL_RS_ENVIRONMENT_TEXTURES, 0, environment.irradiance);
	cl->setDescriptorHeapSRV(MODEL_RS_ENVIRONMENT_TEXTURES, 1, environment.environment);
	cl->setDescriptorHeapSRV(MODEL_RS_BRDF, 0, brdfTex);
	cl->setDescriptorHeapSRV(MODEL_RS_LIGHTS, 0, lightCullingBuffers.lightGrid);
	cl->setDescriptorHeapSRV(MODEL_RS_LIGHTS, 1, lightCullingBuffers.pointLightIndexList);
	cl->setDescriptorHeapSRV(MODEL_RS_LIGHTS, 2, lightCullingBuffers.spotLightIndexList);
	cl->setDescriptorHeapSRV(MODEL_RS_LIGHTS, 3, pointLightBuffer[dxContext.bufferedFrameID]);
	cl->setDescriptorHeapSRV(MODEL_RS_LIGHTS, 4, spotLightBuffer[dxContext.bufferedFrameID]);
	for (uint32 i = 0; i < MAX_NUM_SUN_SHADOW_CASCADES; ++i)
	{
		cl->setDescriptorHeapSRV(MODEL_RS_LIGHTS, 5 + i, sunShadowCascadeTextures[i]);
	}
	cl->setGraphicsDynamicConstantBuffer(MODEL_RS_SUN, sunCBV);

	cl->setGraphicsDynamicConstantBuffer(MODEL_RS_CAMERA, cameraCBV);

	for (const geometry_render_pass::draw_call& dc : geometryRenderPass.drawCalls)
	{
		const mat4& m = dc.transform;
		const submesh_info& submesh = dc.submesh;
		const dx_mesh* mesh = dc.mesh;
		const pbr_material* material = dc.material;

		uint32 flags = 0;

		if (material->albedo) 
		{
			cl->setDescriptorHeapSRV(MODEL_RS_PBR_TEXTURES, 0, *material->albedo); 
			flags |= USE_ALBEDO_TEXTURE;
		}
		if (material->normal)
		{
			cl->setDescriptorHeapSRV(MODEL_RS_PBR_TEXTURES, 1, *material->normal);
			flags |= USE_NORMAL_TEXTURE;
		}
		if (material->roughness)
		{
			cl->setDescriptorHeapSRV(MODEL_RS_PBR_TEXTURES, 2, *material->roughness);
			flags |= USE_ROUGHNESS_TEXTURE;
		}
		if (material->metallic)
		{
			cl->setDescriptorHeapSRV(MODEL_RS_PBR_TEXTURES, 3, *material->metallic);
			flags |= USE_METALLIC_TEXTURE;
		}

		cl->setGraphics32BitConstants(MODEL_RS_MATERIAL, pbr_material_cb{ material->albedoTint, material->roughnessOverride, material->metallicOverride, flags });

		cl->setGraphics32BitConstants(MODEL_RS_MVP, transform_cb{ camera.viewProj * m, m });

		cl->setVertexBuffer(0, mesh->vertexBuffer);
		cl->setIndexBuffer(mesh->indexBuffer);
		cl->drawIndexed(submesh.numTriangles * 3, 1, submesh.firstTriangle * 3, submesh.baseVertex, 0);
	}


	//cl->setStencilReference(1);
	//cl->setStencilReference(0);


	// ----------------------------------------
	// HELPER STUFF
	// ----------------------------------------


#if 0
	// Gizmos.
	if (gizmoType != gizmo_type_none)
	{
		cl->setVertexBuffer(0, gizmoMesh.vertexBuffer);
		cl->setIndexBuffer(gizmoMesh.indexBuffer);

		for (uint32 i = 0; i < 3; ++i)
		{
			mat4 m = createModelMatrix(meshTransform.position, gizmoRotations[i]);
			cl->setGraphics32BitConstants(MODEL_RS_MVP, transform_cb{ camera.viewProj * m, m });
			cl->setGraphics32BitConstants(MODEL_RS_MATERIAL, pbr_material_cb{ gizmoColors[i], 1.f, 0.f, 0 });
			cl->drawIndexed(gizmoSubmeshes[gizmoType].numTriangles * 3, 1, gizmoSubmeshes[gizmoType].firstTriangle * 3, gizmoSubmeshes[gizmoType].baseVertex, 0);
		}
	}
#endif


	if (showLightVolumes)
	{
		// Light volumes.
		cl->setPipelineState(*flatUnlitPipeline.pipeline);
		cl->setGraphicsRootSignature(*flatUnlitPipeline.rootSignature);
		cl->setVertexBuffer(0, positionOnlyMesh.vertexBuffer);
		cl->setIndexBuffer(positionOnlyMesh.indexBuffer);

		for (uint32 i = 0; i < numPointLights; ++i)
		{
			float radius = pointLights[i].radius;
			vec3 position = pointLights[i].position;
			cl->setGraphics32BitConstants(0, camera.viewProj * createModelMatrix(position, quat::identity, vec3(radius, radius, radius)));
			cl->drawIndexed(sphereMesh.numTriangles * 3, 1, sphereMesh.firstTriangle * 3, sphereMesh.baseVertex, 0);
		}
	}



	// ----------------------------------------
	// PRESENT
	// ----------------------------------------

	barrier_batcher(cl)
		.transition(hdrColorTexture.resource, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

	cl->setRenderTarget(windowRenderTarget);
	cl->setViewport(windowViewport);
	if (aspectRatioModeChanged)
	{
		cl->clearRTV(windowRenderTarget.rtvHandles[0], 0.f, 0.f, 0.f);
	}

	cl->setPipelineState(*presentPipeline.pipeline);
	cl->setGraphicsRootSignature(*presentPipeline.rootSignature);

	cl->setGraphics32BitConstants(PRESENT_RS_TONEMAP, tonemap);
	cl->setGraphics32BitConstants(PRESENT_RS_PRESENT, present_cb{ 0, 0.f });
	cl->setDescriptorHeapSRV(PRESENT_RS_TEX, 0, hdrColorTexture);
	cl->drawFullscreenTriangle();


	barrier_batcher(cl)
		.transition(hdrColorTexture.resource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON)
		.transition(frameResult.resource, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);

	//dxContext.executeCommandList(cl);
}

void dx_renderer::blitResultToScreen(dx_command_list* cl, dx_cpu_descriptor_handle rtv)
{
	CD3DX12_VIEWPORT viewport = CD3DX12_VIEWPORT(0.f, 0.f, (float)windowWidth, (float)windowHeight);
	cl->setViewport(viewport);

	cl->setRenderTarget(&rtv.cpuHandle, 1, 0);

	cl->setPipelineState(*blitPipeline.pipeline);
	cl->setGraphicsRootSignature(*blitPipeline.rootSignature);
	cl->setDescriptorHeapSRV(0, 0, frameResult);
	cl->drawFullscreenTriangle();
}
