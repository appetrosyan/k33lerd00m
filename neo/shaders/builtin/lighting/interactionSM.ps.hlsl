/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2013-2024 Robert Beckebans
Copyright (C) 2020 Panos Karabelas

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 BFG Edition Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 BFG Edition Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 BFG Edition Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 BFG Edition Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

#include "global_inc.hlsl"
#include "renderParmSet11.inc.hlsl"
#include "BRDF.inc.hlsl"


// *INDENT-OFF*
Texture2D				t_Normal			: register( t0 VK_DESCRIPTOR_SET( 1 ) );
Texture2D				t_Specular			: register( t1 VK_DESCRIPTOR_SET( 1 ) );
Texture2D				t_BaseColor			: register( t2 VK_DESCRIPTOR_SET( 1 ) );

Texture2D				t_LightFalloff		: register( t3 VK_DESCRIPTOR_SET( 2 ) );
Texture2D				t_LightProjection	: register( t4 VK_DESCRIPTOR_SET( 2 ) );
#if USE_SHADOW_ATLAS
Texture2D				t_ShadowAtlas		: register( t5 VK_DESCRIPTOR_SET( 2 ) );
#else
Texture2DArray<float>	t_ShadowMapArray	: register( t5 VK_DESCRIPTOR_SET( 2 ) );
#endif
Texture2D				t_Jitter			: register( t6 VK_DESCRIPTOR_SET( 2 ) );

SamplerState			s_Material : register( s0 VK_DESCRIPTOR_SET( 3 ) ); // for the normal/specular/basecolor
SamplerState 			s_Lighting : register( s1 VK_DESCRIPTOR_SET( 3 ) ); // for sampling the jitter
SamplerComparisonState  s_Shadow   : register( s2 VK_DESCRIPTOR_SET( 3 ) ); // for the depth shadow map sampler with a compare function
SamplerState 			s_Jitter   : register( s3 VK_DESCRIPTOR_SET( 3 ) ); // for sampling the jitter

#if USE_SOFT_WEDGE
// Analytic soft shadows (per-fragment path): this light's silhouette edges, evaluated against the
// EXACT receiver world position. StructuredBuffer at t12 in the uniforms set (mirrors the joint
// buffer at t11). rpJitterTexScale carries (R, minWidth, numEdges) for the soft path.
#include "softwedge_coverage.inc.hlsl"
// The buffer is the vertex-cache joint buffer, whose struct stride is sizeof(float4)=16. So read it as
// float4 elements: each edge is TWO consecutive float4 (e0 = xyz world endpoint 0 + w silWeight; e1 =
// xyz endpoint 1). Edge se lives at elements [se*2], [se*2+1].
StructuredBuffer<float4> t_SoftEdges : register( t12 VK_DESCRIPTOR_SET( 0 ) );
#endif

struct PS_IN
{
	float4 position		: SV_Position;
	float4 texcoord0	: TEXCOORD0_centroid;
	float4 texcoord1	: TEXCOORD1_centroid;
	float4 texcoord2	: TEXCOORD2_centroid;
	float4 texcoord3	: TEXCOORD3_centroid;
	float4 texcoord4	: TEXCOORD4_centroid;
	float4 texcoord5	: TEXCOORD5_centroid;
	float4 texcoord6	: TEXCOORD6_centroid;
	float4 texcoord7	: TEXCOORD7_centroid;
	float4 texcoord8	: TEXCOORD8_centroid;
	float4 texcoord9	: TEXCOORD9_centroid;
	float4 color		: COLOR0;
};

struct PS_OUT
{
	float4 color : SV_Target0;
};
// *INDENT-ON*

float BlueNoise( float2 n, float x )
{
	float2 uv = n.xy * pc.rpJitterTexOffset.xy;

	float noise = t_Jitter.Sample( s_Jitter, uv ).r;

	noise = frac( noise + c_goldenRatioConjugate * pc.rpJitterTexOffset.w * x );

	//noise = RemapNoiseTriErp( noise );
	//noise = noise * 2.0 - 0.5;

	return noise;
}

float2 VogelDiskSample( float sampleIndex, float samplesCount, float phi )
{
	float goldenAngle = 2.4f;

	float r = sqrt( sampleIndex + 0.5f ) / sqrt( samplesCount );
	float theta = sampleIndex * goldenAngle + phi;

	float sine = sin( theta );
	float cosine = cos( theta );

	return float2( r * cosine, r * sine );
}

void main( PS_IN fragment, out PS_OUT result )
{
	float2 baseUV = fragment.texcoord4.xy;
	float2 bumpUV = fragment.texcoord1.xy;
	float2 specUV = fragment.texcoord5.xy;

	// PSX affine texture mapping
	if( pc.rpPSXDistortions.z > 0.0 )
	{
		baseUV /= fragment.texcoord1.z;
		bumpUV /= fragment.texcoord1.z;
		specUV /= fragment.texcoord1.z;
	}

	float4 bumpMap =		t_Normal.Sample( s_Material, bumpUV );
	float4 lightFalloff =	idtex2Dproj( s_Lighting, t_LightFalloff, fragment.texcoord2 );
	float4 lightProj =		idtex2Dproj( s_Lighting, t_LightProjection, fragment.texcoord3 );
	float4 YCoCG =			t_BaseColor.Sample( s_Material, baseUV );
	float4 specMapSRGB =	t_Specular.Sample( s_Material, specUV );
	float4 specMap =		sRGBAToLinearRGBA( specMapSRGB );

	float3 lightVector = normalize( fragment.texcoord0.xyz );
	float3 viewVector = normalize( fragment.texcoord6.xyz );
	float3 diffuseMap = sRGBToLinearRGB( ConvertYCoCgToRGB( YCoCG ) );

	float3 localNormal;
	// RB begin
#if USE_NORMAL_FMT_RGB8
	localNormal.xy = bumpMap.rg - 0.5;
#else
	localNormal.xy = bumpMap.wy - 0.5;
#endif
	// RB end
	localNormal.z = sqrt( abs( dot( localNormal.xy, localNormal.xy ) - 0.25 ) );
	localNormal = normalize( localNormal );

	// Geometric specular antialiasing: sub-pixel normal variance -> extra GGX
	// roughness, so shiny detailed surfaces (wet blood) don't alias into specular
	// point noise. Source-level fix, no temporal accumulation. See interaction.ps.hlsl.
	float3 dNdx = ddx( localNormal );
	float3 dNdy = ddy( localNormal );
	float specAAvariance = 0.25 * ( dot( dNdx, dNdx ) + dot( dNdy, dNdy ) );
	float specAAkernelRoughness2 = min( 2.0 * specAAvariance, 0.25 );

	// traditional very dark Lambert light model used in Doom 3
	float ldotN = saturate( dot3( localNormal, lightVector ) );

#if defined(USE_HALF_LAMBERT)
	// RB: http://developer.valvesoftware.com/wiki/Half_Lambert
	float halfLdotN = dot3( localNormal, lightVector ) * 0.5 + 0.5;
	halfLdotN *= halfLdotN;

	// tweak to not loose so many details
	float lambert = lerp( ldotN, halfLdotN, 0.5 );
#else
	float lambert = ldotN;
#endif


	//
	// shadow mapping
	//
#if USE_SOFT_WEDGE
	// Analytic soft shadows: sum this light's silhouette-edge coverage against the EXACT receiver world
	// position (model position texcoord7 -> world by the receiver's model matrix). No screen-space depth
	// reconstruction, so no grazing instability; the umbra emerges where the summed occlusion saturates.
	float4 swMP = float4( fragment.texcoord7.xyz, 1.0 );
	float3 swP;
	swP.x = dot4( pc.rpModelMatrixX, swMP );
	swP.y = dot4( pc.rpModelMatrixY, swMP );
	swP.z = dot4( pc.rpModelMatrixZ, swMP );

	float3 swL    = pc.rpGlobalLightOrigin.xyz;
	float  swR    = max( pc.rpJitterTexScale.x, 1e-2 );	// light disk radius (penumbra); floored so pi*r^2 != 0
	int    swN    = int( pc.rpJitterTexScale.z );

	// nvrhi doesn't apply the structured-buffer range byteOffset to the shader index, so index from an
	// explicit first element (this light's edges start here), passed in rpJitterTexOffset.x.
	int swFirstElem = int( pc.rpJitterTexOffset.x );

	// Light-disk coverage combine. Build the area light's disk (centre swL, radius swR) facing the receiver
	// and a 2D basis (swU,swV). For each caster (edges tagged with a group id in e1.w) accumulate the signed
	// area of disk INTERSECT its projected silhouette; the occluded fraction is |area| / (pi r^2). This is
	// winding-correct -> exact for NON-CONVEX casters (unlike a per-edge MIN). Casters union by max. A caster
	// with any vertex not toward the light (receiver in front of it) can't cleanly shadow P -> contributes 0.
	float3 swToL    = swL - swP;
	float  swDistPL = max( length( swToL ), 1e-4 );		// receiver->light distance; casters beyond it can't occlude
	float3 swNrm = swToL / swDistPL;
	float3 swUp  = ( abs( swNrm.z ) > 0.9 ) ? float3( 0.0, 1.0, 0.0 ) : float3( 0.0, 0.0, 1.0 );
	float3 swU   = normalize( cross( swUp, swNrm ) );
	float3 swV   = cross( swNrm, swU );
	float  swDiskArea = PI * swR * swR;
	float  swInvDiskArea = 1.0 / swDiskArea;					// hoisted: per-caster finalize multiplies instead of divides
	float  swSinA     = saturate( swR / swDistPL );				// sin of the light-disk half-angle from P (caster-invariant)
	float  swCosA     = sqrt( 1.0 - swSinA * swSinA );			// cos of it; used by the cheap per-caster angular cull

	float swOcc = 0.0;				// max occlusion across casters
	float swArea = 0.0;				// signed disk-intersection area for the current caster
	bool  haveCaster = false;		// opened a caster (seen its header record) yet?
	bool  swSkip = false;			// current caster culled: its bounding sphere can't reach the light disk
	const float swEps = 1e-3;
	for( int se = 0; se < swN; se++ )
	{
		float4 e0 = t_SoftEdges[swFirstElem + se * 2 + 0];
		float4 e1 = t_SoftEdges[swFirstElem + se * 2 + 1];
		// Per-caster bounding-sphere cull. The header record (e0.w < 0, one per caster from the frontend
		// flatten) carries the caster's world bounding sphere: centre e0.xyz, radius e1.x. Only the DEPTH
		// reject is applied: skip the caster when its sphere lies entirely behind the receiver or entirely
		// beyond the light plane. Provably lossless vs the full loop - a sphere wholly behind the receiver
		// has every vertex dn < swEps, one wholly beyond the light has every vertex dn > swDistPL, so the
		// per-edge slab clip below drops every one of that caster's edges anyway (identical zero contribution).
		// The ANGULAR "sphere cone misses the disk" reject is GATED (AO-4): it is only conservative when the
		// sphere is fully in FRONT of the receiver near-plane (dCn - cRad > swEps). There every silhouette
		// vertex has dn > swEps, so its disk-plane projection radius is bounded (no 1/dn blowup) and the
		// sphere's angular cone genuinely bounds the silhouette's projection - a cone entirely off the disk
		// gives a loop that winds zero disk area (0 coverage). When the sphere STRADDLES the near-plane a
		// vertex at dn->0 projects to radius proportional to 1/dn, so an angularly-off caster can still sweep
		// a winding sector across the disk; angular-culling it there drops real penumbra (the corruption AO-4
		// fixed). So we apply the angular reject only in the safe (fully-in-front) regime.
		if( e0.w < 0.0 )		// header record = caster boundary: finalize the previous caster here (no per-edge id compare)
		{
			if( haveCaster )
			{
				swOcc = max( swOcc, saturate( abs( swArea ) * swInvDiskArea ) );
				if( swOcc >= 0.999 ) { break; }		// fully occluded: no remaining caster can raise the max past 1
			}
			haveCaster = true;
			swArea = 0.0;
			swSkip = false;
			float3 dCv  = e0.xyz - swP;
			float  cRad = e1.x;
			float  dCn  = dot( dCv, swNrm );						// sphere-centre depth along receiver->light
			if( dCn + cRad < swEps || dCn - cRad > swDistPL )		// wholly behind receiver, or wholly beyond light
			{
				swSkip = true;
				continue;
			}
			if( dCn - cRad > 1e-3 )									// sphere fully in front of the near-plane: angular reject is safe
			{
				// Cull when the caster-sphere cone and the light-disk cone are angularly disjoint (ang >
				// alpha+beta), computed WITHOUT transcendentals: cos(ang) < cos(alpha+beta), both sides x dClen
				// -> dCn < cosA*sqrt(dClen^2 - cRad^2) - sinA*cRad. sinA/cosA (the disk half-angle) are
				// caster-invariant and hoisted (swSinA/swCosA). One sqrt, no asin/acos, so a MISS is ~free -
				// the test no longer taxes the casters it fails to cull. Gate above keeps it conservative.
				float front = max( dot( dCv, dCv ) - cRad * cRad, 0.0 );
				swSkip = ( dCn < swCosA * sqrt( front ) - swSinA * cRad );
			}
			continue;
		}
		if( swSkip ) { continue; }
		// Near-plane clip. A silhouette vertex behind the receiver (dn<=0, receiver in front of it) can't be
		// projected onto the light disk. Do NOT skip the edge - that opens the loop and the shoelace closes
		// the gap with a chord that spuriously encloses the disk (over-occlusion). Instead CLIP the edge to
		// the near plane: the clipped endpoint lands on the disk horizon, which CircleTriArea treats as a
		// boundary sector, keeping the loop closed. An edge entirely behind is dropped (that arc is beyond P).
		// Clip the edge to the slab BETWEEN the receiver and the light plane (swEps < dn < swDistPL). An
		// edge fully behind the receiver, or fully BEYOND the light, can't occlude - and a beyond-light
		// caster's silhouette spuriously encloses the disk (uniform over-occlusion). Clipping keeps the
		// loop closed at both planes.
		// Receiver-relative coords a=A-swP, b=B-swP: computed ONCE and reused for both the slab depth (dot with
		// swNrm) and the disk projection (dot with swU/swV), instead of re-subtracting swP inside the projection.
		// Clipping is affine so it is identical in this space; projection is (swDistPL/dn)*(dot(a,swU),dot(a,swV))
		// because dot(L-P,swNrm) == swDistPL and the disk basis (swU,swV) is perpendicular to swNrm.
		float3 a = e0.xyz - swP;
		float3 b = e1.xyz - swP;
		float  dnA = dot( a, swNrm );
		float  dnB = dot( b, swNrm );
		if( ( dnA < swEps && dnB < swEps ) || ( dnA > swDistPL && dnB > swDistPL ) ) { continue; }
		if( dnA < swEps )     { a = a + ( ( swEps - dnA ) / ( dnB - dnA ) ) * ( b - a ); dnA = swEps; }
		if( dnB < swEps )     { b = b + ( ( swEps - dnB ) / ( dnA - dnB ) ) * ( a - b ); dnB = swEps; }
		if( dnA > swDistPL )  { a = a + ( ( swDistPL - dnA ) / ( dnB - dnA ) ) * ( b - a ); dnA = swDistPL; }
		if( dnB > swDistPL )  { b = b + ( ( swDistPL - dnB ) / ( dnA - dnB ) ) * ( a - b ); dnB = swDistPL; }
		float2 qa = ( swDistPL / dnA ) * float2( dot( a, swU ), dot( a, swV ) );
		float2 qb = ( swDistPL / dnB ) * float2( dot( b, swU ), dot( b, swV ) );
		swArea += SoftDisk_CircleTriArea( qa, qb, swR );
	}
	if( haveCaster ) { swOcc = max( swOcc, saturate( abs( swArea ) * swInvDiskArea ) ); }	// last caster
	float shadow = 1.0 - saturate( swOcc );
	int swDbg = int( pc.rpJitterTexScale.w );	// diagnostic selector (r_softShadowDebugShader), visualised at end of main
#elif USE_RT_SHADOW
	// Ray-traced visibility: RtShadowsPass wrote a screen-space mask for this light.
	// SV_Position matches the mask 1:1 (both at render resolution), so a direct Load
	// replaces the entire light-space projection + PCF path below. t_ShadowAtlas is
	// the mask texture (bound at the shadow-map texunit; RT variants compile with
	// USE_SHADOW_ATLAS so the Texture2D binding exists).
	float shadow = t_ShadowAtlas.Load( int3( int2( fragment.position.xy ), 0 ) ).r;
#else
	int shadowIndex = 0;

#if LIGHT_POINT
	float3 toLightGlobal = normalize( fragment.texcoord8.xyz );

	float axis[6];
	axis[0] = -toLightGlobal.x;
	axis[1] =  toLightGlobal.x;
	axis[2] = -toLightGlobal.y;
	axis[3] =  toLightGlobal.y;
	axis[4] = -toLightGlobal.z;
	axis[5] =  toLightGlobal.z;

	for( int i = 0; i < 6; i++ )
	{
		if( axis[i] > axis[shadowIndex] )
		{
			shadowIndex = i;
		}
	}

#endif // #if defined( LIGHT_POINT )

#if LIGHT_PARALLEL

	float viewZ = -fragment.texcoord9.z;

	shadowIndex = 4;
	for( int ci = 0; ci < 4; ci++ )
	{
		if( viewZ < pc.rpCascadeDistances[ci] )
		{
			shadowIndex = ci;
			break;
		}
	}
#endif

#if 0
	if( shadowIndex == 0 )
	{
		result.color = float4( 1.0, 0.0, 0.0, 1.0 );
	}
	else if( shadowIndex == 1 )
	{
		result.color = float4( 0.0, 1.0, 0.0, 1.0 );
	}
	else if( shadowIndex == 2 )
	{
		result.color = float4( 0.0, 0.0, 1.0, 1.0 );
	}
	else if( shadowIndex == 3 )
	{
		result.color = float4( 1.0, 1.0, 0.0, 1.0 );
	}
	else if( shadowIndex == 4 )
	{
		result.color = float4( 1.0, 0.0, 1.0, 1.0 );
	}
	else if( shadowIndex == 5 )
	{
		result.color = float4( 0.0, 1.0, 1.0, 1.0 );
	}

	//result.color.xyz *= lightColor;
	return;
#endif

	// pc.rpShadowMatrices contain model -> world -> shadow transformation for evaluation
	float4 shadowMatrixX = pc.rpShadowMatrices[ int ( shadowIndex * 4 + 0 ) ];
	float4 shadowMatrixY = pc.rpShadowMatrices[ int ( shadowIndex * 4 + 1 ) ];
	float4 shadowMatrixZ = pc.rpShadowMatrices[ int ( shadowIndex * 4 + 2 ) ];
	float4 shadowMatrixW = pc.rpShadowMatrices[ int ( shadowIndex * 4 + 3 ) ];

	float4 modelPosition = float4( fragment.texcoord7.xyz, 1.0 );

	float4 shadowTexcoord;
	shadowTexcoord.x = dot4( modelPosition, shadowMatrixX );
	shadowTexcoord.y = dot4( modelPosition, shadowMatrixY );
	shadowTexcoord.z = dot4( modelPosition, shadowMatrixZ );
	shadowTexcoord.w = dot4( modelPosition, shadowMatrixW );

	//float bias = 0.005 * tan( acos( ldotN ) );
	//bias = clamp( bias, 0, 0.01 );
	float bias = 0.001;

	shadowTexcoord.xyz /= shadowTexcoord.w;

	// receiver / occluder terminology like in ESM
	float receiver = shadowTexcoord.z * pc.rpScreenCorrectionFactor.w;
	//shadowTexcoord.z = shadowTexcoord.z * 0.999991;
	//shadowTexcoord.z = shadowTexcoord.z - bias;
	shadowTexcoord.w = float( shadowIndex );

	// multiple taps

#if 0
	float4 base = shadowTexcoord;

	base.xy += pc.rpJitterTexScale.xy * -0.5;

	float shadow = 0.0;

	//float stepSize = 1.0 / 16.0;
	float numSamples = 16;
	float stepSize = 1.0 / numSamples;

	float2 jitterTC = ( fragment.position.xy * pc.rpScreenCorrectionFactor.xy ) + pc.rpJitterTexOffset.ww;
	for( float n = 0.0; n < numSamples; n += 1.0 )
	{
		float4 jitter = base + t_Jitter.Sample( samp1, jitterTC.xy ) * pc.rpJitterTexScale;
		jitter.zw = shadowTexcoord.zw;

		shadow += t_Jitter.Sample( samp1, jitter.xy / jitter.z ).r;
		jitterTC.x += stepSize;
	}

	shadow *= stepSize;


#elif 0

	// Poisson Disk with White Noise used for years int RBDOOM-3-BFG

	const float2 poissonDisk[12] =
	{
		float2( 0.6111618, 0.1050905 ),
		float2( 0.1088336, 0.1127091 ),
		float2( 0.3030421, -0.6292974 ),
		float2( 0.4090526, 0.6716492 ),
		float2( -0.1608387, -0.3867823 ),
		float2( 0.7685862, -0.6118501 ),
		float2( -0.1935026, -0.856501 ),
		float2( -0.4028573, 0.07754025 ),
		float2( -0.6411021, -0.4748057 ),
		float2( -0.1314865, 0.8404058 ),
		float2( -0.7005203, 0.4596822 ),
		float2( -0.9713828, -0.06329931 )
	};

	float shadow = 0.0;

	// RB: casting a float to int and using it as index can really kill the performance ...
	float numSamples = 12.0; //int(pc.rpScreenCorrectionFactor.w);
	float stepSize = 1.0 / numSamples;

	float4 jitterTC = ( fragment.position * pc.rpScreenCorrectionFactor ) + pc.rpJitterTexOffset;
	float4 random = t_Jitter.Sample( s_Jitter, jitterTC.xy ) * PI;
	//float4 random = fragment.position;

	float2 rot;
	rot.x = cos( random.x );
	rot.y = sin( random.x );

	float shadowTexelSize = pc.rpScreenCorrectionFactor.z * pc.rpJitterTexScale.x;
	for( int i = 0; i < 12; i++ )
	{
		float2 jitter = poissonDisk[i];
		float2 jitterRotated;
		jitterRotated.x = jitter.x * rot.x - jitter.y * rot.y;
		jitterRotated.y = jitter.x * rot.y + jitter.y * rot.x;

		float4 shadowTexcoordJittered = float4( shadowTexcoord.xy + jitterRotated * shadowTexelSize, shadowTexcoord.z, shadowTexcoord.w );

		shadow += idtex2Dproj( samp1, t_ShadowMapArray, shadowTexcoordJittered.xywz ).r;
	}

	shadow *= stepSize;

#elif 1

#if 0

	// Poisson Disk with animated Blue Noise or Interleaved Gradient Noise

	const float2 poissonDisk[12] =
	{
		float2( 0.6111618, 0.1050905 ),
		float2( 0.1088336, 0.1127091 ),
		float2( 0.3030421, -0.6292974 ),
		float2( 0.4090526, 0.6716492 ),
		float2( -0.1608387, -0.3867823 ),
		float2( 0.7685862, -0.6118501 ),
		float2( -0.1935026, -0.856501 ),
		float2( -0.4028573, 0.07754025 ),
		float2( -0.6411021, -0.4748057 ),
		float2( -0.1314865, 0.8404058 ),
		float2( -0.7005203, 0.4596822 ),
		float2( -0.9713828, -0.06329931 )
	};

	float shadow = 0.0;

	// RB: casting a float to int and using it as index can really kill the performance ...
	float numSamples = 12.0;
	float stepSize = 1.0 / numSamples;

	float random = BlueNoise( fragment.position.xy, 1.0 );
	//float random = InterleavedGradientNoiseAnim( fragment.position.xy, pc.rpJitterTexOffset.w );

	random *= PI;

	float2 rot;
	rot.x = cos( random );
	rot.y = sin( random );

	float shadowTexelSize = pc.rpScreenCorrectionFactor.z * pc.rpJitterTexScale.x;
	for( int si = 0; si < 12; si++ )
	{
		float2 jitter = poissonDisk[si];
		float2 jitterRotated;
		jitterRotated.x = jitter.x * rot.x - jitter.y * rot.y;
		jitterRotated.y = jitter.x * rot.y + jitter.y * rot.x;

		// [0 .. 1] -> rectangle in atlas transform
		float2 shadowTexcoordAtlas = shadowTexcoord.xy * pc.rpJitterTexScale.y + pc.rpShadowAtlasOffsets[ shadowIndex ].xy;

		float2 shadowTexcoordJittered = shadowTexcoordAtlas.xy + jitterRotated * shadowTexelSize;

		shadow += t_ShadowAtlas.SampleCmpLevelZero( s_Shadow, shadowTexcoordJittered.xy, receiver );
	}

	shadow *= stepSize;


#else

	// Vogel Disk Sampling
	// https://twitter.com/panoskarabelas1/status/1222663889659355140

	// this approach is more dynamic and can be controlled by r_shadowMapSamples

	float shadow = 0.0;

	float numSamples = pc.rpJitterTexScale.w;
	float stepSize = 1.0 / numSamples;

	float vogelPhi = BlueNoise( fragment.position.xy, 1.0 );
	//float vogelPhi = InterleavedGradientNoiseAnim( fragment.position.xy, pc.rpJitterTexOffset.w );

	float shadowTexelSize = pc.rpScreenCorrectionFactor.z * pc.rpJitterTexScale.x;
	for( float si = 0.0; si < numSamples; si += 1.0 )
	{
		float2 jitter = VogelDiskSample( si, numSamples, vogelPhi );

#if USE_SHADOW_ATLAS
		// [0 .. 1] -> rectangle in atlas transform
		float2 shadowTexcoordAtlas = shadowTexcoord.xy * pc.rpJitterTexScale.y + pc.rpShadowAtlasOffsets[ shadowIndex ].xy;

		float2 shadowTexcoordJittered = shadowTexcoordAtlas + jitter * shadowTexelSize;

		shadow += t_ShadowAtlas.SampleCmpLevelZero( s_Shadow, shadowTexcoordJittered.xy, receiver );
#else
		float3 shadowTexcoordJittered = float3( shadowTexcoord.xy + jitter * shadowTexelSize, shadowTexcoord.w );

		shadow += t_ShadowMapArray.SampleCmpLevelZero( s_Shadow, shadowTexcoordJittered, receiver );
#endif
	}

	shadow *= stepSize;
#endif

#else

#if USE_SHADOW_ATLAS
	float2 uvShadow;
	uvShadow.x = shadowTexcoord.x;
	uvShadow.y = shadowTexcoord.y;

	// [0 .. 1] -> rectangle in atlas transform
	uvShadow = uvShadow * pc.rpJitterTexScale.y + pc.rpShadowAtlasOffsets[ shadowIndex ].xy;

	float shadow = t_ShadowAtlas.SampleCmpLevelZero( s_Shadow, uvShadow.xy, receiver );
#else
	float3 uvzShadow;
	uvzShadow.x = shadowTexcoord.x;
	uvzShadow.y = shadowTexcoord.y;
	uvzShadow.z = shadowTexcoord.w;
	float shadow = t_ShadowMapArray.SampleCmpLevelZero( samp2, uvzShadow, receiver );
#endif

#if 0
	if( shadowIndex == 0 )
	{
		result.color = float4( 1.0, 0.0, 0.0, 1.0 );
	}
	else if( shadowIndex == 1 )
	{
		result.color = float4( 0.0, 1.0, 0.0, 1.0 );
	}
	else if( shadowIndex == 2 )
	{
		result.color = float4( 0.0, 0.0, 1.0, 1.0 );
	}
	else if( shadowIndex == 3 )
	{
		result.color = float4( 1.0, 1.0, 0.0, 1.0 );
	}
	else if( shadowIndex == 4 )
	{
		result.color = float4( 1.0, 0.0, 1.0, 1.0 );
	}
	else if( shadowIndex == 5 )
	{
		result.color = float4( 0.0, 1.0, 1.0, 1.0 );
	}

	result.color.rgb *= shadow;
	return;
#endif

#endif

#endif // USE_RT_SHADOW

#if !USE_SOFT_WEDGE
	// allow shadows to fade out
	// NOTE: the soft-wedge path repurposes rpJitterTexScale.z as the edge count (swN, read at the
	// top of this file), so this fade floor MUST NOT run there - max( 1-swOcc, edgeCount ) saturates
	// to 1.0 and clobbers every soft fragment to fully-lit (no shadow anywhere while the coverage
	// loop still runs). The soft path's shadow = 1 - saturate(swOcc) flows straight to the combine.
	shadow = saturate( max( shadow, pc.rpJitterTexScale.z ) );
#endif

	float3 halfAngleVector = normalize( lightVector + viewVector );
	float hdotN = clamp( dot3( halfAngleVector, localNormal ), 0.0, 1.0 );

#if USE_PBR
	// RB: roughness 0 somehow is not shiny so we clamp it
	float roughness = max( 0.05, specMapSRGB.r );
	const float metallic = specMapSRGB.g;

	// the vast majority of real-world materials (anything not metal or gems) have F(0)
	// values in a very narrow range (~0.02 - 0.08)

	// approximate non-metals with linear RGB 0.04 which is 0.08 * 0.5 (default in UE4)
	const float3 dielectricColor = _float3( 0.04 );

	// derive diffuse and specular from albedo(m) base color
	const float3 baseColor = diffuseMap;

	float3 diffuseColor = baseColor * ( 1.0 - metallic );
	float3 specularColor = lerp( dielectricColor, baseColor, metallic );

#elif KENNY_PBR
	float3 diffuseColor = diffuseMap;
	float3 specularColor;
	float roughness;

	PBRFromSpecmap( specMapSRGB.rgb, specularColor, roughness );
#else
	float roughness = EstimateLegacyRoughness( specMapSRGB.rgb );

	float3 diffuseColor = diffuseMap;
	float3 specularColor = specMapSRGB.rgb; // RB: should be linear but it looks too flat
#endif

	// Geometric specular AA: widen roughness by the sub-pixel normal-variance kernel
	// computed near the top of the shader. This shadow-mapped interaction is the path
	// most in-game lights take, and it previously computed the kernel but never applied
	// it, so bright specular detail (wet blood, metal trim) crawled on this path. Applied
	// as the isotropic variance form to match interaction.ps.hlsl; no temporal component.
	roughness = sqrt( saturate( roughness * roughness + specAAkernelRoughness2 ) );


	// RB FIXME or not: compensate r_lightScale 3 and the division of Pi
	//lambert *= 1.3;
	// see http://seblagarde.wordpress.com/2012/01/08/pi-or-not-to-pi-in-game-lighting-equation/
	//lambert /= PI;

	// pc.rpDiffuseModifier contains light color multiplier
	float3 lightColor = sRGBToLinearRGB( lightProj.xyz * lightFalloff.xyz );

	float vdotN = clamp( dot3( viewVector, localNormal ), 0.0, 1.0 );
	float vdotH = clamp( dot3( viewVector, halfAngleVector ), 0.0, 1.0 );
	float ldotH = clamp( dot3( lightVector, halfAngleVector ), 0.0, 1.0 );

	// keep in mind this is r_lightScale 3 * 2
	float3 reflectColor = specularColor * pc.rpSpecularModifier.rgb;

	// cheap approximation by ARM with only one division
	// http://community.arm.com/servlet/JiveServlet/download/96891546-19496/siggraph2015-mmg-renaldas-slides.pdf
	// page 26

	float rr = roughness * roughness;
	float rrrr = rr * rr;

	// disney GGX
	float D = ( hdotN * hdotN ) * ( rrrr - 1.0 ) + 1.0;
	float VFapprox = ( ldotH * ldotH ) * ( roughness + 0.5 );

#if KENNY_PBR
	// specular cook-torrance brdf (visibility, geo and denom in one)
	//float D = Distribution_GGX_Disney( hdotN, rr );
	//float Vis = Visibility_Schlick( vdotN, lambert, rr );
	//float3 F = Fresnel_Schlick( reflectColor, vdotH );
	//float3 specularLight = D * Vis * F;

	float3 specularLight = ( rrrr / ( 4.0 * D * D * VFapprox ) ) * ldotN * reflectColor;
#else
	float3 specularLight = ( rrrr / ( 4.0 * PI * D * D * VFapprox ) ) * ldotN * reflectColor;
#endif


#if 0
	result.color = float4( _float3( VFapprox ), 1.0 );
	return;
#endif

	//float3 diffuseColor = mix( diffuseMap, F0, metal ) * pc.rpDiffuseModifier.xyz;
	float3 diffuseLight = diffuseColor * lambert * ( pc.rpDiffuseModifier.xyz );

	float3 color = ( diffuseLight + specularLight ) * lightColor * fragment.color.rgb * shadow;

	result.color.rgb = color;
	result.color.a = 1.0;

#if USE_SOFT_WEDGE
	// DIAGNOSTIC (swDbg = rpJitterTexScale.w, set by r_softShadowDebugShader):
	//   7 = solid red IF the debug PARAM arrived (red -> params reach the shader; normal scene -> they don't)
	//   2 = receiver world position as RGB (smooth colour gradient -> swP valid; flat -> swP garbage)
	//   6 = coverage: red = summed occlusion swOcc, green = -swOcc (any colour -> edges produce coverage)
	//   0 = real shadows.
	if( swDbg == 7 )      { result.color = float4( 1.0, 0.0, 0.0, 1.0 ); }				// solid red for any soft-lit fragment (does the soft path run?)
	else if( swDbg == 2 ) { result.color = float4( frac( swP / 64.0 ), 1.0 ); }			// receiver world pos (smooth gradient => swP valid)
	else if( swDbg == 6 ) { result.color = float4( saturate( swOcc ), 0.0, 0.0, 1.0 ); }	// occlusion: red = occluded (shadow), black = lit
	else if( swDbg == 9 ) { result.color = float4( frac( float( swFirstElem ) / 256.0 ), frac( float( swN ) / 64.0 ), 0.0, 1.0 ); }	// R = first-element param, G = edge count param
#endif
}
