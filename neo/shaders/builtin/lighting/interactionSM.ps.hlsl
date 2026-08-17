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
// The buffer is the vertex-cache joint buffer, whose struct stride is sizeof(float4)=16. So read it as
// float4 elements: each edge is TWO consecutive float4 (e0 = xyz world endpoint 0 + w silWeight; e1 =
// xyz endpoint 1). Edge se lives at elements [se*2], [se*2+1].
StructuredBuffer<float4> t_SoftEdges : register( t12 VK_DESCRIPTOR_SET( 0 ) );
// Tile binning (r_softShadowTileBin): per-tile triangle lists written by softtile_bin.cs.hlsl,
// [count | SW_TILE_K indices] per 16x16 tile. rpUser7 = ( base | -1, tilesX, tileOx, tileOy ).
#define SW_TILE_K 512
#define SW_TILE_SIZE 16
#define SW_TILE_UMBRA 0xFFFFFFFEu	// whole-tile umbra sentinel (softtile_bin.cs.hlsl): occlusion is exactly 1
#define SW_TILE_SPILL 0xFFFFFFFDu	// overflowed tile spilled its FULL list: slot+1/+2 = ( span offset, count )
StructuredBuffer<uint> t_SoftTiles : register( t13 VK_DESCRIPTOR_SET( 0 ) );
// Compute-decoupled term (r_softShadowCompute): softterm.cs.hlsl already evaluated this light's
// coverage integral (same include, exact positions from the softpos G-buffer) into an R32F atlas of
// screen-size slots. rpUser6 = ( mode 0|1, slot offset x, y, unused ); mode 1 replaces the whole
// in-shader integral with one Load. Bound to a dummy (black) texture when the pass is off.
Texture2D t_SoftTerm : register( t14 VK_DESCRIPTOR_SET( 0 ) );
// Included AFTER t_SoftEdges: SoftShadow_WedgeOcclusion reads that global directly (HLSL), so the
// declaration must be in scope at include time.
#include "softwedge_coverage.inc.hlsl"
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
	// Material sampling, the local normal and the Lambert term are computed AFTER the shadow block below
	// (moved down from here). The shadow computation uses none of them (verified), so this reorder is
	// lossless - but keeping their registers out of the soft-shadow coverage loop matters: the loop's live
	// state plus these material values pushed the pixel shader to 60 VGPRs = 24 waves/SIMD on gfx1100, where
	// <= 48 VGPRs would give 32 waves. Freeing the material registers across the loop targets that jump.

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
	// SIGN of the record count selects the coverage path: negative = FRONT-FACE stream (r_softShadowFaceCoverage,
	// the accurate receiver-disk coverage), positive = light-silhouette edge stream. abs() gives the real count.
	bool   swFace = swN < 0;
	swN = abs( swN );

	// nvrhi doesn't apply the structured-buffer range byteOffset to the shader index, so index from an
	// explicit first element (this light's edges start here), passed in rpJitterTexOffset.x.
	int swFirstElem = int( pc.rpJitterTexOffset.x );
	// FACE mode (stream v2): rpJitterTexOffset.y carries the caster-table base (float4 elements) -
	// the wedge path keeps the centre-lit flag in that slot (the face path never reads centre-lit).
	int swCasterBase = swFace ? int( pc.rpJitterTexOffset.y ) : 0;

	// per-fragment sample-set rotation angle: WORLD-POSITION hash (the original scheme, bit-identical
	// to when it lived inside the coverage function). A screen-anchored blue-noise rotation was tried
	// against the contour banding and REJECTED by play-test (2026-08-17): no visible improvement - the
	// banding/grain complaint traced mostly to VRS 2x2 coarseness, and world-anchored rotation keeps
	// the pattern glued to surfaces under camera motion where screen-anchored noise swims.
	float swRotHash = dot( swP, float3( 12.9898, 78.233, 37.719 ) );
	float swRotAng = ( swRotHash - floor( swRotHash ) ) * 6.28318531;

	// NO origin bias on the coverage rays: the analytic path traces the exact caster triangles from the
	// exact interpolated receiver position, and the "phantom" shadows once blamed on self-intersection
	// turned out to be REAL contact shadows in seams/cracks (gate finding: the blocking triangle exists
	// in the mesh 0.45 units from the receiver - geometry the biased references skip). A lifted origin
	// erased those true contact shadows and its quad-derivative normal made the term view-dependent
	// (continuity defects 12 -> 369). Exactness is the analytic path's whole advantage; keep it.
	float3 swCovP = swP;

	int swDbg = int( pc.rpJitterTexScale.w );	// diagnostic selector (r_softShadowDebugShader), visualised at end of main

	// FALLOFF-FIRST EARLY-OUT: the light's scissor is a rectangle but its influence region is the
	// projection x falloff support inside it; outside that support the interaction contributes
	// EXACTLY zero regardless of the shadow term (lightColor multiplies everything), so the
	// integral is provably skippable. Hoisted from below (single consumer, line ~915) - the
	// samples live across the walk only on pixels that actually shade. Debug modes never skip:
	// the gate's term instruments (dbg 6/8 readbacks, anaTerm hashes) must stay full-field.
	float4 swLightFalloff = idtex2Dproj( s_Lighting, t_LightFalloff, fragment.texcoord2 );
	float4 swLightProj    = idtex2Dproj( s_Lighting, t_LightProjection, fragment.texcoord3 );
	const bool swZeroLight = ( swDbg == 0 ) &&
							 ( max( max( swLightProj.x * swLightFalloff.x, swLightProj.y * swLightFalloff.y ),
									swLightProj.z * swLightFalloff.z ) <= 0.0 );

	// Light-disk coverage: sum each caster's silhouette against the area-light disk (see
	// softwedge_coverage.inc.hlsl). This is the SAME function the unit tests compile as C++
	// (neo/tests/SoftShadowCoverage_test.cpp via hlsl_compat.h), so the tested math IS the shipped math.
	float shadow;
	float swLocFr = -1.0;		// DIAG (swDbg 10): blocker fraction; -1 = pcss off / receiver outside the face
	float swLocRecv = 0.0;		// DIAG (swDbg 11): receiver depth [0,1]
	float swLocSamp = 0.0;		// DIAG (swDbg 12): shadow-map depth sampled at the receiver's projected texel
	float2 swLocUV = float2( -1, -1 );	// DIAG 13: projected shadow xy (should land in [0,1] within the face)
	float swLocW = 0.0;			// DIAG 14: perspective divisor w (sign matters)
	float swLocFace = 0.0;		// DIAG 15: selected cube face / 5
#if USE_SHADOW_ATLAS
	// STANDALONE PCSS soft shadow (r_shadowMapPCSS): the shadow atlas alone produces the full contact-hardened
	// soft shadow (Fernando 2005) - a blocker search sizes the penumbra, a penumbra-scaled PCF draws the gradient,
	// no analytic coverage integral. pcssScale (rpShadowAtlasOffsets[].z, 0 when r_shadowMapPCSS off) gates the
	// path: off -> the pure analytic wedge everywhere, so the cheap PCSS and the analytic are A/B-comparable live.
	// The atlas depth,
	// shadow matrices, offsets and screen-correction are all bound because the light is ImageAtlasPlaced.
	// ponytail: correctness rides on r_useShadowAtlas (atlas depth must be bound at t5); pcssScale>0 implies it.
	// pcssScale sign is a 3-way mode select (set in RenderBackend per light):
	//   > 0  this light owns an atlas tile -> run PCSS (the cheap standalone path below).
	//   < 0  PCSS MODE but this light got NO atlas tile (atlas exhausted). PCSS is a SEPARATE, cheaper mode chosen
	//        on hardware that cannot afford the analytic wedge, so regressing to the wedge here is NOT acceptable -
	//        it would spike exactly the machines that opted out of it. There is no tile to sample, so leave this one
	//        light unshadowed (cheap, honest degradation). The fit-budget tile cap makes this essentially
	//        unreachable; the branch exists only so an atlas overflow can never resurrect the wedge under PCSS.
	//   == 0 ANALYTIC mode (r_shadowMapPCSS off): the pure wedge, as explicitly chosen.
	float pcssScale = pc.rpShadowAtlasOffsets[ 0 ].z;
	// FACE mode NEVER touches PCSS (explicit directive): the term is the exact analytic integral,
	// full stop. PCSS is temporally unstable here - its shading, its atlas-resolution limits, AND
	// its locator fast-outs (a low-res blocker search deciding "fully lit, skip the analytic" can
	// erase a real thin shadow) must not be able to change a face-mode pixel. The nature of a
	// shadow must not depend on where the camera looks; one code path, one look. PCSS survives
	// only for the legacy L-sil wedge stream below.
	if( swZeroLight )
	{
		// zero light contribution at this pixel: the term is multiplied by 0 downstream, any value
		// is exact. 1.0 keeps the unshadowed invariants (and the branch is wave-coherent at the
		// scissor corners / outside the projection support, where whole waves skip the walk).
		shadow = 1.0;
	}
	else if( swFace && pc.rpUser6.x > 0.5 )
	{
		// COMPUTE-DECOUPLED term (r_softShadowCompute): the per-light softterm dispatch already ran
		// this exact integral at this exact position (softpos G-buffer); just Load the term. R32F
		// storage + shared include + identical inputs => bit-exact vs the else-branch below (the
		// designed A/B property: anaTerm hashes must match with the cvar 0 vs 1).
		shadow = t_SoftTerm.Load( int3( int2( fragment.position.xy ) + int2( int( pc.rpUser6.y ), int( pc.rpUser6.z ) ), 0 ) ).r;
	}
	else if( swFace )
	{
		// TILE-BINNED walk when the prepass ran (rpUser7.x >= 0): this fragment's 16x16 tile carries
		// the conservative list of triangles that can occlude any receiver in the tile, so the walk
		// touches only those. Overflowed tiles (count sentinel) and out-of-rect fragments fall back
		// to the full walk - the result is bit-exact either way (the list is a conservative superset
		// re-culled per fragment; a triangle outside it can hit no sample ray of this fragment).
		float swOcc;
		int swTileBase = int( pc.rpUser7.x );
		int swTx = int( fragment.position.x ) / SW_TILE_SIZE - int( pc.rpUser7.z );
		int swTy = int( fragment.position.y ) / SW_TILE_SIZE - int( pc.rpUser7.w );
		int  swSlot = -1;
		uint swCnt  = 0xFFFFFFFFu;
		if( swTileBase >= 0 && swTx >= 0 && swTy >= 0 )
		{
			swSlot = swTileBase + ( swTy * int( pc.rpUser7.y ) + swTx ) * ( SW_TILE_K + 1 );
			swCnt  = t_SoftTiles[ swSlot ];
		}
		const bool swBinned = ( swSlot >= 0 ) && ( swCnt != 0xFFFFFFFFu ) && ( swCnt != SW_TILE_UMBRA ) && ( swCnt != SW_TILE_SPILL );
		if( ( swSlot >= 0 ) && ( swCnt == SW_TILE_UMBRA ) )
		{
			swOcc = 1.0;	// whole tile provably in umbra (bin sentinel): the integral saturates to 1
		}
		else if( ( swSlot >= 0 ) && ( swCnt == SW_TILE_SPILL ) )
		{
			// overflowed tile: the span holds this tile's surviving CLUSTER records (stream v3) -
			// the two-level walk amortizes the cone cull ~3.6x exactly where lists are huge
			const uint swOfs = t_SoftTiles[ swSlot + 1 ];
			const uint swSpN = t_SoftTiles[ swSlot + 2 ];
			swOcc = SoftShadow_FaceCoverageClusterList( swCovP, swL, swR, swFirstElem, int( swOfs ), int( swSpN ), swRotAng );
		}
		else if( swBinned )
		{
			swOcc = SoftShadow_FaceCoverageList( swCovP, swL, swR, swFirstElem, swSlot + 1, int( swCnt ), swRotAng );
		}
		else
		{
			swOcc = SoftShadow_Coverage( swCovP, swL, swR, swFirstElem, swCasterBase, swN, pc.rpJitterTexOffset.y, swFace, swRotAng );
		}
		// NO penumbra second-pass refinement: a 2x16-sample average (independent rotations, penumbra
		// fragments only) was tried against the contour banding and REMOVED (play-test 2026-08-17) -
		// the bands are insensitive to the rotation scheme entirely (hash vs blue noise: "no
		// difference"), so doubling the effective sample count bought nothing visible for ~15-20% of
		// the heavy-scene frame time. The bands are structural, not sampling noise; find the actual
		// mechanism before spending samples on it.
		shadow = 1.0 - saturate( swOcc );
	}
	else if( pcssScale < 0.0 )
	{
		shadow = 1.0;
	}
	else if( pcssScale == 0.0 )
	{
		float swOcc = SoftShadow_Coverage( swCovP, swL, swR, swFirstElem, swCasterBase, swN, pc.rpJitterTexOffset.y, swFace, swRotAng );
		shadow = 1.0 - saturate( swOcc );
	}
	else
	{
		// pick the cube face the receiver faces (mirrors the shadow-map projection path)
		int swSI = 0;
#if LIGHT_POINT
		float3 swTL = normalize( fragment.texcoord8.xyz );
		float swAx[6] = { -swTL.x, swTL.x, -swTL.y, swTL.y, -swTL.z, swTL.z };
		for( int fi = 1; fi < 6; fi++ )
		{
			if( swAx[fi] > swAx[swSI] )
			{
				swSI = fi;
			}
		}
#endif
		float swFrac = pc.rpShadowAtlasOffsets[ swSI ].w;	// atlas rect scale (plumbed here; rpJitterTexScale.y holds the soft minWidth)
		// NORMAL-OFFSET BIAS: lift the receiver's SAMPLE POSITION along its surface normal toward the light BEFORE
		// projecting, instead of biasing the compared DEPTH. On a grazing floor a depth bias cannot stop self-shadow
		// acne without receding the occluder's contact by a large world distance - acne and peter-pan are the SAME
		// knob there. Offsetting the position off the surface removes self-shadowing WITHOUT moving the occluder's
		// shadow, so the contact stays under the caster like RT (which has no bias at all). Offset scales with the
		// WORLD size of a shadow texel at the receiver (~ dist-to-light / tile-resolution); r_shadowMapPCSSBias = the
		// texel count. Geometric normal from screen derivatives (no per-vertex normal plumbing needed).
		float3 swNrmW = cross( ddx( swP ), ddy( swP ) );
		float4 swMPb  = swMP;
		if( length( swNrmW ) > 1e-8 )
		{
			float3 swNrmM  = normalize( cross( ddx( swMP.xyz ), ddy( swMP.xyz ) ) );
			float  swNSign = ( dot( swNrmW, swL - swP ) < 0.0 ) ? -1.0 : 1.0;	// point toward the light
			// 2.07 = 2*tan(46deg), the per-texel angular span of the 92deg shadow cube face (r_shadowMapFrustumFOV);
			// approximate - the whole offset is a heuristic re-tunable via r_shadowMapPCSSBias, so a plumbed FOV isn't
			// worth a uniform. worldTexel = dist-to-light * angular-texel / tile-fraction.
			float  swWorldTexel = length( swL - swP ) * 2.07 * pc.rpScreenCorrectionFactor.z / max( swFrac, 1e-6 );
			// SLOPE SCALING: the depth error a shadow texel induces on the receiver grows as tan(angle
			// between surface and light) - at grazing incidence (floor lit by a near-floor light) a
			// constant-texel offset is an order of magnitude short, which paints broad acne bands where
			// the analytic sees nothing (gate finding: the erebus1_05 stripe). Scale the offset by
			// 1/(N.L), capped so steep geometry keeps the small proven offset and the cap bounds any
			// added contact recession to a few texels.
			float  swNL = abs( dot( normalize( swNrmW ), normalize( swL - swP ) ) );
			float  swSlope = min( 1.0 / max( swNL, 0.05 ), 20.0 );
			swMPb.xyz += swNSign * swNrmM * ( pc.rpJitterTexOffset.z * 4.0 * swWorldTexel * swSlope );
		}
		float4 swSTC;
		swSTC.x = dot4( swMPb, pc.rpShadowMatrices[ swSI * 4 + 0 ] );
		swSTC.y = dot4( swMPb, pc.rpShadowMatrices[ swSI * 4 + 1 ] );
		swSTC.z = dot4( swMPb, pc.rpShadowMatrices[ swSI * 4 + 2 ] );
		swSTC.w = dot4( swMPb, pc.rpShadowMatrices[ swSI * 4 + 3 ] );
		swLocW = swSTC.w;			// DIAG (pre-divide)
		swLocFace = float( swSI ) / 5.0;	// DIAG
		swSTC.xyz /= swSTC.w;
		swLocUV = swSTC.xy;			// DIAG (post-divide projected xy)
		// Raw projected depth - NO multiplicative receiver bias. rpScreenCorrectionFactor.w
		// (r_shadowMapRegularDepthBiasScale ~= 0.999) receded the compare depth by ~0.001, which on a grazing floor
		// is a visible contact GAP (the shadow starts away from the caster). The normal-offset above already prevents
		// self-shadow, so no depth bias belongs here; only the tiny swConstBias noise margin remains.
		float swRecv = swSTC.z;
		float2 swBase = swSTC.xy * swFrac + pc.rpShadowAtlasOffsets[ swSI ].xy;
		// FAITHFUL PCSS blocker-search radius: the light-disk footprint projected to THIS receiver scales with
		// (d_receiver - d_near)/d_receiver. For this infinite-far D3D projection (d = 1 - n/z_eye) that factor reduces
		// exactly to the window depth swRecv, so the search - and the penumbra it feeds - grows with distance-to-light
		// instead of being a fixed texel count. Floored at one texel so a near-light receiver still samples.
		float swSearch = max( pcssScale * pc.rpScreenCorrectionFactor.z * swRecv, pc.rpScreenCorrectionFactor.z );
		float swPhi = BlueNoise( fragment.position.xy, 1.0 );
		// The receiver-plane depth bias (Isidoro) and its slope-scaled constant - the peter-pan source - are GONE,
		// replaced by the position normal-offset above. All that remains is a tiny fixed float/derivative-noise
		// margin so a texel exactly on the receiver plane doesn't flicker; it is slope-INDEPENDENT so it can never
		// recede the contact.
		float swConstBias = 0.00005;
		// STANDALONE PCSS (Fernando 2005), full contact-hardening variant. The atlas IS depth-readable with the
		// non-comparison sampler (s_Lighting) now that t5 binds the real atlas (was the empty rtShadowMask, which
		// read 0 -> the old "TD_DEPTH can't be read raw" note); dbg12 raw depth tracks dbg11 receiver depth 1:1.
		// PASS 1 - BLOCKER SEARCH: average the raw stored depth of texels CLOSER than the receiver over the
		// light-disk footprint. That average distance drives the penumbra width; no blocker in the footprint =>
		// fully lit, skip pass 2.
		// Clamp every atlas fetch to THIS light's tile rect (minus a half-texel guard). A large penumbra (big
		// swFilter) or a small tile (fit-budget shrink) can push swBase+off outside the tile into a NEIGHBOURING
		// light's atlas texels, giving garbage blocker depth and a wrong PCF - a localized shadow smear. Clamping
		// keeps all samples inside the caster's own tile.
		float2 swTileLo = pc.rpShadowAtlasOffsets[ swSI ].xy + 0.5 * pc.rpScreenCorrectionFactor.z;
		float2 swTileHi = pc.rpShadowAtlasOffsets[ swSI ].xy + swFrac - 0.5 * pc.rpScreenCorrectionFactor.z;
		// Loop-invariant, hoisted: no per-tap depth bias (the normal-offset already lifted the sample off the
		// surface), so compare stored depth directly against the receiver depth minus the tiny noise margin.
		float swRecvAt = swRecv - swConstBias;
		float swBlkSum = 0.0;
		float swBlkCnt = 0.0;
		float swBlkMax = 0.0;	// NEAREST-to-receiver blocker (largest stored depth) - drives CONTACT hardening
		for( float bi = 0.0; bi < 16.0; bi += 1.0 )
		{
			float2 swOff = VogelDiskSample( bi, 16.0, swPhi ) * swSearch;
			float bd = t_ShadowAtlas.SampleLevel( s_Lighting, clamp( swBase + swOff, swTileLo, swTileHi ), 0 ).r;
			if( bd < swRecvAt && bd < 0.999 )	// stored 1.0 = cleared/far: never a blocker
			{
				swBlkSum += bd;
				swBlkCnt += 1.0;
				swBlkMax = max( swBlkMax, bd );
			}
		}
		// Centre tap (the light-disk-centre ray): swCentreLit = 1 if the centre is visible (penumbra RING),
		// 0 if blocked (umbra CORE). Drives the analytic wedge's winding handling and the deep-umbra fast-out.
		float swBdC = t_ShadowAtlas.SampleLevel( s_Lighting, clamp( swBase, swTileLo, swTileHi ), 0 ).r;
		float swCentreLit = ( swBdC < swRecvAt && swBdC < 0.999 ) ? 0.0 : 1.0;
		swLocFr = swBlkCnt / 16.0;								// DIAG (10): blocker fraction
		swLocRecv = swRecv;										// DIAG (11)
		swLocSamp = ( swBlkCnt > 0.0 ) ? swBlkSum / swBlkCnt : 1.0;	// DIAG (12): average blocker depth
		if( swBlkCnt < 0.5 )
		{
			shadow = 1.0;	// no blocker in the search footprint => fully lit, skip the PCF
		}
		else
		{
			// PASS 2 - PENUMBRA WIDTH by similar triangles, then a PCF over a kernel scaled to it: the closer the
			// blocker sits to the receiver the smaller the kernel -> sharp CONTACT shadow that softens with distance.
			// Clamp to [1 texel, full light footprint] so contact stays crisp and the kernel can't exceed the light.
			// The similar-triangles ratio (zRecv - zBlk)/zBlk MUST be in LINEAR light-space distance, but swRecv /
			// swAvgBlk are non-linear shadow-map z. For this point-light projection (far plane at infinity, window
			// z = 0.5*ndc+0.5) linear distance z_eye = k/(1-d), so the ratio reduces to (dRecv-dBlk)/(1-dRecv) - the
			// k cancels, no near plane needed. Dividing by raw swAvgBlk instead (the old bug) collapsed the ratio
			// toward 0 near the far plane - exactly where an elevated caster's floor receiver sits - so a crate
			// floating well above the floor got a HARD blob instead of the widest, softest penumbra.
			float swAvgBlk = swBlkSum / swBlkCnt;	// kept for DIAG (dbg12)
			// CONTACT HARDENING: size the penumbra from the NEAREST blocker (swBlkMax, deepest stored depth), not
			// the average. A resting/embedded object's search footprint also catches its own RAISED BODY, which
			// inflates the average blocker distance and keeps the penumbra wide even AT the contact - so the umbra
			// softens and pulls away from the object (the peter-panning gap; RT, which resolves per-ray occlusion,
			// keeps the contact hard). The nearest blocker at a contact point IS the object's base (~receiver
			// depth) => swPen ~ 0 => crisp contact; away from the object the nearest blocker is the raised
			// silhouette => swPen grows => soft. Matches the RT reference: sharp at contact, soft with distance.
			// max(1-swRecv, 1e-3) floors the denominator: near the far plane (swRecv -> 1) a tiny depth error would
			// otherwise swing swPen by ~1e4; 1e-3 caps the ratio so the penumbra can't explode (the swFilter clamp is
			// the second backstop). Only affects receivers past swRecv ~0.999.
			float swPen    = ( swRecv - swBlkMax ) / max( 1.0 - swRecv, 1e-3 ) * pcssScale;
			// The PCF (filter) radius must be free to grow to the full penumbra - it is NOT the blocker-search
			// radius. Capping it at swSearch (= pcssScale texels, the SEARCH footprint) is the bug that made every
			// elevated caster hard: a crate floating high above the floor wants a penumbra far wider than the few
			// texels used to FIND its blocker, but the clamp pinned it there. Allow up to 6x the search footprint
			// (still bounded so 24 taps stay adequately dense); contact (swPen~0) still clamps to 1 texel = crisp.
			float swFilter = clamp( swPen * pc.rpScreenCorrectionFactor.z, pc.rpScreenCorrectionFactor.z, 6.0 * swSearch );
			// (face mode never reaches this branch - it is purely analytic with no PCSS/atlas reads at all)
			float swLitSum = 0.0;
			for( float fi = 0.0; fi < 24.0; fi += 1.0 )
			{
				float2 fOff = VogelDiskSample( fi, 24.0, swPhi ) * swFilter;
				// swRecvAt hoisted above; COMPARISON sampler returns the LIT fraction (stored passes vs swRecvAt).
				swLitSum += t_ShadowAtlas.SampleCmpLevelZero( s_Shadow, clamp( swBase + fOff, swTileLo, swTileHi ), swRecvAt );
			}
			float pcssLit = swLitSum / 24.0;	// soft penumbra gradient, contact-hardened

			// HYBRID (r_shadowMapPCSSAnalyticContact, plumbed in rpJitterTexOffset.w): in the conservative PENUMBRA
			// band (partial occlusion), run the EXACT analytic silhouette-edge integral for physically correct contact
			// hardening and a smooth edge (kills the shadow-map texel staircase), then take the MORE-occluded of it and
			// the PCSS PCF so edge-less dynamic casters (silEdges==NULL -> invisible to the analytic) keep their atlas
			// shadow. Full UMBRA (all taps blocked AND centre blocked) and the LIT case both skip the analytic, so the
			// O(edges) integral runs only in the thin band. swCentreLit (centre tap) picks the wedge's ring/core winding.
			// DEEP-UMBRA FAST-OUT (perf only): skip the O(edges) analytic where it would add nothing. All 16 search taps
			// blocked AND the centre blocked marks umbra, but that set INCLUDES the inner boundary band: the narrow search
			// footprint (swSearch) saturates to 16/16 one texel INSIDE the silhouette, while the WIDE penumbra PCF
			// (24 taps over swFilter, the full penumbra width) still catches lit shadow-map texels there -> pcssLit > 0.
			// That band is exactly the penumbra->umbra transition whose contour the analytic must draw pixel-exact instead
			// of the shadow-map staircase, so it must NOT be skipped. pcssLit collapses to ~0 only once the fragment is
			// deeper than a full penumbra-width inside the umbra - no part of the filter kernel reaches past the silhouette
			// - which is the genuine deep interior. Gate on pcssLit so the thin boundary band runs the analytic (exact umbra
			// contour) and only the saturated interior fast-outs; the analytic's own occ>=0.999 early-break is the backstop.
			bool swDeepUmbra = ( swBlkCnt > 15.5 ) && ( swCentreLit < 0.5 ) && ( pcssLit < 0.002 );
			if( pc.rpJitterTexOffset.w > 0.5 && !swDeepUmbra )
			{
				float swOcc = SoftShadow_Coverage( swCovP, swL, swR, swFirstElem, swCasterBase, swN, swCentreLit, swFace, swRotAng );
				// legacy L-sil wedge only (face mode exits above): the wedge undershoots off-axis, so it
				// keeps the PCSS fallback for occlusion its silhouette stream cannot see.
				shadow = ( swOcc > 0.003 ) ? ( 1.0 - saturate( swOcc ) ) : pcssLit;
			}
			else
			{
				shadow = pcssLit;
			}
		}
	}
#else
	float swOcc = SoftShadow_Coverage( swCovP, swL, swR, swFirstElem, swCasterBase, swN, pc.rpJitterTexOffset.y, swFace, swRotAng );
	shadow = 1.0 - saturate( swOcc );
#endif
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
#if USE_SHADOW_ATLAS
	// PCSS: rpShadowAtlasOffsets[shadowIndex].z carries the light-size scale (0 = off). Blocker search reads
	// the RAW atlas depth (non-comparison s_Lighting), averages blockers closer than the receiver, and scales
	// the PCF radius by the similar-triangles penumbra width -> contact hardening. That width is also the
	// coarse penumbra locator for the analytic hybrid (near-zero => fully lit or fully shadowed).
	float pcssScale = pc.rpShadowAtlasOffsets[ shadowIndex ].z;
	if( pcssScale > 0.0 )
	{
		float2 atlasBase = shadowTexcoord.xy * pc.rpJitterTexScale.y + pc.rpShadowAtlasOffsets[ shadowIndex ].xy;
		float searchR = pcssScale * pc.rpScreenCorrectionFactor.z;
		float blockerSum = 0.0;
		float blockerCnt = 0.0;
		for( float bi = 0.0; bi < 16.0; bi += 1.0 )
		{
			float2 bj = VogelDiskSample( bi, 16.0, vogelPhi );
			float bd = t_ShadowAtlas.SampleLevel( s_Lighting, atlasBase + bj * searchR, 0 ).r;
			if( bd < receiver )
			{
				blockerSum += bd;
				blockerCnt += 1.0;
			}
		}
		if( blockerCnt > 0.0 )
		{
			float avgBlocker = blockerSum / blockerCnt;
			float penumbra = ( receiver - avgBlocker ) / max( avgBlocker, 1e-4 ) * pcssScale;
			shadowTexelSize = penumbra * pc.rpScreenCorrectionFactor.z;
		}
	}
#endif
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

	// Material sampling + normal + Lambert, moved here from the top of main() so their registers are not live
	// across the soft-shadow coverage loop (occupancy: see the note above the shadow block).
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
#if USE_SOFT_WEDGE
	// already sampled above for the falloff-first early-out; identical coordinates, single sample
	float4 lightFalloff =	swLightFalloff;
	float4 lightProj =		swLightProj;
#else
	float4 lightFalloff =	idtex2Dproj( s_Lighting, t_LightFalloff, fragment.texcoord2 );
	float4 lightProj =		idtex2Dproj( s_Lighting, t_LightProjection, fragment.texcoord3 );
#endif
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
	else if( swDbg == 6 ) { result.color = float4( saturate( SoftShadow_Coverage( swCovP, swL, swR, swFirstElem, swCasterBase, swN, pc.rpJitterTexOffset.y, swFace, swRotAng ) ), 0.0, 0.0, 1.0 ); }	// occlusion: red = occluded (shadow), black = lit
	else if( swDbg == 9 ) { result.color = float4( frac( float( swFirstElem ) / 256.0 ), frac( float( swN ) / 64.0 ), 0.0, 1.0 ); }	// R = first-element param, G = edge count param
		else if( swDbg == 8 ) { result.color = float4( shadow, shadow, shadow, 1.0 ); }	// isolated shadow visibility (1 = lit, 0 = shadowed); same convention as rtShadowMaskImage -> RT-vs-analytic term diff
		else if( swDbg == 10 ) { result.color = ( swLocFr < 0.0 ) ? float4( 0.0, 0.0, 0.4, 1.0 ) : float4( swLocFr, 1.0 - swLocFr, 0.0, 1.0 ); }	// LOCATOR: green = lit (frac 0), red = umbra (frac 1), blue = pcss off / outside face
		else if( swDbg == 11 ) { result.color = float4( swLocRecv, swLocRecv, swLocRecv, 1.0 ); }	// receiver depth [0,1] (smooth gradient => projection sane)
		else if( swDbg == 12 ) { result.color = float4( swLocSamp, swLocSamp, swLocSamp, 1.0 ); }	// shadow-map depth at the receiver's texel (should track receiver depth in lit regions)
		else if( swDbg == 13 ) { result.color = float4( saturate( swLocUV.x ), saturate( swLocUV.y ), ( swLocUV.x < 0.0 || swLocUV.x > 1.0 || swLocUV.y < 0.0 || swLocUV.y > 1.0 ) ? 1.0 : 0.0, 1.0 ); }	// projected in-face UV (blue = OUT of [0,1] => bad projection)
		else if( swDbg == 14 ) { result.color = float4( saturate( swLocW * 0.0005 ), saturate( -swLocW * 0.0005 ), 0.0, 1.0 ); }	// perspective w: red = positive, green = negative (behind)
		else if( swDbg == 15 ) { result.color = float4( swLocFace, 1.0 - swLocFace, 0.0, 1.0 ); }	// selected cube face / 5
#endif
}
