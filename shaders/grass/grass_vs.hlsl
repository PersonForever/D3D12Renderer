#include "grass_rs.hlsli"
#include "camera.hlsli"
#include "grass_vertex.hlsli"

ConstantBuffer<grass_cb> cb				: register(b0);
ConstantBuffer<camera_cb> camera		: register(b1);
StructuredBuffer<grass_blade> blades	: register(t0);

struct vs_output
{
	float2 uv				: TEXCOORDS;
	float3 normal			: NORMAL;
	float3 tangent			: TANGENT;
	float3 worldPosition	: POSITION;

	float4 position : SV_POSITION;
};

vs_output main(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
{
	grass_bend_settings bendSettings;
	bendSettings.relTipOffsetZ = 0.7f;
	bendSettings.controlPointZ = bendSettings.relTipOffsetZ * 0.5f;
	bendSettings.controlPointY = 0.8f;



	grass_blade blade = blades[instanceID];

#if DISABLE_ALL_GRASS_DYNAMICS
	blade.facing = float2(0.f, 1.f);
#endif


	float2 uv = grassUV(blade, vertexID, cb.numVertices);
	float2 wind = grassWind(blade, cb.windDirection, cb.time);
	float3 position = grassPosition(blade, uv, cb.height, cb.halfWidth, bendSettings, wind);
	float3 normal = grassNormal(blade, uv, bendSettings, wind);



	vs_output OUT;
	OUT.uv = uv;
	OUT.normal = normal;
	OUT.tangent = float3(blade.facing.y, 0.f, blade.facing.x);
	OUT.worldPosition = position;
	OUT.position = mul(camera.viewProj, float4(position, 1.f));
	return OUT;
}
