/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2014-2024 Robert Beckebans
Copyright (C) 2014-2016 Kot in Action Creative Artel

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
#include "precompiled.h"
#pragma hdrstop

#if defined(USE_INTRINSICS_SSE)
	#if MOC_MULTITHREADED
		#include "CullingThreadPool.h"
	#else
		#include "../libs/moc/MaskedOcclusionCulling.h"
	#endif
#endif

#include "RenderCommon.h"
#include "RenderCapture.h"
#include "Model_local.h"
#include "Passes/SoftShadowClassify.h"
#include "SoftShadowHull.h"		// brush-recovery: per-area hull table + record encoding

extern idCVar r_useRTShadows;	// RT shadows need occluders whose shadow is off-view (RenderSystem_init.cpp)
extern idCVar r_useSoftShadowVolumes;	// soft shadow volumes reuse the stencil shadow-volume geometry (RenderSystem_init.cpp)
extern idCVar r_softShadowKeepOffViewCasters;	// keep off-view casters whose PENUMBRA reaches the view (RenderSystem_init.cpp)
extern idCVar r_rtShadowCullOffView;	// trim the shadow TLAS: cull casters whose shadow misses the view (RtShadowsPass.cpp)
// dynamic stencil shadow volume for moved / non-static casters, built per-frame from the
// model's static silEdges + doubled shadowCache against the current light (defined in Interaction.cpp).
srfTriangles_t* R_CreateInteractionShadowVolume( const idRenderEntityLocal* ent, const srfTriangles_t* tri, const idRenderLightLocal* light );
// analytic soft shadows: per-silhouette-edge world-space edges for per-fragment coverage (Interaction.cpp).
void R_CollectPenumbraEdges( const idRenderEntityLocal* ent, const srfTriangles_t* tri, const idRenderLightLocal* light,
							 float penumbraSize, const float* modelToWorld,
							 softShadowEdge_t** outEdges, int* outNumEdges );
// analytic soft shadows: FRONT-FACE coverage stream v2 (3 float4 per caster triangle) - accurate receiver-disk
// coverage, temporally stable; the light-silhouette path above undershoots off-axis (Interaction.cpp).
void R_CollectPenumbraFaces( const idRenderEntityLocal* ent, const srfTriangles_t* tri, const idRenderLightLocal* light,
							 float penumbraSize, const float* modelToWorld,
							 idVec4** outElems, int* outNumElems, idVec4** outClusters, int* outNumClusters, bool* outIsBox = NULL );
extern idCVar r_shadowPenumbraSize;	// soft shadow volumes: light source radius (RenderSystem_init.cpp)
extern idCVar r_shadowPenumbraAuto, r_shadowPenumbraAutoScale;

// Per-light soft-shadow emitter radius (task #105: lights sized by their PHYSICAL emitters, not one
// global). Priority: authored "penumbraSize" entity key (the physical truth - a disk approximates any
// fixture shape; the radius, not the shape, drives penumbra width), else a heuristic from the light's
// own extents (fixtures scale weakly with their volumes), else the r_shadowPenumbraSize global. The
// global also CAPS the derived value, so no light ever gets a larger emitter than the legacy default.
// Smaller emitters shrink the penumbra band, the per-fragment cull cones and the tile lists together -
// the attribution runs measured those as the dominant per-pixel work multipliers.
float R_SoftPenumbraRadius( const idRenderLightLocal* lightDef )
{
	const float globalR = Max( r_shadowPenumbraSize.GetFloat(), 1e-2f );
	if( lightDef == NULL )
	{
		return globalR;
	}
	const float authored = lightDef->parms.penumbraSize;
	if( authored > 0.0f )
	{
		return Min( authored, 128.0f );
	}
	if( !r_shadowPenumbraAuto.GetBool() )
	{
		return globalR;
	}
	float ext;
	if( lightDef->parms.pointLight )
	{
		ext = Min( lightDef->parms.lightRadius.x, Min( lightDef->parms.lightRadius.y, lightDef->parms.lightRadius.z ) );
	}
	else
	{
		ext = Min( lightDef->parms.right.Length(), lightDef->parms.up.Length() );	// projected: aperture scale
	}
	const float derived = ext * r_shadowPenumbraAutoScale.GetFloat();
	return Max( Min( 1.0f, globalR ), Min( derived, globalR ) );
}
extern idCVar r_softShadowFaceCoverage;	// 1 = stream caster faces + front-face coverage instead of light silhouette

idCVar r_skipStaticShadows( "r_skipStaticShadows", "0", CVAR_RENDERER | CVAR_BOOL, "skip static shadows" );
idCVar r_skipDynamicShadows( "r_skipDynamicShadows", "0", CVAR_RENDERER | CVAR_BOOL, "skip dynamic shadows" );
idCVar r_useParallelAddModels( "r_useParallelAddModels", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_NOCHEAT, "add all models in parallel with jobs" );
idCVar r_useParallelAddShadows( "r_useParallelAddShadows", "1", CVAR_RENDERER | CVAR_INTEGER | CVAR_NOCHEAT, "0 = off, 1 = threaded", 0, 1 );
idCVar r_forceShadowCaps( "r_forceShadowCaps", "0", CVAR_RENDERER | CVAR_BOOL, "0 = skip rendering shadow caps if view is outside shadow volume, 1 = always render shadow caps" );
// WIP: restore stencil shadow volumes. When on, static shadow-casting surfaces build a
// stencil shadow-volume drawSurf (into vLight->globalShadows/localShadows) instead of a
// shadow-map caster. M4 will fold this into an r_shadowMethod selector, default stencil.
idCVar r_useStencilShadows( "r_useStencilShadows", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL | CVAR_NEW, "use stencil shadow volumes instead of shadow maps (WIP)" );

// M2 diagnostics: cumulative tallies of stencil-volume caster gate outcomes (see R_AddSingleModel).
// Printed by DrawInteractions when r_showShadows is set; names the missing prerequisite when 0 volumes build.
int fe_stencilBuilt = 0;
int fe_softEdgesCollected = 0;	// cumulative soft-shadow silhouette edges collected (diagnostic: 0 => soft-wedge inert)
int fe_occludersBuilt = 0;		// cumulative PCSS-locator atlas OCCLUDER surfs built (diagnostic: 0 => atlas empty for soft lights)
int fe_rejSilEdges = 0;
int fe_rejSurfInter = 0;
int fe_rejNumIdx = 0;
int fe_rejIdxStale = 0;
int fe_rejShadowCache = 0;
int fe_softUmbraCulledTris = 0;	// r_softShadowUmbraAccum: triangles dropped as fully inside kept same-light umbra
int fe_softUmbraRecomputes = 0;	// cache rebuilds (a rebuild per frame = volatile stream, cull effectively off)

// ---- Phase 0 static/dynamic ATTRIBUTION (r_softShadowStaticStats / r_softShadowSkipStatic) --------
// A static-caster x static-light soft shadow is frame-invariant: the analytic term never changes yet is
// re-collected and re-walked every frame. These per-view counters measure how much of the soft caster
// mass is that reusable static half - the ceiling any cross-frame cache could remove. Reset per view in
// R_AddModels, printed in the backend under r_softShadowStaticStats. See plan noble-sniffing-rose.
int fe_softStaticCasters = 0, fe_softDynCasters = 0;	// caster meshes classified static vs dynamic
int fe_softStaticRecords = 0, fe_softDynRecords = 0;	// tri-stream float4 records (the walk-relevant mass)
int fe_softDynLightMoved = 0;						// of the dynamic records: those under a MOVED light
int fe_softDynGeomMoved = 0;						// of the dynamic records: static light but moving/animated caster

// PROXY PROFILER (r_softShadowProxyProfile): one-shot, per distinct caster MODEL. It exists to IDENTIFY a
// specific mesh offline - which named erebus meshes are genuine right-angle boxes (corner gap ~0) versus
// rounded/beveled ones (corner gap > 0) - so the operator can name the right one in r_softShadowProxyModel.
// It never swaps anything; it only reports. Accumulates over one frame of collection, then prints + resets.
struct softProxyProf_t
{
	idStr	name;
	int		casts;			// number of (model,light) collections this frame
	int		records;		// sum of tris across those casts (the walk-relevant mass)
	int		tris;			// tri count of one instance
	float	cornerRatio;	// worst-of-8 AABB-corner gap / diagonal: ~0 = right-angle box, larger = rounded corners
	float	faceRatio;		// deepest vertex INTO the box / diagonal: ~0 = shell hugs the box, larger = interior detail
};
static idList<softProxyProf_t>	s_softProxyProf;
static int						s_softProxyProfFrame = -1;

static void R_SoftProxyProfileMetrics( const srfTriangles_t* tri, int& outTris, float& outCorner, float& outFace )
{
	outTris = tri->numIndexes / 3;
	outCorner = 1.0f;
	outFace = 0.0f;
	const idDrawVert* verts = tri->posedShadowVerts != NULL ? tri->posedShadowVerts : tri->verts;
	if( verts == NULL || tri->numVerts <= 0 )
	{
		return;
	}
	idVec3 mn( 1e30f, 1e30f, 1e30f ), mx( -1e30f, -1e30f, -1e30f );
	for( int v = 0; v < tri->numVerts; v++ )
	{
		const idVec3& p = verts[v].xyz;
		mn.x = Min( mn.x, p.x ); mn.y = Min( mn.y, p.y ); mn.z = Min( mn.z, p.z );
		mx.x = Max( mx.x, p.x ); mx.y = Max( mx.y, p.y ); mx.z = Max( mx.z, p.z );
	}
	const float diag = ( mx - mn ).Length();
	if( diag < 1e-4f )
	{
		return;
	}
	float worstCorner = 0.0f;					// the AABB corner that is FARTHEST from any mesh vertex
	for( int c = 0; c < 8; c++ )
	{
		const idVec3 corner( ( c & 1 ) ? mx.x : mn.x, ( c & 2 ) ? mx.y : mn.y, ( c & 4 ) ? mx.z : mn.z );
		float best = 1e30f;
		for( int v = 0; v < tri->numVerts; v++ )
		{
			const float d = ( verts[v].xyz - corner ).Length();
			if( d < best ) { best = d; }
		}
		if( best > worstCorner ) { worstCorner = best; }
	}
	outCorner = worstCorner / diag;
	float maxGap = 0.0f;
	for( int v = 0; v < tri->numVerts; v++ )
	{
		const idVec3& p = verts[v].xyz;
		const float g = Min( Min( Min( p.x - mn.x, mx.x - p.x ), Min( p.y - mn.y, mx.y - p.y ) ), Min( p.z - mn.z, mx.z - p.z ) );
		maxGap = Max( maxGap, g );
	}
	outFace = maxGap / diag;
}

static void R_SoftProxyProfile( const idRenderEntityLocal* ent, const srfTriangles_t* tri )
{
	extern idCVar r_softShadowProxyProfile;
	// lazy flush: once a full frame has elapsed since the enable, print the table and auto-reset the cvar.
	if( s_softProxyProfFrame >= 0 && tr.frameCount != s_softProxyProfFrame )
	{
		extern int fe_softProxyBoxed;
		extern idCVar r_softShadowProxyModel;
		common->Printf( "[softproxy] per-model profile (%d distinct), sorted by records = tris x casts:\n", s_softProxyProf.Num() );
		common->Printf( "[softproxy] curated model '%s' -> %d casters boxed this frame\n", r_softShadowProxyModel.GetString(), fe_softProxyBoxed );
		common->Printf( "[softproxy]   records casts  tris  cornerGap  faceGap  model\n" );
		const int LIMIT = 80;
		for( int printed = 0; printed < LIMIT; printed++ )
		{
			int best = -1;
			for( int i = 0; i < s_softProxyProf.Num(); i++ )
			{
				if( s_softProxyProf[i].casts >= 0 && ( best < 0 || s_softProxyProf[i].records > s_softProxyProf[best].records ) )
				{
					best = i;
				}
			}
			if( best < 0 ) { break; }
			const softProxyProf_t& e = s_softProxyProf[best];
			common->Printf( "[softproxy] %9d %5d %5d   %7.4f  %7.4f  %s\n", e.records, e.casts, e.tris, e.cornerRatio, e.faceRatio, e.name.c_str() );
			s_softProxyProf[best].casts = -1;		// mark printed
		}
		const bool quitAfter = ( r_softShadowProxyProfile.GetInteger() >= 2 );	// 2 = headless one-shot
		s_softProxyProf.Clear();
		s_softProxyProfFrame = -1;
		r_softShadowProxyProfile.SetInteger( 0 );
		// profile 2 = headless: quit after the table so a loadGame run terminates on its own. profile 1
		// stays alive (interactive playtest can profile without being killed).
		if( quitAfter )
		{
			cmdSystem->AppendCommandText( "quit\n" );
		}
		return;
	}
	if( !r_softShadowProxyProfile.GetBool() )
	{
		return;
	}
	if( s_softProxyProfFrame < 0 ) { s_softProxyProfFrame = tr.frameCount; }
	const char* nm = ( ent != NULL && ent->parms.hModel != NULL ) ? ent->parms.hModel->Name() : "<null>";
	int idx = -1;
	for( int i = 0; i < s_softProxyProf.Num(); i++ )
	{
		if( s_softProxyProf[i].name.Icmp( nm ) == 0 ) { idx = i; break; }
	}
	int tris; float corner, face;
	R_SoftProxyProfileMetrics( tri, tris, corner, face );
	if( idx < 0 )
	{
		softProxyProf_t e;
		e.name = nm; e.casts = 0; e.records = 0; e.tris = tris; e.cornerRatio = corner; e.faceRatio = face;
		idx = s_softProxyProf.Append( e );
	}
	s_softProxyProf[idx].casts++;
	s_softProxyProf[idx].records += tris;
	s_softProxyProf[idx].cornerRatio = Max( s_softProxyProf[idx].cornerRatio, corner );
	s_softProxyProf[idx].faceRatio = Max( s_softProxyProf[idx].faceRatio, face );
}

// =========================================================================================================
// WEDGE SELECTOR (r_softShadowWedgeSelector): route each soft caster to the CHEAPEST method that is EXACT in
// its region; Fubini is only the residual where nothing cheaper is exact. Per-caster method is a tag (enum),
// so the concave interval-union fill (step 1.5) drops into the same dispatch without reworking it.
enum swMethod_t
{
	SW_M_FUBINI = 0,			// residual: NOT eligible (deep+large-light, or non-manifold soup) - permanent Fubini
	SW_M_FILLHULL_CONVEX,		// eligible + coplanar + CONVEX + <=8 hull verts: exact O(verts) FillHull into the shared grid
	SW_M_CONCAVE_INTERVAL		// eligible but concave / >8v / non-coplanar convex: RESERVED per-row interval-union
								//   fill (step 1.5); falls back to Fubini today (NOT a permanent classification)
};

// eligibility (manifold silhouette walk requires silEdges; exact-region = coplanar sheet at any light size,
// or a small enough light that the deep-caster parallax vanishes). Shared by the selector AND the census.
static bool R_SoftWedgeEligible( const srfTriangles_t* tri, const idRenderLightLocal* lightDef,
		const viewLight_t* vLight, const float* modelToWorld, bool& outManifold, bool& outPlanar, bool& outSmall )
{
	extern float R_SoftPenumbraRadius( const idRenderLightLocal* lightDef );
	extern idCVar r_softShadowWedgeSinA;
	outManifold = tri->silEdges != NULL && tri->numSilEdges > 0;
	outPlanar = false;
	outSmall = false;
	if( !outManifold )
	{
		return false;
	}
	const idVec3 ext = tri->bounds[1] - tri->bounds[0];
	int mn = 0;
	if( ext[1] < ext[mn] ) { mn = 1; }
	if( ext[2] < ext[mn] ) { mn = 2; }
	outPlanar = ext[mn] < 0.25f * ext.Length();
	idVec3 lc = ( tri->bounds[0] + tri->bounds[1] ) * 0.5f, wc;
	R_LocalPointToGlobal( modelToWorld, lc, wc );
	const float dist = ( vLight->globalLightOrigin - wc ).Length();
	const float sinA = ( dist > 1e-3f ) ? R_SoftPenumbraRadius( lightDef ) / dist : 1.0f;
	outSmall = sinA <= r_softShadowWedgeSinA.GetFloat();
	return outPlanar || outSmall;
}

static inline float R_SoftHull_Cross2( const idVec2& o, const idVec2& a, const idVec2& b )
{
	return ( a.x - o.x ) * ( b.y - o.y ) - ( a.y - o.y ) * ( b.x - o.x );
}

// COPLANAR convex hull of a caster's world verts. Returns the hull vertex count (3..SW_HULL_MAX_VERTS), with
// outHull filled (world xyz, CCW), when the caster is a SOLID CONVEX flat polygon - its filled area equals
// its convex-hull area (so FillHull/FillPoly is bit-exact for it at any light size). Returns 0 for any
// concavity, hole, double-sided sheet, non-coplanarity, >8 hull verts, or oversized mesh (-> Fubini/reserved).
// This is the losslessness certificate: FillHull only ever runs on a caster proven to fill its convex hull.
static int R_SoftWedgeConvexHull( const srfTriangles_t* tri, const float* modelToWorld, idVec3* outHull,
		int& outHullVerts, bool& outConvex )
{
	outHullVerts = 0;
	outConvex = false;
	const int nv = tri->numVerts;
	if( nv < 3 || nv > 8192 )				// oversized: cost-bound skip; <3: no polygon
	{
		return 0;
	}
	const idDrawVert* verts = tri->posedShadowVerts != NULL ? tri->posedShadowVerts : tri->verts;
	static idVec3 wv[8192];					// static scratch (frontend single-threaded): avoids a large stack frame
	idVec3 c( 0.0f, 0.0f, 0.0f );
	idVec3 bmin, bmax;
	for( int i = 0; i < nv; i++ )
	{
		R_LocalPointToGlobal( modelToWorld, verts[i].xyz, wv[i] );
		c += wv[i];
		if( i == 0 ) { bmin = bmax = wv[0]; }
		else
		{
			bmin.x = Min( bmin.x, wv[i].x ); bmin.y = Min( bmin.y, wv[i].y ); bmin.z = Min( bmin.z, wv[i].z );
			bmax.x = Max( bmax.x, wv[i].x ); bmax.y = Max( bmax.y, wv[i].y ); bmax.z = Max( bmax.z, wv[i].z );
		}
	}
	c /= ( float )nv;
	// Newell-summed face normal + one-sided filled area (sum of world tri areas)
	const triIndex_t* idx = tri->indexes;
	const int ntris = tri->numIndexes / 3;
	idVec3 nrm( 0.0f, 0.0f, 0.0f );
	double triAreaSum = 0.0;
	for( int t = 0; t < ntris; t++ )
	{
		const idVec3& a = wv[idx[t * 3 + 0]];
		const idVec3& b = wv[idx[t * 3 + 1]];
		const idVec3& d = wv[idx[t * 3 + 2]];
		idVec3 cr = ( b - a ).Cross( d - a );
		nrm += cr;
		triAreaSum += 0.5 * ( double )cr.Length();
	}
	const float nl = nrm.Length();
	if( nl < 1e-6f )						// zero-area / fully degenerate
	{
		return 0;
	}
	nrm /= nl;
	const float diag = ( bmax - bmin ).Length();
	float maxOff = 0.0f;
	for( int i = 0; i < nv; i++ )
	{
		maxOff = Max( maxOff, idMath::Fabs( ( wv[i] - c ) * nrm ) );
	}
	if( maxOff > 0.01f * Max( diag, 1.0f ) )	// not a flat sheet (thin slab / 3-D body): defer to Fubini/box path
	{
		return 0;
	}
	// in-plane 2-D basis + projection
	idVec3 u = ( idMath::Fabs( nrm.z ) > 0.9f ) ? idVec3( 1.0f, 0.0f, 0.0f ) : idVec3( 0.0f, 0.0f, 1.0f );
	u = u - nrm * ( u * nrm );
	u.Normalize();
	const idVec3 vv = nrm.Cross( u );
	static idVec2 p2[8192];
	static int order[8192];
	for( int i = 0; i < nv; i++ )
	{
		p2[i].x = ( wv[i] - c ) * u;
		p2[i].y = ( wv[i] - c ) * vv;
		order[i] = i;
	}
	std::sort( order, order + nv, [&]( int a, int b )
	{
		return p2[a].x < p2[b].x || ( p2[a].x == p2[b].x && p2[a].y < p2[b].y );
	} );
	// Andrew's monotone chain -> CCW hull (indices)
	static int hull[16384];
	int hn = 0;
	for( int k = 0; k < nv; k++ )
	{
		const int i = order[k];
		while( hn >= 2 && R_SoftHull_Cross2( p2[hull[hn - 2]], p2[hull[hn - 1]], p2[i] ) <= 0.0f ) { hn--; }
		hull[hn++] = i;
	}
	const int lower = hn + 1;
	for( int k = nv - 2; k >= 0; k-- )
	{
		const int i = order[k];
		while( hn >= lower && R_SoftHull_Cross2( p2[hull[hn - 2]], p2[hull[hn - 1]], p2[i] ) <= 0.0f ) { hn--; }
		hull[hn++] = i;
	}
	hn--;									// last vertex == first
	if( hn < 3 )
	{
		return 0;							// degenerate projection: not a polygon (non-coplanar handled above)
	}
	outHullVerts = hn;						// hull vertex count regardless of the shader's 8-vert fill cap
	// convexity CERTIFICATE: filled area (tri sum) must equal the hull area, else concave / holed / two-sided
	double hullArea = 0.0;
	for( int i = 0; i < hn; i++ )
	{
		const idVec2& A = p2[hull[i]];
		const idVec2& B = p2[hull[( i + 1 ) % hn]];
		hullArea += ( double )A.x * B.y - ( double )B.x * A.y;
	}
	hullArea = 0.5 * ( hullArea < 0.0 ? -hullArea : hullArea );
	outConvex = hullArea >= 1e-4 && ( hullArea - triAreaSum <= 0.01 * hullArea ) && ( triAreaSum - hullArea <= 0.01 * hullArea );
	if( !outConvex || hn > SW_HULL_MAX_VERTS )
	{
		return 0;							// concave (fill needs interval-union), or convex but past the FillPoly cap
	}
	for( int i = 0; i < hn; i++ )
	{
		outHull[i] = wv[hull[i]];
	}
	return hn;
}

// Per-caster exact method + (for FillHull) the hull verts. outN = hull vert count when FILLHULL_CONVEX.
// outHullVerts/outConvex carry the FULL classification (convex regardless of the 8-vert cap) for the census.
static swMethod_t R_SoftWedgeMethod( const srfTriangles_t* tri, const idRenderLightLocal* lightDef,
		const viewLight_t* vLight, const float* modelToWorld, idVec3* outHull, int& outN, int& outHullVerts, bool& outConvex )
{
	outN = 0;
	outHullVerts = 0;
	outConvex = false;
	bool man, pl, sm;
	if( !R_SoftWedgeEligible( tri, lightDef, vLight, modelToWorld, man, pl, sm ) )
	{
		return SW_M_FUBINI;					// residual (non-manifold, or deep+large-light)
	}
	const int n = R_SoftWedgeConvexHull( tri, modelToWorld, outHull, outHullVerts, outConvex );
	if( n >= 3 )
	{
		outN = n;
		return SW_M_FILLHULL_CONVEX;
	}
	return SW_M_CONCAVE_INTERVAL;			// eligible but concave/>8v/non-coplanar: reserved -> Fubini today
}

// selector active? gated on the STEP-1 verification config: SW_SCANLINE grid (FillHull lives there) on, tile
// binner off (step 2 wires the binner). FillHull records ride the existing softIsBox analytic path.
static bool R_SoftWedgeSelectorActive()
{
	extern idCVar r_softShadowWedgeSelector, r_softShadowTileBin, r_softShadowScanline;
	return r_softShadowWedgeSelector.GetBool() && !r_softShadowTileBin.GetBool() && r_softShadowScanline.GetBool();
}

// ---- one-shot eligibility + method census (r_softShadowWedgeCensus), reported at frame-probe end ----
struct softWedgeCensus_t
{
	long casters, tris;					// all soft casters
	long convCasters, convTris, convHullVerts;	// FILLHULL-CONVEX NOW: convex + coplanar + <=8 hull verts (the O(verts) win)
	long cbigCasters, cbigTris;			// CONVEX-but->8v: passes the convexity certificate but exceeds the 8-vert fill cap
	long cbigHist[4];					// hull-vert histogram of the >8v-convex sub-bucket: [<=12, <=16, <=24, >24]
	long cpcCasters, cpcTris;			// COPLANAR-CONCAVE: fails convex cert but IS coplanar -> FillPolyConcave (step 1.5)
	long cpcSilHist[5];					// its outline size = tri->numSilEdges: [<=8, <=12, <=16, <=24, >24] (sizes SW_POLY_MAX_VERTS)
	long ncvCasters, ncvTris;			// NON-COPLANAR concave (3-D parallax): stays Fubini for now
	long resCasters, resTris;			// SW_M_FUBINI residual: non-eligible - permanently irreducible
};
static softWedgeCensus_t	s_wc;
static softWedgeCensus_t	s_wcLast;
static int					s_wcFrame = -1;
long g_swWedgeRoutedTris = 0, g_swWedgeRoutedHullVerts = 0;	// exact ROUTED collapse (selector on), cumulative

static void R_SoftWedgeCensusAccum( const srfTriangles_t* tri, const idRenderLightLocal* lightDef,
		const viewLight_t* vLight, const float* modelToWorld )
{
	const long tris = tri->numIndexes / 3;
	s_wc.casters++;
	s_wc.tris += tris;
	idVec3 hull[SW_HULL_MAX_VERTS];
	int hn = 0, hvAll = 0;
	bool cvx = false;
	switch( R_SoftWedgeMethod( tri, lightDef, vLight, modelToWorld, hull, hn, hvAll, cvx ) )
	{
		case SW_M_FILLHULL_CONVEX:
			s_wc.convCasters++; s_wc.convTris += tris; s_wc.convHullVerts += hn; break;
		case SW_M_CONCAVE_INTERVAL:
			// split the "concave-manifold" bucket: CONVEX-but->8v (raise the shader cap) vs TRULY-CONCAVE (interval fill)
			if( cvx )
			{
				s_wc.cbigCasters++; s_wc.cbigTris += tris;
				const int b = ( hvAll <= 12 ) ? 0 : ( hvAll <= 16 ) ? 1 : ( hvAll <= 24 ) ? 2 : 3;
				s_wc.cbigHist[b]++;
			}
			else if( hvAll > 0 )		// hull was computed => COPLANAR but concave -> FillPolyConcave candidate
			{
				s_wc.cpcCasters++; s_wc.cpcTris += tris;
				const int se = tri->numSilEdges;	// coplanar caster: silEdges == boundary outline size
				const int b = ( se <= 8 ) ? 0 : ( se <= 12 ) ? 1 : ( se <= 16 ) ? 2 : ( se <= 24 ) ? 3 : 4;
				s_wc.cpcSilHist[b]++;
			}
			else						// non-coplanar concave (3-D): needs true silhouette, stays Fubini
			{
				s_wc.ncvCasters++; s_wc.ncvTris += tris;
			}
			break;
		default:
			s_wc.resCasters++; s_wc.resTris += tris; break;
	}
}

void R_SoftWedgeCensusReport()
{
	extern idCVar r_softShadowWedgeSinA;
	const softWedgeCensus_t& w = s_wcLast.casters > 0 ? s_wcLast : s_wc;
	if( w.casters <= 0 )
	{
		common->Printf( "[wedgecensus] no soft casters collected (soft path inert at this view?)\n" );
		return;
	}
	const double t = Max( 1L, w.tris );
	common->Printf( "[wedgecensus] sinA<=%.3f | %ld soft casters (%ld tris cost). By exact method:\n", r_softShadowWedgeSinA.GetFloat(), w.casters, w.tris );
	common->Printf( "[wedgecensus]   FILLHULL-CONVEX (now):    %ld casters, %ld tris = %.0f%% of cost -> %ld hull verts (%.1fx tri->vert collapse)\n",
					w.convCasters, w.convTris, 100.0 * w.convTris / t, w.convHullVerts, w.convHullVerts > 0 ? ( double )w.convTris / ( double )w.convHullVerts : 0.0 );
	common->Printf( "[wedgecensus]   CONVEX->8v (raise cap):    %ld casters, %ld tris = %.0f%% of cost | hull-vert hist [<=12:%ld <=16:%ld <=24:%ld >24:%ld]\n",
					w.cbigCasters, w.cbigTris, 100.0 * w.cbigTris / t, w.cbigHist[0], w.cbigHist[1], w.cbigHist[2], w.cbigHist[3] );
	common->Printf( "[wedgecensus]   COPLANAR-CONCAVE (FillPolyConcave): %ld casters, %ld tris = %.0f%% of cost | outline(silEdge) hist [<=8:%ld <=12:%ld <=16:%ld <=24:%ld >24:%ld]\n",
					w.cpcCasters, w.cpcTris, 100.0 * w.cpcTris / t, w.cpcSilHist[0], w.cpcSilHist[1], w.cpcSilHist[2], w.cpcSilHist[3], w.cpcSilHist[4] );
	common->Printf( "[wedgecensus]   NON-COPLANAR concave (Fubini):  %ld casters, %ld tris = %.0f%% of cost (3-D parallax outline)\n",
					w.ncvCasters, w.ncvTris, 100.0 * w.ncvTris / t );
	common->Printf( "[wedgecensus]   RESIDUAL-FUBINI (permanent): %ld casters, %ld tris = %.0f%% of cost (non-manifold / deep+large-light)\n",
					w.resCasters, w.resTris, 100.0 * w.resTris / t );
	if( g_swWedgeRoutedTris > 0 )
	{
		common->Printf( "[wedgecensus]   ROUTED (selector on): %ld tris -> %ld hull verts = %.1fx O(tris)->O(verts) collapse\n",
						g_swWedgeRoutedTris, g_swWedgeRoutedHullVerts, g_swWedgeRoutedHullVerts > 0 ? ( double )g_swWedgeRoutedTris / ( double )g_swWedgeRoutedHullVerts : 0.0 );
	}
}

// A soft caster is CACHEABLE-STATIC iff its light is immobile AND its geometry is immobile (static world
// surface or DM_STATIC entity) AND the entity was not updated this frame. Mirrors the stencil path's
// static-interaction gate (IsDynamicModel()==DM_STATIC) + event-driven move invalidation (lastModifiedFrameNum).
// There is no entityHasMoved field; lastModifiedFrameNum vs the frame count is the move proxy.
static bool R_SoftCasterIsStatic( const idRenderEntityLocal* entityDef, const viewLight_t* vLight )
{
	if( vLight == NULL )
	{
		return false;
	}
	const idRenderModel* m = ( entityDef != NULL ) ? entityDef->parms.hModel : NULL;
	if( m == NULL )
	{
		return false;
	}
	// CURATED ANALYTIC BOX (r_softShadowProxyModel): force this model onto the DYNAMIC path so its box
	// caster is (re)emitted + tagged by the frontend flatten every frame, never routed through the warm
	// static cache (which has a separate flatten that would not carry the numTris<0 box tag). Cheap - one
	// analytic caster - and keeps the v1 box wiring confined to the single dynamic flatten site.
	{
		extern idCVar r_softShadowProxyModel;
		const char* pm = r_softShadowProxyModel.GetString();
		if( pm != NULL && pm[0] != '\0' && idStr::Icmp( m->Name(), pm ) == 0 )
		{
			return false;
		}
	}
	// SURF-CACHE STREAM ALIGNMENT (audit #6): with the world-texel cache on, the VIEW's static
	// prefix must equal the WARM collector's acceptance set EXACTLY (world model + bench soup,
	// R_BuildLightStaticSoftStream) - a served fragment SKIPS the whole static prefix on the
	// assumption the cache folded it, so a DM_STATIC prop classified static here but never warmed
	// (SWD_I_NOTSTREAM) silently lost its shadow on every cache hit, and a light whose only
	// casters were such props warmed nothing while the serve gate still engaged (100% misses).
	// Route those props onto the DYNAMIC suffix instead: walked per fragment on hits, so their
	// shadows stay exact (just uncached). Placed BEFORE the linger clause so linger cannot
	// re-admit a model the warm set will never fold. Cache off: byte-identical to the historical
	// classification below.
	{
		extern idCVar r_softShadowSurfCache;
		if( r_softShadowSurfCache.GetBool() && !m->IsStaticWorldModel()
				&& idStr::Cmpn( m->Name(), "_softBenchCaster_", 17 ) != 0 )
		{
			return false;
		}
	}
	// BENCH REPLAY soup: classifies STATIC (DM_STATIC, never updated) - and since the task-#87
	// stream alignment the WARM collectors ingest _softBenchCaster_* too (and honor
	// r_softShadowBenchExcludeWorld), so the warm stream and the view's static prefix agree: a
	// cache HIT serves the same geometry the view walks. (An interim fix forced the soup DYNAMIC,
	// which zeroed every bench light's static count and disabled serving outright - the serve
	// precondition is softStaticCasterCount > 0.)
	// LINGER classification (r_softShadowContribLinger > 0): a light+entity pair unmodified for
	// >= linger frames is cacheable regardless of movement history - the sticky lightHasMoved and
	// the static-model-only rule capped the cacheable share at ~29% of the live stream while most
	// of it is geometrically still. lastModifiedFrameNum ticks on every UpdateLightDef/
	// UpdateEntityDef, so anything animating/moving reclassifies the moment it changes; the
	// fingerprint/generation machinery then re-records (bounded, exact - just slower while moving).
	// DM_CONTINUOUS models (beams/particles: view-dependent geometry with no entity update) can
	// never linger-qualify.
	extern idCVar r_softShadowContribLinger;
	const int swLinger = r_softShadowContribLinger.GetInteger();
	if( swLinger > 0 )
	{
		if( m->IsDynamicModel() == DM_CONTINUOUS )
		{
			return false;
		}
		if( vLight->lightDef == NULL || tr.frameCount - vLight->lightDef->lastModifiedFrameNum < swLinger )
		{
			return false;
		}
		return tr.frameCount - entityDef->lastModifiedFrameNum >= swLinger;
	}
	if( vLight->lightHasMoved )
	{
		return false;
	}
	if( !m->IsStaticWorldModel() && m->IsDynamicModel() != DM_STATIC )
	{
		return false;
	}
	return entityDef->lastModifiedFrameNum != tr.frameCount;	// not moved/updated this frame
}

// ============================ same-light umbra-accumulation cull (r_softShadowUmbraAccum) ===================
// Per LIGHT (never across lights), walk this light's caster triangles in depth order from the light; kept
// geometry accumulates; a triangle whose full soft shadow falls inside the shadow of already-kept geometry
// contributes nothing to the union coverage and is NOT emitted into the walk stream. The composite test is
// the one VALIDATED offline (tests/SoftShadowProxyFit_test.cpp @study:SoftShadowUmbraAccum, cap0062/63/
// 64: 31-35% of records culled, zero supra-quantum false positives - every residual over-cull is below the
// walk's own 1/16-disk resolution):
//   1. coplanar-convex merge (signed planes) -> convex hull polygon certificates,
//   2. exact umbra certificate per kept polygon (plane + edge planes tangent to the light sphere; convex
//      region, so vertex containment is exact),
//   3. union containment by recursive midpoint subdivision,
//   4. aggregate sampled integral vs kept-so-far geometry with DILATED corner probes,
//   5. two depth-ordered passes; a culled unit's triangles and certificate retire immediately.
// Results are CACHED per light keyed on the participating surfaces' content hashes; surfaces whose stream
// changed since the previous frame (dynamics) are VOLATILE - excluded from the cull entirely (kept, and
// not used as blockers), so a moving caster can neither be wrongly culled nor wrongly cull others.
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <functional>

struct swUmbraCullSurf_t
{
	uint64_t		key;		// content hash (identity across frames)
	const idVec4*	tris;		// world-space tri stream, 3 float4 per triangle
	int				numTris;
};

// content hash: FNV over the count and the first/last triangle records - moves/deforms change it
static uint64_t SwUmbraSurfKey( const idVec4* tris, int numTris )
{
	uint64_t h = 1469598103934665603ull;
	auto mix = [&]( const void* p, size_t n )
	{
		const unsigned char* b = ( const unsigned char* )p;
		for( size_t i = 0; i < n; i++ ) { h = ( h ^ b[i] ) * 1099511628211ull; }
	};
	mix( &numTris, sizeof( numTris ) );
	if( numTris > 0 )
	{
		mix( &tris[0], sizeof( idVec4 ) * 3 );
		mix( &tris[( numTris - 1 ) * 3], sizeof( idVec4 ) * 3 );
		if( numTris > 2 ) { mix( &tris[( numTris / 2 ) * 3], sizeof( idVec4 ) * 3 ); }
	}
	return h;
}

// masks[surfKey][tri] = 1 -> cull. Constants mirror the validated study exactly.
static void SwUmbraCullCompute( const std::vector<swUmbraCullSurf_t>& surfs, const idVec3& Lp, float swR,
								std::unordered_map<uint64_t, std::vector<unsigned char>>& outMasks )
{
	struct Tri { idVec3 v[3]; int owner; float depth; };
	std::vector<Tri> tris;
	std::vector<int> surfBase( surfs.size() );
	for( size_t o = 0; o < surfs.size(); o++ )
	{
		surfBase[o] = ( int )tris.size();
		for( int t = 0; t < surfs[o].numTris; t++ )
		{
			Tri T;
			for( int k = 0; k < 3; k++ ) { const idVec4& r = surfs[o].tris[t * 3 + k]; T.v[k].Set( r.x, r.y, r.z ); }
			T.owner = ( int )o;
			T.depth = Min( ( T.v[0] - Lp ).Length(), Min( ( T.v[1] - Lp ).Length(), ( T.v[2] - Lp ).Length() ) );
			tris.push_back( T );
		}
	}
	if( tris.empty() ) { return; }

	struct Cert
	{
		idVec3 nt, V0;
		idVec3 en[12], ea[12]; int ne;
		idVec3 axis; float cosH, dMin;
	};
	// certificate w.r.t. an ARBITRARY light ball (LC, LR): LC=Lp/LR=swR is the classic full-light
	// certificate; a CELL of the light-sphere ball grid gives the per-cell certificate the conservative
	// aggregate composes (different blockers per cell = multi-blocker union, no sampling anywhere).
	auto makePolyCertAt = [&]( const idVec3* pv, int n, const idVec3& LC, float LR, Cert& out ) -> bool
	{
		if( n < 3 || n > 12 ) { return false; }
		idVec3 nt = ( pv[1] - pv[0] ).Cross( pv[2] - pv[0] );
		float ntl = nt.Length(); if( ntl <= 1e-6f ) { return false; }
		nt *= 1.0f / ntl;
		float dL = nt * ( LC - pv[0] ); if( dL < 0.0f ) { nt = -nt; dL = -dL; }
		if( dL <= LR + 1e-3f ) { return false; }
		idVec3 cen( 0, 0, 0 ); for( int k = 0; k < n; k++ ) { cen += pv[k]; } cen *= 1.0f / n;
		for( int e = 0; e < n; e++ )
		{
			idVec3 Va = pv[e], Vb = pv[( e + 1 ) % n];
			idVec3 ed = Vb - Va; float el2 = ed * ed; if( el2 < 1e-12f ) { return false; }
			idVec3 u2 = ed * ( 1.0f / idMath::Sqrt( el2 ) );
			idVec3 w = LC - Va; idVec3 wp = w - u2 * ( w * u2 ); float W2 = wp * wp;
			if( W2 <= LR * LR + 1e-6f ) { return false; }
			float invW = 1.0f / idMath::Sqrt( W2 ); idVec3 n0 = wp * invW; idVec3 m = u2.Cross( n0 );
			float sinT = LR * invW, cosT = idMath::Sqrt( Max( 1.0f - sinT * sinT, 0.0f ) );
			float sigma = ( ( m * ( cen - Va ) ) >= 0.0f ) ? 1.0f : -1.0f;
			out.en[e] = n0 * sinT + m * ( sigma * cosT );
			out.ea[e] = Va;
		}
		out.ne = n; out.nt = nt; out.V0 = pv[0];
		idVec3 ax = cen - LC; float axl = ax.Length(); if( axl < 1e-4f ) { return false; }
		out.axis = ax * ( 1.0f / axl );
		out.cosH = 1.0f;
		out.dMin = dL;			// nearest possible umbra point = perpendicular ball-to-plane distance
		for( int k = 0; k < n; k++ )
		{
			idVec3 dv = pv[k] - LC; float dl = dv.Length();
			if( dl > 1e-4f ) { out.cosH = Min( out.cosH, ( dv * ( 1.0f / dl ) ) * out.axis ); }
		}
		out.cosH -= 1e-3f;
		return true;
	};
	auto makePolyCert = [&]( const idVec3* pv, int n, Cert& out ) -> bool
	{
		return makePolyCertAt( pv, n, Lp, swR, out );
	};
	auto inCert = [&]( const Cert& C, const idVec3& v ) -> bool
	{
		float distPL = ( Lp - v ).Length();
		if( ( C.nt * ( v - C.V0 ) ) >= -2e-4f * distPL ) { return false; }
		for( int e = 0; e < C.ne; e++ ) { if( ( C.en[e] * ( v - C.ea[e] ) ) < 0.0f ) { return false; } }
		return true;
	};

	// emission units: coplanar-convex hull merges (signed plane key - opposite-facing tris never merge)
	struct Unit { std::vector<int> members; std::vector<idVec3> poly; float depth; };
	std::vector<Unit> units;
	{
		std::unordered_map<uint64_t, std::vector<int>> groups;
		for( size_t i = 0; i < tris.size(); i++ )
		{
			const Tri& T = tris[i];
			idVec3 n = ( T.v[1] - T.v[0] ).Cross( T.v[2] - T.v[0] );
			float nl = n.Length();
			if( nl < 1e-9f ) { Unit u; u.members = { ( int )i }; u.poly = { T.v[0], T.v[1], T.v[2] }; u.depth = T.depth; units.push_back( u ); continue; }
			n *= 1.0f / nl;
			float d = n * T.v[0];
			uint64_t key = ( ( uint64_t )T.owner << 40 )
						   ^ ( ( uint64_t )( int64_t )idMath::Rint( n.x * 512 ) & 0x3FF )
						   ^ ( ( ( uint64_t )( int64_t )idMath::Rint( n.y * 512 ) & 0x3FF ) << 10 )
						   ^ ( ( ( uint64_t )( int64_t )idMath::Rint( n.z * 512 ) & 0x3FF ) << 20 )
						   ^ ( ( ( uint64_t )( int64_t )idMath::Rint( d * 2.0f ) & 0xFFFF ) << 24 );
			groups[key].push_back( ( int )i );
		}
		for( auto& kv : groups )
		{
			std::vector<int>& g = kv.second;
			bool merged = false;
			if( g.size() >= 2 )
			{
				const Tri& T0 = tris[g[0]];
				idVec3 n = ( T0.v[1] - T0.v[0] ).Cross( T0.v[2] - T0.v[0] ); n.Normalize();
				idVec3 e0 = ( ( idMath::Fabs( n.z ) < 0.9f ) ? idVec3( 0, 0, 1 ) : idVec3( 1, 0, 0 ) ).Cross( n ); e0.Normalize();
				idVec3 e1 = n.Cross( e0 );
				// coincident duplicate tris (spawned capture casters over live geometry) double-count the
				// area sum, letting a NON-convex union pass the hull test -> unsound over-cull. Skip the
				// merge attempt when the group contains a duplicate (per-tri units stay sound).
				bool dup3 = false;
				{
					std::unordered_set<uint64_t> triKeys;
					for( int gi : g )
					{
						const Tri& T = tris[gi];
						uint64_t tk = 1469598103934665603ull;
						for( int k = 0; k < 3; k++ )
						{
							int64_t q[3] = { ( int64_t )idMath::Rint( T.v[k].x * 8.0f ), ( int64_t )idMath::Rint( T.v[k].y * 8.0f ), ( int64_t )idMath::Rint( T.v[k].z * 8.0f ) };
							tk = ( tk ^ ( uint64_t )( q[0] * 73856093 + q[1] * 19349663 + q[2] * 83492791 ) ) * 1099511628211ull;
						}
						if( !triKeys.insert( tk ).second ) { dup3 = true; break; }
					}
				}
				// TRUE PLANARITY: the quantized plane key admits ~0.5u of sag; a hull from sagging tris
				// certifies a FLATTENED polygon that does not physically exist (measured FP in the study's
				// cell certificates). Require every group vert within a tight absolute plane distance.
				for( size_t gi2 = 0; gi2 < g.size() && !dup3; gi2++ )
				{
					for( int k = 0; k < 3; k++ )
					{
						if( idMath::Fabs( n * ( tris[g[gi2]].v[k] - T0.v[0] ) ) > 0.03f ) { dup3 = true; break; }
					}
				}
				std::vector<std::pair<float, float>> p2; std::vector<idVec3> p3;
				double triArea = 0;
				if( dup3 ) { p2.clear(); }		// duplicates present: leave p2 empty so the hull attempt below rejects
				for( int gi : g )
				{
					if( dup3 ) { break; }
					const Tri& T = tris[gi];
					triArea += 0.5 * ( ( T.v[1] - T.v[0] ).Cross( T.v[2] - T.v[0] ) ).Length();
					for( int k = 0; k < 3; k++ )
					{
						float u = ( T.v[k] - T0.v[0] ) * e0, v = ( T.v[k] - T0.v[0] ) * e1;
						bool dup = false;
						for( auto& q : p2 ) { if( idMath::Fabs( q.first - u ) < 1e-3f && idMath::Fabs( q.second - v ) < 1e-3f ) { dup = true; break; } }
						if( !dup ) { p2.push_back( { u, v } ); p3.push_back( T.v[k] ); }
					}
				}
				if( p2.size() >= 3 && p2.size() <= 64 )
				{
					std::vector<int> idx( p2.size() );
					for( size_t k = 0; k < idx.size(); k++ ) { idx[k] = ( int )k; }
					std::sort( idx.begin(), idx.end(), [&]( int a, int b ) { return p2[a] < p2[b]; } );
					auto cr2 = [&]( int o, int a, int b ) { return ( double )( p2[a].first - p2[o].first ) * ( p2[b].second - p2[o].second ) - ( double )( p2[a].second - p2[o].second ) * ( p2[b].first - p2[o].first ); };
					std::vector<int> hull( 2 * idx.size() ); int hn = 0;
					for( size_t k = 0; k < idx.size(); k++ ) { while( hn >= 2 && cr2( hull[hn - 2], hull[hn - 1], idx[k] ) <= 0 ) { hn--; } hull[hn++] = idx[k]; }
					int lower = hn + 1;
					for( int k = ( int )idx.size() - 2; k >= 0; k-- ) { while( hn >= lower && cr2( hull[hn - 2], hull[hn - 1], idx[k] ) <= 0 ) { hn--; } hull[hn++] = idx[k]; }
					hn--;
					double hullArea = 0;
					for( int k = 0; k < hn; k++ ) { int a = hull[k], b = hull[( k + 1 ) % hn]; hullArea += 0.5 * ( ( double )p2[a].first * p2[b].second - ( double )p2[b].first * p2[a].second ); }
					hullArea = idMath::Fabs( ( float )hullArea );
					if( hn >= 3 && hn <= 12 && triArea >= hullArea * 0.999 )
					{
						Unit u; u.members = g;
						u.depth = 1e30f;
						for( int gi : g ) { u.depth = Min( u.depth, tris[gi].depth ); }
						for( int k = 0; k < hn; k++ ) { u.poly.push_back( p3[hull[k]] ); }
						units.push_back( u ); merged = true;
					}
				}
			}
			if( !merged )
			{
				for( int gi : g ) { Unit u; u.members = { gi }; u.poly = { tris[gi].v[0], tris[gi].v[1], tris[gi].v[2] }; u.depth = tris[gi].depth; units.push_back( u ); }
			}
		}
	}
	std::sort( units.begin(), units.end(), []( const Unit& a, const Unit& b ) { return a.depth < b.depth; } );

	std::vector<Cert> certs; certs.reserve( 1024 );
	std::vector<unsigned char> certAlive; certAlive.reserve( 1024 );
	std::vector<unsigned char> culled( tris.size(), 0 );
	std::vector<int> triCert( tris.size(), -1 );
	const int SUBDIV = 3;
	std::function<bool( const idVec3&, const idVec3&, const idVec3&, int )> inUnion =
		[&]( const idVec3& a, const idVec3& b, const idVec3& c, int depth ) -> bool
	{
		for( size_t ci = 0; ci < certs.size(); ci++ )
		{
			if( !certAlive[ci] ) { continue; }
			const Cert& C = certs[ci];
			bool maybe = true;
			const idVec3 vv[3] = { a, b, c };
			for( int k = 0; k < 3 && maybe; k++ )
			{
				idVec3 dv = vv[k] - Lp; float dl = dv.Length();
				if( dl <= C.dMin || ( dv * ( 1.0f / dl ) ) * C.axis < C.cosH ) { maybe = false; }
			}
			if( !maybe ) { continue; }
			if( inCert( C, a ) && inCert( C, b ) && inCert( C, c ) ) { return true; }
		}
		if( depth <= 0 ) { return false; }
		idVec3 ab = ( a + b ) * 0.5f, bc = ( b + c ) * 0.5f, ca = ( c + a ) * 0.5f;
		return inUnion( a, ab, ca, depth - 1 ) && inUnion( ab, b, bc, depth - 1 )
			   && inUnion( ca, bc, c, depth - 1 ) && inUnion( ab, bc, ca, depth - 1 );
	};

	// aggregate fallback: kept-so-far blockers per surf (bounding sphere precull), Moller-Trumbore segments
	struct BSph { idVec3 c; float r; };
	std::vector<BSph> bs( surfs.size() );
	for( size_t o = 0; o < surfs.size(); o++ )
	{
		idVec3 mn( 1e30f, 1e30f, 1e30f ), mx( -1e30f, -1e30f, -1e30f );
		for( int t = 0; t < surfs[o].numTris * 3; t++ )
		{
			const idVec4& r = surfs[o].tris[t];
			mn.x = Min( mn.x, r.x ); mn.y = Min( mn.y, r.y ); mn.z = Min( mn.z, r.z );
			mx.x = Max( mx.x, r.x ); mx.y = Max( mx.y, r.y ); mx.z = Max( mx.z, r.z );
		}
		bs[o].c = ( mn + mx ) * 0.5f; bs[o].r = ( mx - mn ).Length() * 0.5f + 1e-2f;
	}
	std::vector<std::vector<int>> keptOf( surfs.size() );
	auto rayBlockedByKept = [&]( const idVec3& P, const idVec3& tgt ) -> bool
	{
		idVec3 seg = tgt - P; float segL = seg.Length(); if( segL < 1e-4f ) { return true; }
		idVec3 dir = seg * ( 1.0f / segL );
		for( size_t b = 0; b < surfs.size(); b++ )
		{
			if( keptOf[b].empty() ) { continue; }
			idVec3 oc = bs[b].c - P; float tp = oc * dir;
			if( tp < -bs[b].r || tp > segL + bs[b].r ) { continue; }
			float tc = Max( 0.0f, Min( tp, segL ) );
			idVec3 q = P + dir * tc - bs[b].c;
			if( q * q > bs[b].r * bs[b].r ) { continue; }
			for( int qi : keptOf[b] )
			{
				if( culled[qi] ) { continue; }
				const Tri& K = tris[qi];
				idVec3 e1 = K.v[1] - K.v[0], e2 = K.v[2] - K.v[0], pv = seg.Cross( e2 );
				float det = e1 * pv; if( idMath::Fabs( det ) < 1e-12f ) { continue; }
				float inv = 1.0f / det; idVec3 tv = P - K.v[0];
				float u = ( tv * pv ) * inv; if( u < -1e-6f || u > 1.0f + 1e-6f ) { continue; }
				idVec3 qv = tv.Cross( e1 );
				float v = ( seg * qv ) * inv; if( v < -1e-6f || u + v > 1.0f + 1e-6f ) { continue; }
				float t = ( e2 * qv ) * inv;
				if( t > 1e-5f && t < 1.0f - 1e-5f ) { return true; }
			}
		}
		return false;
	};
	auto triFullyShadowedByKept = [&]( const Tri& T ) -> bool
	{
		static const float BC[26][3] = { {1.f/3,1.f/3,1.f/3}, {0.6f,0.2f,0.2f}, {0.2f,0.6f,0.2f}, {0.2f,0.2f,0.6f}, {0.45f,0.45f,0.1f}, {0.1f,0.45f,0.45f},
										 {0.9f,0.05f,0.05f}, {0.05f,0.9f,0.05f}, {0.05f,0.05f,0.9f}, {0.475f,0.475f,0.05f}, {0.05f,0.475f,0.475f}, {0.475f,0.05f,0.475f},
										 {0.96f,0.02f,0.02f}, {0.02f,0.96f,0.02f}, {0.02f,0.02f,0.96f}, {0.25f,0.5f,0.25f},
										 {1.12f,-0.06f,-0.06f}, {-0.06f,1.12f,-0.06f}, {-0.06f,-0.06f,1.12f}, {0.56f,0.56f,-0.12f},
										 {1.3f,-0.15f,-0.15f}, {-0.15f,1.3f,-0.15f}, {-0.15f,-0.15f,1.3f},
										 {-0.12f,0.56f,0.56f}, {0.56f,-0.12f,0.56f}, {0.65f,0.65f,-0.3f} };
		const int KD = 32;
		for( int s = 0; s < 26; s++ )
		{
			idVec3 P = T.v[0] * BC[s][0] + T.v[1] * BC[s][1] + T.v[2] * BC[s][2];
			idVec3 toL = Lp - P; float dL = toL.Length(); if( dL < 1e-3f ) { return false; }
			idVec3 nrm = toL * ( 1.0f / dL );
			idVec3 u2 = ( ( idMath::Fabs( nrm.z ) < 0.9f ) ? idVec3( 0, 0, 1 ) : idVec3( 1, 0, 0 ) ).Cross( nrm ); u2.Normalize();
			idVec3 v2 = nrm.Cross( u2 );
			for( int k = 0; k < KD; k++ )
			{
				float a = ( float )( 2.0 * 3.14159265358979 * k / KD );
				float rr = swR * idMath::Sqrt( ( k + 0.5f ) / KD );
				if( !rayBlockedByKept( P, Lp + u2 * ( rr * idMath::Cos( a ) ) + v2 * ( rr * idMath::Sin( a ) ) ) ) { return false; }
			}
		}
		return true;
	};

	const int PASSES = 2;
	// CONSERVATIVE CELL AGGREGATE: cover the light SPHERE with a ball grid; a unit is cullable iff every
	// cell has SOME kept unit whose certificate w.r.t. that cell-ball contains all the unit's verts.
	// Different blockers per cell = the multi-blocker union, conservative in the light dimension (ball
	// inflation) and EXACT in the surface dimension (convexity + vertex containment) - no sampling
	// anywhere; the study audits it at ZERO unblocked rays across all grid resolutions.
	std::vector<idVec3> cellC; float cellRad = 0.0f;
	{
		const int G = 8;
		const float step = 2.0f * swR / G;
		cellRad = 0.5f * step * 1.7320508f;
		for( int i = 0; i < G; i++ )
			for( int j = 0; j < G; j++ )
				for( int k = 0; k < G; k++ )
				{
					idVec3 off( -swR + ( i + 0.5f ) * step, -swR + ( j + 0.5f ) * step, -swR + ( k + 0.5f ) * step );
					if( off.Length() <= swR + cellRad ) { cellC.push_back( Lp + off ); }
				}
	}
	std::vector<int> keptUnitIdx; std::vector<unsigned char> keptUnitAlive;
	std::vector<int> unitKeptPos( units.size(), -1 );
	std::unordered_map<uint64_t, Cert> cellCertMemo;
	std::unordered_set<uint64_t> cellCertBad;
	std::vector<int> cellLastHit( cellC.size(), -1 );
	auto cellCovered = [&]( const Unit& U, int ci ) -> bool
	{
		auto tryW = [&]( int w ) -> bool
		{
			if( w < 0 || !keptUnitAlive[w] ) { return false; }
			const uint64_t key = ( ( uint64_t )w << 24 ) | ( uint64_t )ci;
			if( cellCertBad.count( key ) ) { return false; }
			auto it = cellCertMemo.find( key );
			if( it == cellCertMemo.end() )
			{
				const Unit& W = units[keptUnitIdx[w]];
				Cert tmp;
				if( !makePolyCertAt( W.poly.data(), ( int )W.poly.size(), cellC[ci], cellRad, tmp ) )
				{
					cellCertBad.insert( key ); return false;
				}
				it = cellCertMemo.emplace( key, tmp ).first;
			}
			const Cert& C = it->second;
			for( size_t k = 0; k < U.poly.size(); k++ ) { if( !inCert( C, U.poly[k] ) ) { return false; } }
			return true;
		};
		if( tryW( cellLastHit[ci] ) ) { return true; }
		for( int w = ( int )keptUnitIdx.size() - 1; w >= 0; w-- )
		{
			if( w == cellLastHit[ci] ) { continue; }
			if( tryW( w ) ) { cellLastHit[ci] = w; return true; }
		}
		return false;
	};

	for( int pass = 0; pass < PASSES; pass++ )
	{
		for( size_t ui = 0; ui < units.size(); ui++ )
		{
			const Unit& U = units[ui];
			if( culled[U.members[0]] ) { continue; }
			bool cull = false;
			if( U.poly.size() == 3 )
			{
				cull = inUnion( U.poly[0], U.poly[1], U.poly[2], SUBDIV );
			}
			else
			{
				cull = true;
				for( size_t k = 1; k + 1 < U.poly.size() && cull; k++ ) { cull = inUnion( U.poly[0], U.poly[k], U.poly[k + 1], SUBDIV ); }
			}
			// AGGRESSIVE stage: the conservative cell aggregate (multi-blocker union). Sound by
			// construction (no sampling); the legacy sampled integral it replaces was gate-red.
			extern idCVar r_softShadowUmbraAccumAggressive;
			if( !cull && r_softShadowUmbraAccumAggressive.GetBool() )
			{
				cull = true;
				for( size_t ci = 0; ci < cellC.size() && cull; ci++ ) { if( !cellCovered( U, ( int )ci ) ) { cull = false; } }
			}
			if( cull )
			{
				for( int m : U.members ) { culled[m] = 1; }
				if( triCert[U.members[0]] >= 0 ) { certAlive[triCert[U.members[0]]] = 0; }
				if( unitKeptPos[ui] >= 0 ) { keptUnitAlive[unitKeptPos[ui]] = 0; }
				continue;
			}
			if( pass == 0 )
			{
				Cert nc;
				if( makePolyCert( U.poly.data(), ( int )U.poly.size(), nc ) )
				{
					certs.push_back( nc ); certAlive.push_back( 1 );
					for( int m : U.members ) { triCert[m] = ( int )certs.size() - 1; }
				}
				unitKeptPos[ui] = ( int )keptUnitIdx.size();
				keptUnitIdx.push_back( ( int )ui ); keptUnitAlive.push_back( 1 );
				for( int m : U.members ) { keptOf[tris[m].owner].push_back( m ); }
			}
		}
	}

	for( size_t o = 0; o < surfs.size(); o++ )
	{
		std::vector<unsigned char> mask( surfs[o].numTris, 0 );
		bool any = false;
		for( int t = 0; t < surfs[o].numTris; t++ ) { if( culled[surfBase[o] + t] ) { mask[t] = 1; any = true; } }
		if( any ) { outMasks[surfs[o].key] = std::move( mask ); }
	}
}

// per-light cull cache: comboKey over the STABLE surfaces; masks by surface content hash. Volatile
// surfaces (content changed since last frame) never participate.
struct swUmbraLightCache_t
{
	uint64_t comboKey = 0;
	std::unordered_map<uint64_t, std::vector<unsigned char>> masks;
	std::unordered_set<uint64_t> prevKeys, curKeys;
};
static std::unordered_map<int, swUmbraLightCache_t> s_swUmbraCache;

// CAMERA-INVARIANT static-caster cache (r_softShadowSurfCache). The surf-fold cache stores residual
// tri indices POSITIONALLY into the emitted static-prefix stream, so the prefix must be byte-stable
// across frames or the residual lists point at the wrong triangles and the whole GPU cache is wiped
// wholesale every frame (the standing-still degradation). The view only collects the statics inside
// the current frustum/PVS, so idle camera sway alone churns the world-surface set at portal/screen
// edges. Fix: the FIRST time the view collects a static caster surface we COPY its world-space wedge
// stream here (frame-alloc'd softEdges do not survive the frame), keyed by a stable identity; every
// frame we re-emit the WHOLE cached set in key-sorted order, so the prefix is deterministic and the
// fingerprint only flips when the set genuinely grows/shrinks. Entries unseen for a while are evicted
// to bound growth across a level (an eviction is one clear, then stable again).
struct swStaticCaster_t
{
	uint64_t key = 0;					// (entityIndex<<32) ^ quantized-first-vertex - stable per surface
	int      entityIndex = -1;
	bool     isWorld = false;
	idRenderEntityLocal* entityDef = NULL;	// persistent renderWorld entity (never frame-alloc)
	int      lastSeenFrame = 0;
	std::vector<idVec4> tris;			// world-space triangle stream (numTris*3 float4)
	std::vector<idVec4> clusters;		// cluster table (numClusters*2 float4), surface-local firstTri
};
struct swStaticLightCache_t
{
	std::unordered_map<uint64_t, swStaticCaster_t> casters;
};
static std::unordered_map<int, swStaticLightCache_t> s_swStaticCasterCache;

// CONTRIBUTION SCORE (r_softShadowDepthOrder 2): angular size of the caster as seen from the
// light = boundRadius / distance. A wider cone cuts a wider shadow frustum and saturates more
// receivers' disks, so emitting casters in DESCENDING score order lets the walk's saturation
// early-outs fire before the low-score tail is ever visited. The bound is a stride-sampled vert
// min/max (cheap, deterministic); exactness is irrelevant - order is semantically free (the walk
// is a commutative union), only the early-out landing point moves.
static void R_SoftAccumSampleBounds( const idVec4* e, int n, idBounds& b )
{
	if( e == NULL || n <= 0 )
	{
		return;
	}
	const int stride = Max( 1, n / 8 );
	for( int i = 0; i < n; i += stride )
	{
		b.AddPoint( idVec3( e[i].x, e[i].y, e[i].z ) );
	}
}
static float R_SoftAngularScore( const idBounds& b, const idVec3& lightOrg )
{
	if( b.IsCleared() )
	{
		return 0.0f;	// empty stream: conservative-only junk, lands last
	}
	const idVec3 c = b.GetCenter();
	const float r = ( b[1] - b[0] ).Length() * 0.5f;
	return r / Max( ( c - lightOrg ).Length(), Max( r, 1e-3f ) );
}
// RB begin
idCVar r_forceShadowMapsOnAlphaTestedSurfaces( "r_forceShadowMapsOnAlphaTestedSurfaces", "1", CVAR_RENDERER | CVAR_BOOL, "0 = same shadowing as with stencil shadows, 1 = ignore noshadows for alpha tested materials" );
// RB end
// foresthale 2014-11-24: cvar to control the material lod flags - this is the distance at which a mesh switches from lod1 to lod2, where lod3 will appear at this distance *2, lod4 at *4, and persistentLOD keyword will disable the max distance check (thus extending this LOD to all further distances, rather than disappearing)
idCVar r_lodMaterialDistance( "r_lodMaterialDistance", "500", CVAR_RENDERER | CVAR_FLOAT, "surfaces further than this distance will use lower quality versions (if their material uses the lod1-4 keywords, persistentLOD disables the max distance checks)" );

static const float CHECK_BOUNDS_EPSILON = 1.0f;

// Stencil shadow volumes: how far to stretch the near-clip expansion of the inside test.
// (In theory should vary with FOV.) Restored from DOOM-3-BFG jobs/ShadowShared.
static const float INSIDE_SHADOW_VOLUME_EXTRA_STRETCH = 4.0f;

/*
======================
R_ViewPotentiallyInsideInfiniteShadowVolume

If we know that we are "off to the side" of an infinite shadow volume, we can draw it without
caps in Z-pass mode - which avoids the projected-to-infinity far cap entirely. Only when the
view might be inside the volume do we need Z-fail with caps. Restored verbatim from the deleted
DOOM-3-BFG jobs/ShadowShared.cpp so the stencil pass can choose Z-pass at a distance (the common
case) instead of forcing Z-fail everywhere - forcing Z-fail draws the far cap for every caster,
and against distant scene depth that cap paints phantom shadows down long corridors.
======================
*/
static bool R_ViewPotentiallyInsideInfiniteShadowVolume( const idBounds& occluderBounds, const idVec3& localLight, const idVec3& localView, const float zNear )
{
	// Expand the bounds to account for the near clip plane, because the view could be
	// mathematically outside, but if the near clip plane chops a volume edge then the
	// Z-pass rendering would fail.
	const idBounds expandedBounds = occluderBounds.Expand( zNear );

	// If the view is inside the geometry bounding box then the view is also inside the shadow projection.
	if( expandedBounds.ContainsPoint( localView ) )
	{
		return true;
	}

	// If the light is inside the geometry bounding box then the shadow is projected in all
	// directions and any view position is inside the infinite shadow projection.
	if( expandedBounds.ContainsPoint( localLight ) )
	{
		return true;
	}

	// If the line from localLight to localView intersects the geometry bounding box then the
	// view is inside the infinite shadow projection.
	if( expandedBounds.LineIntersection( localLight, localView ) )
	{
		return true;
	}

	// The view is definitely not inside the projected shadow.
	return false;
}



/*
==================
R_ClearEntityDefDynamicModel

If we know the reference bounds stays the same, we
only need to do this on entity update, not the full
R_FreeEntityDefDerivedData
==================
*/
void R_ClearEntityDefDynamicModel( idRenderEntityLocal* def )
{
	// free all the interaction surfaces
	for( idInteraction* inter = def->firstInteraction; inter != NULL && !inter->IsEmpty(); inter = inter->entityNext )
	{
		inter->FreeSurfaces();
	}

	// clear the dynamic model if present
	if( def->dynamicModel )
	{
		// this is copied from cachedDynamicModel, so it doesn't need to be freed
		def->dynamicModel = NULL;
	}
	def->dynamicModelFrameCount = 0;
}

/*
==================
R_IssueEntityDefCallback
==================
*/
bool R_IssueEntityDefCallback( idRenderEntityLocal* def )
{
	idBounds oldBounds = def->localReferenceBounds;

	bool update;
	if( tr.viewDef != NULL )
	{
		update = def->parms.callback( &def->parms, &tr.viewDef->renderView );
	}
	else
	{
		update = def->parms.callback( &def->parms, NULL );
	}
	tr.pc.c_entityDefCallbacks++;

	if( def->parms.hModel == NULL )
	{
		common->Error( "R_IssueEntityDefCallback: dynamic entity callback didn't set model" );
	}

	if( r_checkBounds.GetBool() )
	{
		if(	oldBounds[0][0] > def->localReferenceBounds[0][0] + CHECK_BOUNDS_EPSILON ||
				oldBounds[0][1] > def->localReferenceBounds[0][1] + CHECK_BOUNDS_EPSILON ||
				oldBounds[0][2] > def->localReferenceBounds[0][2] + CHECK_BOUNDS_EPSILON ||
				oldBounds[1][0] < def->localReferenceBounds[1][0] - CHECK_BOUNDS_EPSILON ||
				oldBounds[1][1] < def->localReferenceBounds[1][1] - CHECK_BOUNDS_EPSILON ||
				oldBounds[1][2] < def->localReferenceBounds[1][2] - CHECK_BOUNDS_EPSILON )
		{
			common->Printf( "entity %i callback extended reference bounds\n", def->index );
		}
	}

	return update;
}

/*
===================
R_EntityDefDynamicModel

This is also called by the game code for idRenderWorldLocal::ModelTrace(), and idRenderWorldLocal::Trace() which is bad for performance...

Issues a deferred entity callback if necessary.
If the model isn't dynamic, it returns the original.
Returns the cached dynamic model if present, otherwise creates it.
===================
*/
idRenderModel* R_EntityDefDynamicModel( idRenderEntityLocal* def )
{
	if( def->dynamicModelFrameCount == tr.frameCount )
	{
		return def->dynamicModel;
	}

	// allow deferred entities to construct themselves
	bool callbackUpdate;
	if( def->parms.callback != NULL )
	{
		SCOPED_PROFILE_EVENT( "R_IssueEntityDefCallback" );
		callbackUpdate = R_IssueEntityDefCallback( def );
	}
	else
	{
		callbackUpdate = false;
	}

	idRenderModel* model = def->parms.hModel;

	if( model == NULL )
	{
		common->Error( "R_EntityDefDynamicModel: NULL model" );
		return NULL;
	}

	if( model->IsDynamicModel() == DM_STATIC )
	{
		def->dynamicModel = NULL;
		def->dynamicModelFrameCount = 0;
		return model;
	}

	// continously animating models (particle systems, etc) will have their snapshot updated every single view
	if( callbackUpdate || ( model->IsDynamicModel() == DM_CONTINUOUS && def->dynamicModelFrameCount != tr.frameCount ) )
	{
		R_ClearEntityDefDynamicModel( def );
	}

	// if we don't have a snapshot of the dynamic model, generate it now
	if( def->dynamicModel == NULL )
	{
		SCOPED_PROFILE_EVENT( "InstantiateDynamicModel" );

		// instantiate the snapshot of the dynamic model, possibly reusing memory from the cached snapshot
		def->cachedDynamicModel = model->InstantiateDynamicModel( &def->parms, tr.viewDef, def->cachedDynamicModel );

		if( def->cachedDynamicModel != NULL && r_checkBounds.GetBool() )
		{
			idBounds b = def->cachedDynamicModel->Bounds();
			if(	b[0][0] < def->localReferenceBounds[0][0] - CHECK_BOUNDS_EPSILON ||
					b[0][1] < def->localReferenceBounds[0][1] - CHECK_BOUNDS_EPSILON ||
					b[0][2] < def->localReferenceBounds[0][2] - CHECK_BOUNDS_EPSILON ||
					b[1][0] > def->localReferenceBounds[1][0] + CHECK_BOUNDS_EPSILON ||
					b[1][1] > def->localReferenceBounds[1][1] + CHECK_BOUNDS_EPSILON ||
					b[1][2] > def->localReferenceBounds[1][2] + CHECK_BOUNDS_EPSILON )
			{
				common->Printf( "entity %i dynamic model exceeded reference bounds\n", def->index );
			}
		}

		def->dynamicModel = def->cachedDynamicModel;
		def->dynamicModelFrameCount = tr.frameCount;
	}

	// set model depth hack value
	if( def->dynamicModel != NULL && model->DepthHack() != 0.0f && tr.viewDef != NULL )
	{
		idPlane eye, clip;
		idVec3 ndc;
		R_TransformModelToClip( def->parms.origin, tr.viewDef->worldSpace.modelViewMatrix, tr.viewDef->projectionMatrix, eye, clip );
		R_TransformClipToDevice( clip, ndc );
		def->parms.modelDepthHack = model->DepthHack() * ( 1.0f - ndc.z );
	}
	else
	{
		def->parms.modelDepthHack = 0.0f;
	}

	return def->dynamicModel;
}

/*
===================
R_SetupDrawSurfShader
===================
*/
void R_SetupDrawSurfShader( drawSurf_t* drawSurf, const idMaterial* shader, const renderEntity_t* renderEntity )
{
	drawSurf->material = shader;
	drawSurf->sort = shader->GetSort();

	// process the shader expressions for conditionals / color / texcoords
	const float*	constRegs = shader->ConstantRegisters();
	if( likely( constRegs != NULL ) )
	{
		// shader only uses constant values
		drawSurf->shaderRegisters = constRegs;
	}
	else
	{
		// by default evaluate with the entityDef's shader parms
		const float* shaderParms = renderEntity->shaderParms;

		// a reference shader will take the calculated stage color value from another shader
		// and use that for the parm0-parm3 of the current shader, which allows a stage of
		// a light model and light flares to pick up different flashing tables from
		// different light shaders
		float generatedShaderParms[MAX_ENTITY_SHADER_PARMS];
		if( unlikely( renderEntity->referenceShader != NULL ) )
		{
			// evaluate the reference shader to find our shader parms
			float refRegs[MAX_EXPRESSION_REGISTERS];
			renderEntity->referenceShader->EvaluateRegisters( refRegs, renderEntity->shaderParms,
					tr.viewDef->renderView.shaderParms,
					tr.viewDef->renderView.time[renderEntity->timeGroup] * 0.001f, renderEntity->referenceSound );

			const shaderStage_t* pStage = renderEntity->referenceShader->GetStage( 0 );

			memcpy( generatedShaderParms, renderEntity->shaderParms, sizeof( generatedShaderParms ) );
			generatedShaderParms[0] = refRegs[ pStage->color.registers[0] ];
			generatedShaderParms[1] = refRegs[ pStage->color.registers[1] ];
			generatedShaderParms[2] = refRegs[ pStage->color.registers[2] ];

			shaderParms = generatedShaderParms;
		}

		// allocate frame memory for the shader register values
		float* regs = ( float* )R_FrameAlloc( shader->GetNumRegisters() * sizeof( float ), FRAME_ALLOC_SHADER_REGISTER );
		drawSurf->shaderRegisters = regs;

		// process the shader expressions for conditionals / color / texcoords
		shader->EvaluateRegisters( regs, shaderParms, tr.viewDef->renderView.shaderParms,
								   tr.viewDef->renderView.time[renderEntity->timeGroup] * 0.001f, renderEntity->referenceSound );
	}
}

/*
===================
R_SetupDrawSurfJoints
===================
*/
void R_SetupDrawSurfJoints( drawSurf_t* drawSurf, const srfTriangles_t* tri, const idMaterial* shader, nvrhi::ICommandList* commandList )
{
	// RB: added check wether GPU skinning is available at all
	if( tri->staticModelWithJoints == NULL || !r_useGPUSkinning.GetBool() )
	{
		drawSurf->jointCache = 0;
		return;
	}
	// RB end

	idRenderModelStatic* model = tri->staticModelWithJoints;
	assert( model->jointsInverted != NULL );

	if( !vertexCache.CacheIsCurrent( model->jointsInvertedBuffer ) )
	{
		model->jointsInvertedBuffer = vertexCache.AllocJoint( model->jointsInverted, model->numInvertedJoints, sizeof( idJointMat ), commandList );
	}
	drawSurf->jointCache = model->jointsInvertedBuffer;
}

/*
===================
R_AddSingleModel

May be run in parallel.

Here is where dynamic models actually get instantiated, and necessary
interaction surfaces get created. This is all done on a sort-by-model
basis to keep source data in cache (most likely L2) as any interactions
and shadows are generated, since dynamic models will typically be lit by
two or more lights.
===================
*/
void R_AddSingleModel( viewEntity_t* vEntity )
{
	// we will add all interaction surfs here, to be chained to the lights in later serial code
	vEntity->drawSurfs = NULL;

	// globals we really should pass in...
	const viewDef_t* viewDef = tr.viewDef;

	idRenderEntityLocal* entityDef = vEntity->entityDef;
	const renderEntity_t* renderEntity = &entityDef->parms;
	const idRenderWorldLocal* world = entityDef->world;

	if( viewDef->isXraySubview && entityDef->parms.xrayIndex == 1 )
	{
		return;
	}
	else if( !viewDef->isXraySubview && entityDef->parms.xrayIndex == 2 )
	{
		return;
	}

	SCOPED_PROFILE_EVENT( renderEntity->hModel == NULL ? "Unknown Model" : renderEntity->hModel->Name() );

	// calculate the znear for testing whether or not the view is inside a shadow projection
	const float znear = ( viewDef->renderView.cramZNear ) ? ( r_znear.GetFloat() * 0.25f ) : r_znear.GetFloat();

	// if the entity wasn't seen through a portal chain, it was added just for light shadows
	const bool modelIsVisible = !vEntity->scissorRect.IsEmpty();
	const bool addInteractions = modelIsVisible && ( !viewDef->isXraySubview || entityDef->parms.xrayIndex == 2 );
	const int entityIndex = entityDef->index;

	// BRUSH-RECOVERY soft shadows (r_softShadowBrushHulls): each portal area's world model is entity
	// "_area%i" (AddWorldModelEntities). When that area carries dmap'd convex hulls, the worldspawn
	// soft-caster below emits one analytic hull record per hull INSTEAD of the area's triangle stream.
	// Resolve the area's hulls once here (name-derived area index; area world models are identity space).
	extern idCVar r_softShadowBrushHulls;
	const areaShadowHull_t* swAreaHulls = NULL;
	int swNumAreaHulls = 0;
	const areaResidualTris_t* swAreaResiduals = NULL;	// package B: the area's non-hull-emittable brushes, as triangles
	int swAreaIdx = -1;									// entity scope: shared by the hull + residual emit below
	if( r_softShadowBrushHulls.GetBool() && entityDef->parms.hModel != NULL && entityDef->parms.hModel->IsStaticWorldModel() )
	{
		if( sscanf( entityDef->parms.hModel->Name(), "_area%d", &swAreaIdx ) == 1 && swAreaIdx >= 0 )
		{
			swAreaHulls = R_GetAreaShadowHulls( swAreaIdx, &swNumAreaHulls );
			swAreaResiduals = R_GetAreaResidualTris( swAreaIdx );
		}
	}
	// dedup: each area's hulls are the WHOLE area (not per-surface), so emit them once per contacted
	// light. Records the light indices already served this entity (small - a few lights touch an area).
	idList<int> swHullLightsDone;

	//---------------------------
	// Find which of the visible lights contact this entity
	//
	// If the entity doesn't accept light or cast shadows from any surface,
	// this can be skipped.
	//
	// OPTIMIZE: world areas can assume all referenced lights are used
	//---------------------------
	int	numContactedLights = 0;
	static const int MAX_CONTACTED_LIGHTS = 128;
	viewLight_t* contactedLights[MAX_CONTACTED_LIGHTS];
	idInteraction* staticInteractions[MAX_CONTACTED_LIGHTS];

	if( renderEntity->hModel == NULL ||
			renderEntity->hModel->ModelHasInteractingSurfaces() ||
			renderEntity->hModel->ModelHasShadowCastingSurfaces() )
	{
		SCOPED_PROFILE_EVENT( "Find lights" );
		for( viewLight_t* vLight = viewDef->viewLights; vLight != NULL; vLight = vLight->next )
		{
			if( vLight->scissorRect.IsEmpty() )
			{
				continue;
			}
			if( vLight->entityInteractionState != NULL )
			{
				// new code path, everything was done in AddLight
				if( vLight->entityInteractionState[entityIndex] == viewLight_t::INTERACTION_YES )
				{
					contactedLights[numContactedLights] = vLight;
					staticInteractions[numContactedLights] = world->interactionTable[vLight->lightDef->index * world->interactionTableWidth + entityIndex];
					if( ++numContactedLights == MAX_CONTACTED_LIGHTS )
					{
						break;
					}
				}
				continue;
			}

			const idRenderLightLocal* lightDef = vLight->lightDef;

			if( !lightDef->globalLightBounds.IntersectsBounds( entityDef->globalReferenceBounds ) )
			{
				continue;
			}

			if( R_CullModelBoundsToLight( lightDef, entityDef->localReferenceBounds, entityDef->modelRenderMatrix ) )
			{
				continue;
			}

			if( !modelIsVisible )
			{
				// some lights have their center of projection outside the world
				if( lightDef->areaNum != -1 )
				{
					// if no part of the model is in an area that is connected to
					// the light center (it is behind a solid, closed door), we can ignore it
					bool areasConnected = false;
					for( areaReference_t* ref = entityDef->entityRefs; ref != NULL; ref = ref->ownerNext )
					{
						if( world->AreasAreConnected( lightDef->areaNum, ref->area->areaNum, PS_BLOCK_VIEW ) )
						{
							areasConnected = true;
							break;
						}
					}
					if( areasConnected == false )
					{
						// can't possibly be seen or shadowed
						continue;
					}
				}

				// check more precisely for shadow visibility
				idBounds shadowBounds;
				R_ShadowBounds( entityDef->globalReferenceBounds, lightDef->globalLightBounds, lightDef->globalLightOrigin, shadowBounds );

				// this doesn't say that the shadow can't effect anything, only that it can't
				// effect anything in the view. Only ray-traced shadows need off-view casters
				// (their TLAS gathers occluders behind the view); stencil shadow volumes render
				// into the view, so a caster whose shadow bounds miss the frustum can't affect it
				// and MUST be culled - otherwise its volume's far cap paints phantom shadows on
				// distant surfaces that vanish as the caster comes into view. So keep off-view
				// casters only when RT shadows are the active method, not merely when the archived
				// r_useRTShadows cvar lingers on alongside stencil.
				// Shadow-method precedence: RT wins when on, else stencil, else shadow maps. RT is the
				// only method that needs off-view casters (its TLAS traces them), so keep them exactly
				// when RT is the active method - stencil renders volumes into the view and must cull.
				// TLAS trim (r_rtShadowCullOffView): a caster shadowing an in-view receiver has that
				// receiver inside shadowBounds, so shadowBounds intersects the frustum and survives the
				// cull below (incl. behind-camera casters) - the only casters dropped are those whose
				// shadow lands nowhere visible, which RT does not need either. So RT can honour the same
				// cull stencil uses instead of keeping EVERY off-view caster. Set the cvar 0 to restore
				// the keep-everything behaviour if a real off-view shadow goes missing.
				// R_ShadowBounds bounds the HARD shadow (umbra). A soft penumbra flares BEYOND it, so an
					// off-view caster whose umbra bounds miss the frustum but whose penumbra reaches into the
					// view is wrongly culled here - and as the camera moves such casters cross this coarse
					// boundary and their whole penumbra wedge pops in/out (the "flicker all over the screen").
					// Soft shadows, like RT, therefore need off-view casters; the wedge pixel shader gates
					// coverage analytically, so keeping them paints no phantom on distant surfaces. 0 to A/B.
					const bool keepOffViewCasters = ( r_useRTShadows.GetBool() && !r_rtShadowCullOffView.GetBool() )
							|| ( r_useSoftShadowVolumes.GetBool() && r_softShadowKeepOffViewCasters.GetBool() );
				if( !keepOffViewCasters && idRenderMatrix::CullBoundsToMVP( viewDef->worldSpace.mvp, shadowBounds ) )
				{
					continue;
				}
			}
			contactedLights[numContactedLights] = vLight;
			staticInteractions[numContactedLights] = world->interactionTable[vLight->lightDef->index * world->interactionTableWidth + entityIndex];
			if( ++numContactedLights == MAX_CONTACTED_LIGHTS )
			{
				break;
			}
		}
	}

	// if we aren't visible and none of the shadows stretch into the view,
	// we don't need to do anything else
	if( !modelIsVisible && numContactedLights == 0 )
	{
		return;
	}

	//---------------------------
	// create a dynamic model if the geometry isn't static
	//---------------------------
	idRenderModel* model = R_EntityDefDynamicModel( entityDef );
	if( model == NULL || model->NumSurfaces() <= 0 )
	{
		return;
	}

	// add the lightweight blood decal surfaces if the model is directly visible
	if( modelIsVisible )
	{
		assert( !vEntity->scissorRect.IsEmpty() );

		if( entityDef->decals != NULL && !r_skipDecals.GetBool() )
		{
			entityDef->decals->CreateDeferredDecals( model );

			unsigned int numDrawSurfs = entityDef->decals->GetNumDecalDrawSurfs();
			for( unsigned int i = 0; i < numDrawSurfs; i++ )
			{
				drawSurf_t* decalDrawSurf = entityDef->decals->CreateDecalDrawSurf( vEntity, i );
				if( decalDrawSurf != NULL )
				{
					decalDrawSurf->linkChain = NULL;
					decalDrawSurf->nextOnLight = vEntity->drawSurfs;
					vEntity->drawSurfs = decalDrawSurf;
				}
			}
		}

		if( entityDef->overlays != NULL && !r_skipOverlays.GetBool() )
		{
			entityDef->overlays->CreateDeferredOverlays( model );

			unsigned int numDrawSurfs = entityDef->overlays->GetNumOverlayDrawSurfs();
			for( unsigned int i = 0; i < numDrawSurfs; i++ )
			{
				drawSurf_t* overlayDrawSurf = entityDef->overlays->CreateOverlayDrawSurf( vEntity, model, i );
				if( overlayDrawSurf != NULL )
				{
					overlayDrawSurf->linkChain = NULL;
					overlayDrawSurf->nextOnLight = vEntity->drawSurfs;
					vEntity->drawSurfs = overlayDrawSurf;
				}
			}
		}
	}

	//---------------------------
	// copy matrix related stuff for back-end use
	// and setup a render matrix for faster culling
	//---------------------------
	vEntity->modelDepthHack = renderEntity->modelDepthHack;
	vEntity->weaponDepthHack = renderEntity->weaponDepthHack;
	vEntity->skipMotionBlur = renderEntity->skipMotionBlur;

	memcpy( vEntity->modelMatrix, entityDef->modelMatrix, sizeof( vEntity->modelMatrix ) );
	R_MatrixMultiply( entityDef->modelMatrix, viewDef->worldSpace.modelViewMatrix, vEntity->modelViewMatrix );

	idRenderMatrix viewMat;
	idRenderMatrix::Transpose( *( idRenderMatrix* )vEntity->modelViewMatrix, viewMat );
	idRenderMatrix::Multiply( viewDef->projectionRenderMatrix, viewMat, vEntity->mvp );
	idRenderMatrix::Multiply( viewDef->unjitteredProjectionRenderMatrix, viewMat, vEntity->unjitteredMVP );
	if( renderEntity->weaponDepthHack )
	{
		idRenderMatrix::ApplyDepthHack( vEntity->mvp );
	}
	if( renderEntity->modelDepthHack != 0.0f )
	{
		idRenderMatrix::ApplyModelDepthHack( vEntity->mvp, renderEntity->modelDepthHack );
	}

	// local light and view origins are used to determine if the view is definitely outside
	// an extruded shadow volume, which means we can skip drawing the end caps
	idVec3 localViewOrigin;
	R_GlobalPointToLocal( vEntity->modelMatrix, viewDef->renderView.vieworg, localViewOrigin );

	//---------------------------
	// add all the model surfaces
	//---------------------------
	for( int surfaceNum = 0; surfaceNum < model->NumSurfaces(); surfaceNum++ )
	{
		const modelSurface_t* surf = model->Surface( surfaceNum );

		// for debugging, only show a single surface at a time
		if( r_singleSurface.GetInteger() >= 0 && surfaceNum != r_singleSurface.GetInteger() )
		{
			continue;
		}

		srfTriangles_t* tri = surf->geometry;
		if( tri == NULL )
		{
			continue;
		}
		if( tri->numIndexes == 0 )
		{
			continue;		// happens for particles
		}
		const idMaterial* shader = surf->shader;
		if( shader == NULL )
		{
			continue;
		}

		// motorsep 11-24-2014; checking for LOD surface for LOD1 iteration
		if( shader->IsLOD() )
		{
			// foresthale 2014-11-24: calculate the bounds and get the distance from camera to bounds
			idBounds& localBounds = tri->bounds;
			if( tri->staticModelWithJoints )
			{
				// skeletal models have difficult to compute bounds for surfaces, so use the whole entity
				localBounds = vEntity->entityDef->localReferenceBounds;
			}
			const float* bounds = localBounds.ToFloatPtr();
			idVec3 nearestPointOnBounds = localViewOrigin;
			nearestPointOnBounds.x = Max( nearestPointOnBounds.x, bounds[0] );
			nearestPointOnBounds.x = Min( nearestPointOnBounds.x, bounds[3] );
			nearestPointOnBounds.y = Max( nearestPointOnBounds.y, bounds[1] );
			nearestPointOnBounds.y = Min( nearestPointOnBounds.y, bounds[4] );
			nearestPointOnBounds.z = Max( nearestPointOnBounds.z, bounds[2] );
			nearestPointOnBounds.z = Min( nearestPointOnBounds.z, bounds[5] );
			idVec3 delta = nearestPointOnBounds - localViewOrigin;
			float distance = delta.LengthFast();

			if( !shader->IsLODVisibleForDistance( distance, r_lodMaterialDistance.GetFloat() ) )
			{
				continue;
			}
		}

		// foresthale 2014-09-01: don't skip surfaces that use the "forceShadows" flag
		if( !shader->IsDrawn() && !shader->SurfaceCastsShadow() )
		{
			continue;		// collision hulls, etc
		}

		// RemapShaderBySkin
		if( entityDef->parms.customShader != NULL )
		{
			// this is sort of a hack, but causes deformed surfaces to map to empty surfaces,
			// so the item highlight overlay doesn't highlight the autosprite surface
			if( shader->Deform() )
			{
				continue;
			}
			shader = entityDef->parms.customShader;
		}
		else if( entityDef->parms.customSkin )
		{
			shader = entityDef->parms.customSkin->RemapShaderBySkin( shader );
			if( shader == NULL )
			{
				continue;
			}
			// foresthale 2014-09-01: don't skip surfaces that use the "forceShadows" flag
			if( !shader->IsDrawn() && !shader->SurfaceCastsShadow() )
			{
				continue;
			}
		}

		// optionally override with the renderView->globalMaterial
		if( tr.primaryRenderView.globalMaterial != NULL )
		{
			shader = tr.primaryRenderView.globalMaterial;
		}

		SCOPED_PROFILE_EVENT( shader->GetName() );

		// debugging tool to make sure we have the correct pre-calculated bounds
		if( r_checkBounds.GetBool() )
		{
			for( int j = 0; j < tri->numVerts; j++ )
			{
				int k;
				for( k = 0; k < 3; k++ )
				{
					if( tri->verts[j].xyz[k] > tri->bounds[1][k] + CHECK_BOUNDS_EPSILON
							|| tri->verts[j].xyz[k] < tri->bounds[0][k] - CHECK_BOUNDS_EPSILON )
					{
						common->Printf( "bad tri->bounds on %s:%s\n", entityDef->parms.hModel->Name(), shader->GetName() );
						break;
					}
					if( tri->verts[j].xyz[k] > entityDef->localReferenceBounds[1][k] + CHECK_BOUNDS_EPSILON
							|| tri->verts[j].xyz[k] < entityDef->localReferenceBounds[0][k] - CHECK_BOUNDS_EPSILON )
					{
						common->Printf( "bad referenceBounds on %s:%s\n", entityDef->parms.hModel->Name(), shader->GetName() );
						break;
					}
				}
				if( k != 3 )
				{
					break;
				}
			}
		}

		// view frustum culling for the precise surface bounds, which is tighter
		// than the entire entity reference bounds
		// If the entire model wasn't visible, there is no need to check the
		// individual surfaces.
		bool surfaceDirectlyVisible = modelIsVisible && !idRenderMatrix::CullBoundsToMVP( vEntity->mvp, tri->bounds );

		// RB: added check wether GPU skinning is available at all
		const bool gpuSkinned = ( tri->staticModelWithJoints != NULL && r_useGPUSkinning.GetBool() );

		//const char* shaderName = shader->GetName();
		//if( idStr::Cmp( shaderName, "textures/rock/sharprock_dark") == 0 )
		//{
		//	tr.pc.c_mocTests += 0;
		//}

#if defined(USE_INTRINSICS_SSE)

		const bool viewInsideSurface = tri->bounds.ContainsPoint( localViewOrigin );

		//if( viewInsideSurface && idStr::Cmp( shaderName, "models/weapons/berserk/fist") != 0 )
		//{
		//	tr.pc.c_mocTests += 1;
		//
		//	tr.viewDef->renderWorld->DebugBounds( colorCyan, tri->bounds, renderEntity->origin );
		//}

		// RB: test surface visibility by drawing the triangles of the bounds
		if( r_useMaskedOcclusionCulling.GetBool() && !viewInsideSurface && !viewDef->isMirror && !viewDef->isSubview )
		{
			if( //!model->IsStaticWorldModel() &&
				!renderEntity->weaponDepthHack && renderEntity->modelDepthHack == 0.0f )
			{
				idVec4 triVerts[8];

				tr.pc.c_mocIndexes += 36;
				tr.pc.c_mocVerts += 8;

				idBounds surfaceBounds;
#if 1
				if( gpuSkinned )
				{
					surfaceBounds = vEntity->entityDef->localReferenceBounds;
				}
				else
#endif
				{
					surfaceBounds = tri->bounds;
				}

				idRenderMatrix modelRenderMatrix;
				idRenderMatrix::CreateFromOriginAxis( renderEntity->origin, renderEntity->axis, modelRenderMatrix );

				idRenderMatrix inverseBaseModelProject;
				idRenderMatrix::OffsetScaleForBounds( modelRenderMatrix, surfaceBounds, inverseBaseModelProject );

				idRenderMatrix invProjectMVPMatrix;
				idRenderMatrix::Multiply( viewDef->worldSpace.unjitteredMVP, inverseBaseModelProject, invProjectMVPMatrix );

				tr.pc.c_mocTests += 1;

				// NOTE: unit cube instead of zeroToOne cube
				idVec4* verts = tr.maskedUnitCubeVerts;
				for( int i = 0; i < 8; i++ )
				{
					// transform to clip space
					invProjectMVPMatrix.TransformPoint( verts[i], triVerts[i] );
				}


				// backface none so objects are still visible where we run into
#if MOC_MULTITHREADED
				tr.maskedOcclusionThreaded->SetMatrix( NULL );
				MaskedOcclusionCulling::CullingResult result = tr.maskedOcclusionThreaded->TestTriangles( ( float* )triVerts, tr.maskedZeroOneCubeIndexes, 12, MaskedOcclusionCulling::BACKFACE_NONE );
#else
				MaskedOcclusionCulling::CullingResult result = tr.maskedOcclusionCulling->TestTriangles( ( float* )triVerts, tr.maskedZeroOneCubeIndexes, 12, NULL, MaskedOcclusionCulling::BACKFACE_NONE );
#endif
				if( result != MaskedOcclusionCulling::VISIBLE )
				{
					tr.pc.c_mocCulledSurfaces += 1;
					surfaceDirectlyVisible = false;
				}
			}
		}
#endif // #if defined(USE_INTRINSICS_SSE)

		//--------------------------
		// base drawing surface
		//--------------------------
		const float* shaderRegisters = NULL;
		drawSurf_t* baseDrawSurf = NULL;
		if( surfaceDirectlyVisible && shader->IsDrawn() )
		{
			// make sure we have an ambient cache and all necessary normals / tangents
			if( !vertexCache.CacheIsCurrent( tri->indexCache ) )
			{
				tri->indexCache = vertexCache.AllocIndex( tri->indexes, tri->numIndexes );
			}

			if( !vertexCache.CacheIsCurrent( tri->ambientCache ) )
			{
				// we are going to use it for drawing, so make sure we have the tangents and normals
				if( shader->ReceivesLighting() && !tri->tangentsCalculated )
				{
					assert( tri->staticModelWithJoints == NULL );
					R_DeriveTangents( tri );

					// RB: this was hit by parametric particle models ..
					//assert( false );	// this should no longer be hit
					// RB end
				}
				tri->ambientCache = vertexCache.AllocVertex( tri->verts, tri->numVerts );
			}

			// add the surface for drawing
			// we can re-use some of the values for light interaction surfaces
			baseDrawSurf = ( drawSurf_t* )R_FrameAlloc( sizeof( *baseDrawSurf ), FRAME_ALLOC_DRAW_SURFACE );
			baseDrawSurf->frontEndGeo = tri;
			baseDrawSurf->space = vEntity;
			baseDrawSurf->scissorRect = vEntity->scissorRect;
			baseDrawSurf->extraGLState = 0;

			R_SetupDrawSurfShader( baseDrawSurf, shader, renderEntity );

			shaderRegisters = baseDrawSurf->shaderRegisters;

			// Check for deformations (eyeballs, flares, etc)
			const deform_t shaderDeform = shader->Deform();
			if( shaderDeform != DFRM_NONE )
			{
				drawSurf_t* deformDrawSurf = R_DeformDrawSurf( baseDrawSurf );
				if( deformDrawSurf != NULL )
				{
					// any deforms may have created multiple draw surfaces
					for( drawSurf_t* surf = deformDrawSurf, * next = NULL; surf != NULL; surf = next )
					{
						next = surf->nextOnLight;

						surf->linkChain = NULL;
						surf->nextOnLight = vEntity->drawSurfs;
						vEntity->drawSurfs = surf;
					}
				}
			}

			// Most deform source surfaces do not need to be rendered.
			// However, particles are rendered in conjunction with the source surface.
			if( shaderDeform == DFRM_NONE || shaderDeform == DFRM_PARTICLE || shaderDeform == DFRM_PARTICLE2 )
			{
				// copy verts and indexes to this frame's hardware memory if they aren't already there
				if( !vertexCache.CacheIsCurrent( tri->ambientCache ) )
				{
					tri->ambientCache = vertexCache.AllocVertex( tri->verts, tri->numVerts );
				}
				if( !vertexCache.CacheIsCurrent( tri->indexCache ) )
				{
					tri->indexCache = vertexCache.AllocIndex( tri->indexes, tri->numIndexes );
				}

				R_SetupDrawSurfJoints( baseDrawSurf, tri, shader );

				baseDrawSurf->numIndexes = tri->numIndexes;
				baseDrawSurf->ambientCache = tri->ambientCache;
				baseDrawSurf->indexCache = tri->indexCache;

				baseDrawSurf->linkChain = NULL;		// link to the view
				baseDrawSurf->nextOnLight = vEntity->drawSurfs;
				vEntity->drawSurfs = baseDrawSurf;
			}

			// RB: use area the surface is in because a model can span multiple areas #965
			baseDrawSurf->area = NULL;

			if( shader->ReceivesLighting() )
			{
				idVec3 surfaceCenter;
				idVec3 triCenter = tri->bounds.GetCenter();

				idRenderMatrix modelRenderMatrix;
				idRenderMatrix::CreateFromOriginAxis( renderEntity->origin, renderEntity->axis, modelRenderMatrix );
				modelRenderMatrix.TransformPoint( triCenter, surfaceCenter );

				int surfaceArea = tr.primaryWorld->PointInArea( surfaceCenter );

				for( areaReference_t* ref = entityDef->entityRefs; ref != NULL; ref = ref->ownerNext )
				{
					idImage* lightGridImage = ref->area->lightGrid.GetIrradianceImage();

					if( surfaceArea == ref->area->areaNum && ref->area->lightGrid.lightGridPoints.Num() && lightGridImage != NULL && !lightGridImage->IsDefaulted() )
					{
						baseDrawSurf->area = ref->area;
						break;
					}
				}

				// RB: use first valid lightgrid
				// this would be wrong but less wrong than a flickering env_probe fallback
				if( baseDrawSurf->area == NULL )
				{
					for( areaReference_t* ref = entityDef->entityRefs; ref != NULL; ref = ref->ownerNext )
					{
						idImage* lightGridImage = ref->area->lightGrid.GetIrradianceImage();

						if( ref->area->lightGrid.lightGridPoints.Num() && lightGridImage != NULL && !lightGridImage->IsDefaulted() )
						{
							baseDrawSurf->area = ref->area;
							break;
						}
					}
				}

#if 0
				// show which area the surface is coming from
				//if( baseDrawSurf->area == NULL )
				{
					idBounds surfaceBounds;
					if( gpuSkinned )
					{
						surfaceBounds = vEntity->entityDef->localReferenceBounds;
					}
					else
					{
						surfaceBounds = tri->bounds;
					}

					idRenderMatrix modelRenderMatrix;
					idRenderMatrix::CreateFromOriginAxis( renderEntity->origin, renderEntity->axis, modelRenderMatrix );

					idRenderMatrix inverseBaseModelProject;
					idRenderMatrix::OffsetScaleForBounds( modelRenderMatrix, surfaceBounds, inverseBaseModelProject );

					// NOTE: unit cube instead of zeroToOne cube
					idVec4* verts = tr.maskedUnitCubeVerts;
					idVec4 triVerts[8];

					for( int i = 0; i < 8; i++ )
					{
						// transform to clip space
						inverseBaseModelProject.TransformPoint( verts[i], triVerts[i] );
					}

					static idVec4 colors[] = { colorBrown, colorBlue, colorCyan, colorGreen, colorYellow, colorRed, colorWhite };
					idVec4 color = colors[surfaceArea & 7];

					if( baseDrawSurf->area == NULL )
					{
						color = colorPurple;
					}

					// same as idRenderWorldLocal::DebugBox
					const int lifetime = 0;
					for( int i = 0; i < 4; i++ )
					{
						tr.viewDef->renderWorld->DebugLine( color, triVerts[i].ToVec3(), triVerts[( i + 1 ) & 3].ToVec3(), lifetime );
						tr.viewDef->renderWorld->DebugLine( color, triVerts[4 + i].ToVec3(), triVerts[4 + ( ( i + 1 ) & 3 )].ToVec3(), lifetime );
						tr.viewDef->renderWorld->DebugLine( color, triVerts[i].ToVec3(), triVerts[4 + i].ToVec3(), lifetime );
					}

					tr.viewDef->renderWorld->DebugAxis( surfaceCenter, renderEntity->axis );
				}
#endif
			}
		}

		//----------------------------------------
		// add all light interactions
		//----------------------------------------
		for( int contactedLight = 0; contactedLight < numContactedLights; contactedLight++ )
		{
			viewLight_t* vLight = contactedLights[contactedLight];
			const idRenderLightLocal* lightDef = vLight->lightDef;
			const idInteraction* interaction = staticInteractions[contactedLight];

			// check for a static interaction
			surfaceInteraction_t* surfInter = NULL;
			if( interaction > INTERACTION_EMPTY && interaction->staticInteraction )
			{
				// we have a static interaction that was calculated accurately
				assert( model->NumSurfaces() == interaction->numSurfaces );
				surfInter = &interaction->surfaces[surfaceNum];
			}
			else
			{
				// try to do a more precise cull of this model surface to the light
				if( R_CullModelBoundsToLight( lightDef, tri->bounds, entityDef->modelRenderMatrix ) )
				{
					continue;
				}
			}

			// "invisible ink" lights and shaders (imp spawn drawing on walls, etc)
			if( shader->Spectrum() != lightDef->lightShader->Spectrum() )
			{
				continue;
			}

			// Calculate the local light origin to determine if the view is inside the shadow
			// projection and to calculate the triangle facing for dynamic shadow volumes.
			idVec3 localLightOrigin;
			R_GlobalPointToLocal( vEntity->modelMatrix, lightDef->globalLightOrigin, localLightOrigin );


			//--------------------------
			// surface light interactions
			//--------------------------

			if( addInteractions && surfaceDirectlyVisible && shader->ReceivesLighting() )
			{
				// static interactions can commonly find that no triangles from a surface
				// contact the light, even when the total model does
				if( surfInter == NULL || surfInter->lightTrisIndexCache > 0 )
				{
					// make sure we have a valid shader register even if we didn't generate a drawn mesh above
					if( shaderRegisters == NULL )
					{
						drawSurf_t scratchSurf;
						R_SetupDrawSurfShader( &scratchSurf, shader, renderEntity );
						shaderRegisters = scratchSurf.shaderRegisters;
					}

					if( shaderRegisters != NULL )
					{
						// create a drawSurf for this interaction
						drawSurf_t* lightDrawSurf = ( drawSurf_t* )R_FrameAlloc( sizeof( *lightDrawSurf ), FRAME_ALLOC_DRAW_SURFACE );

						if( surfInter != NULL )
						{
							// optimized static interaction
							lightDrawSurf->numIndexes = surfInter->numLightTrisIndexes;
							lightDrawSurf->indexCache = surfInter->lightTrisIndexCache;
						}
						else
						{
							// throw the entire source surface at it without any per-triangle culling
							lightDrawSurf->numIndexes = tri->numIndexes;
							lightDrawSurf->indexCache = tri->indexCache;
						}

						lightDrawSurf->ambientCache = tri->ambientCache;
						lightDrawSurf->frontEndGeo = tri;
						lightDrawSurf->space = vEntity;
						lightDrawSurf->material = shader;
						lightDrawSurf->extraGLState = 0;
						lightDrawSurf->scissorRect = vLight->scissorRect; // interactionScissor;
						lightDrawSurf->sort = 0.0f;
						lightDrawSurf->shaderRegisters = shaderRegisters;

						R_SetupDrawSurfJoints( lightDrawSurf, tri, shader );

						// Determine which linked list to add the light surface to.
						// There will only be localSurfaces if the light casts shadows and
						// there are surfaces with NOSELFSHADOW.
						if( shader->Coverage() == MC_TRANSLUCENT )
						{
							lightDrawSurf->linkChain = &vLight->translucentInteractions;
						}
						else if( !lightDef->parms.noShadows && shader->TestMaterialFlag( MF_NOSELFSHADOW ) )
						{
							lightDrawSurf->linkChain = &vLight->localInteractions;
						}
						else
						{
							lightDrawSurf->linkChain = &vLight->globalInteractions;
						}
						lightDrawSurf->nextOnLight = vEntity->drawSurfs;
						vEntity->drawSurfs = lightDrawSurf;
					}
				}
			}

			//--------------------------
			// surface shadows
			//--------------------------

#if 1
			if( !shader->SurfaceCastsShadow() && !( r_forceShadowMapsOnAlphaTestedSurfaces.GetBool() && shader->Coverage() == MC_PERFORATED ) )
			{
				continue;
			}
#else
			// Steel Storm 2 behaviour - this destroys many alpha tested shadows in vanilla BFG

			// motorsep 11-08-2014; if r_forceShadowMapsOnAlphaTestedSurfaces is 0 when shadow mapping is on,
			// don't render shadows from all alphaTest surfaces.
			// Useful as global performance booster for old GPUs to disable shadows from grass/foliage/etc.
			if( r_useShadowMapping.GetBool() )
			{
				if( shader->Coverage() == MC_PERFORATED )
				{
					if( !r_forceShadowMapsOnAlphaTestedSurfaces.GetBool() )
					{
						continue;
					}
				}
			}

			// if material has "noShadows" global key
			if( !shader->SurfaceCastsShadow() )
			{
				// motorsep 11-08-2014; if r_forceShadowMapsOnAlphaTestedSurfaces is 1 when shadow mapping is on,
				// check if a surface IS NOT alphaTested and has "noShadows" global key;
				// or if a surface IS alphaTested and has "noShadows" global key;
				// if either is true, don't make surfaces cast shadow maps.
				if( r_useShadowMapping.GetBool() )
				{
					if( shader->Coverage() != MC_PERFORATED && shader->TestMaterialFlag( MF_NOSHADOWS ) )
					{
						continue;
					}
					else if( shader->Coverage() == MC_PERFORATED && shader->TestMaterialFlag( MF_NOSHADOWS ) )
					{
						continue;
					}
				}
				else
				{
					continue;
				}
			}
#endif

			if( !lightDef->LightCastsShadows() )
			{
				continue;
			}

			// some entities, like view weapons, don't cast any shadows
			if( entityDef->parms.noShadow )
			{
				continue;
			}

			// No shadow if it's suppressed for this light.
			if( entityDef->parms.suppressShadowInLightID && entityDef->parms.suppressShadowInLightID == lightDef->parms.lightId )
			{
				continue;
			}

			// stencil shadow volumes: build a shadow-volume drawSurf from the surface's static
			// shadow volume (indexes in surfInter->shadowIndexCache reference the surface's doubled
			// static shadowCache, both built at load) and link it into the light's global/local
			// shadow chain, then skip the shadow-map occluder path. M2: static casters only (their
			// shadowCache is a static buffer); always z-fail with caps (z-pass + cap selection is a
			// later optimisation). Dynamic / GPU-skinned casters fall through to shadow maps for now.
			// Soft shadow volumes reuse the very same shadow-VOLUME geometry (the umbra is stamped from
			// it into the light visibility buffer), so build it for either method.
			if( ( r_useStencilShadows.GetBool() || r_useSoftShadowVolumes.GetBool() ) && !r_useRTShadows.GetBool() )	// RT takes precedence when both are on
			{
				// stencil mode: build a shadow-VOLUME drawSurf if this static caster has one. Crucially
				// we NEVER build a shadow-MAP occluder in this mode - those carry ambientCache, not
				// shadowCache, and the stencil pass (which walks vLight->globalShadows/localShadows)
				// would try to draw them as volumes and fail. So continue regardless. M2: static
				// casters only; always z-fail with caps (z-pass + cap selection is a later opt).
				// M2 diagnostics: tally why casters are accepted/rejected for a stencil volume.
				extern int fe_stencilBuilt, fe_rejSilEdges, fe_rejSurfInter, fe_rejNumIdx, fe_rejIdxStale, fe_rejShadowCache;
				if( tri->silEdges == NULL )			fe_rejSilEdges++;
				else if( surfInter == NULL )			fe_rejSurfInter++;
				else if( surfInter->numShadowIndexes <= 0 )	fe_rejNumIdx++;
				else if( !vertexCache.CacheIsCurrent( surfInter->shadowIndexCache ) )	fe_rejIdxStale++;
				else if( !vertexCache.CacheIsStatic( tri->shadowCache ) )	fe_rejShadowCache++;
				else						fe_stencilBuilt++;

				// diagnostics: with r_singleLight + r_listViewLights, name every accepted stencil
				// caster so an enveloping/broken volume can be traced to its source surface.
				extern idCVar r_listViewLights;
				if( r_listViewLights.GetBool() && r_singleLight.GetInteger() == vLight->lightDef->index &&
						tri->silEdges != NULL && surfInter != NULL && surfInter->numShadowIndexes > 0 )
				{
					common->Printf( "stencilCaster light %i: ent %i model '%s' shader '%s' tris %i shadowIdx %i bounds (%.0f %.0f %.0f)-(%.0f %.0f %.0f)\n",
									vLight->lightDef->index, entityDef->index,
									entityDef->parms.hModel ? entityDef->parms.hModel->Name() : "?",
									shader->GetName(), tri->numIndexes / 3, surfInter->numShadowIndexes,
									tri->bounds[0].x, tri->bounds[0].y, tri->bounds[0].z,
									tri->bounds[1].x, tri->bounds[1].y, tri->bounds[1].z );
				}

				if( tri->silEdges != NULL && surfInter != NULL && surfInter->numShadowIndexes > 0 &&
						vertexCache.CacheIsCurrent( surfInter->shadowIndexCache ) &&
						vertexCache.CacheIsStatic( tri->shadowCache ) )
				{
					drawSurf_t* shadowDrawSurf = ( drawSurf_t* )R_FrameAlloc( sizeof( *shadowDrawSurf ), FRAME_ALLOC_DRAW_SURFACE );

					// Z-pass vs Z-fail: force Z-fail (full front+rear caps) only when the view might be
						// inside this caster's infinite shadow volume; otherwise Z-pass with NO caps. Z-pass
						// omits the projected-to-infinity far cap - forcing that cap for every caster is what
						// paints phantom shadows on distant geometry down long corridors (dark at range, lit
						// up close). localView/localLightOrigin are in this entity's model space.
						const bool viewMaybeInside = R_ViewPotentiallyInsideInfiniteShadowVolume(
								tri->bounds, localLightOrigin, localViewOrigin,
								r_znear.GetFloat() * INSIDE_SHADOW_VOLUME_EXTRA_STRETCH );
						shadowDrawSurf->numIndexes = viewMaybeInside ? surfInter->numShadowIndexes : surfInter->numShadowIndexesNoCaps;
					shadowDrawSurf->indexCache = surfInter->shadowIndexCache;
					shadowDrawSurf->shadowCache = tri->shadowCache;
					shadowDrawSurf->ambientCache = 0;
					shadowDrawSurf->jointCache = 0;
					shadowDrawSurf->frontEndGeo = NULL;
					shadowDrawSurf->space = vEntity;
					shadowDrawSurf->material = NULL;
					shadowDrawSurf->extraGLState = 0;
					shadowDrawSurf->scissorRect = vLight->scissorRect;
					shadowDrawSurf->sort = 0.0f;
					shadowDrawSurf->renderZFail = viewMaybeInside ? 1 : 0;	// Z-fail (with caps) only when possibly inside
					shadowDrawSurf->shaderRegisters = NULL;

					shadowDrawSurf->linkChain = shader->TestMaterialFlag( MF_NOSELFSHADOW ) ? &vLight->localShadows : &vLight->globalShadows;
					shadowDrawSurf->nextOnLight = vEntity->drawSurfs;
					vEntity->drawSurfs = shadowDrawSurf;
				}
				else if( surfInter == NULL && tri->silEdges != NULL )
				{
					// Moved / dynamic / animated caster: no baked static interaction exists (surfInter
					// == NULL), so this surface's silhouette against the light changes every frame.
					// Rebuild the shadow-volume index set on the fly from the model's silEdges + doubled
					// shadowCache and upload it to the frame index cache. This is what makes moving props
					// AND animated characters cast a stencil shadow that tracks them, instead of silently
					// dropping out (the old "static casters only" punt that left moved objects shadowless).
					// ponytail: per-frame R_CreateInteractionShadowVolume does a malloc+free; fine for
					// the moving casters in a scene, revisit if one throws hundreds.

					// Ensure a shadow vertex cache for this pose. Rigid movers carry a static shadowCache
					// (always current). Animated (MD5) casters get PERSISTENT posed verts in UpdateSurface;
					// rebuild the per-frame shadow cache from them whenever it's stale - crucially this
					// covers a settled ragdoll, whose cached dynamic model stops calling UpdateSurface, so
					// its frame shadow cache from an earlier frame has expired (that was the corpse whose
					// shadow vanished on settling). facing/cull use the posed override transparently.
					if( !vertexCache.CacheIsCurrent( tri->shadowCache ) )
					{
						const idDrawVert* posed = tri->posedShadowVerts != NULL ? tri->posedShadowVerts : tri->verts;
						if( posed != NULL && tri->numVerts > 0 )
						{
							const int numShadowVerts = tri->numVerts * 2;
							idShadowVert* shadowVerts = ( idShadowVert* )R_FrameAlloc( numShadowVerts * ( int )sizeof( idShadowVert ), FRAME_ALLOC_UNKNOWN );
							idShadowVert::CreateShadowCache( shadowVerts, posed, tri->numVerts );
							tri->shadowCache = vertexCache.AllocVertex( shadowVerts, numShadowVerts, sizeof( idShadowVert ) );
						}
					}

					srfTriangles_t* shadowTri = vertexCache.CacheIsCurrent( tri->shadowCache ) ? R_CreateInteractionShadowVolume( entityDef, tri, lightDef ) : NULL;
					if( shadowTri != NULL )
					{
						vertCacheHandle_t shadowIndexCache = vertexCache.AllocIndex( shadowTri->indexes, shadowTri->numIndexes );

						drawSurf_t* shadowDrawSurf = ( drawSurf_t* )R_FrameAlloc( sizeof( *shadowDrawSurf ), FRAME_ALLOC_DRAW_SURFACE );

						const bool viewMaybeInside = R_ViewPotentiallyInsideInfiniteShadowVolume(
								tri->bounds, localLightOrigin, localViewOrigin,
								r_znear.GetFloat() * INSIDE_SHADOW_VOLUME_EXTRA_STRETCH );
						// opaque casters can drop the caps when the view is safely outside the volume;
						// perforated ones already fold caps in (numShadowIndexesNoCaps == numIndexes there).
						shadowDrawSurf->numIndexes = viewMaybeInside ? shadowTri->numIndexes
								: ( shader->Coverage() == MC_OPAQUE ? shadowTri->numShadowIndexesNoCaps : shadowTri->numIndexes );
						shadowDrawSurf->indexCache = shadowIndexCache;
						shadowDrawSurf->shadowCache = tri->shadowCache;
						shadowDrawSurf->ambientCache = 0;
						shadowDrawSurf->jointCache = 0;
						shadowDrawSurf->frontEndGeo = NULL;
						shadowDrawSurf->space = vEntity;
						shadowDrawSurf->material = NULL;
						shadowDrawSurf->extraGLState = 0;
						shadowDrawSurf->scissorRect = vLight->scissorRect;
						shadowDrawSurf->sort = 0.0f;
						shadowDrawSurf->renderZFail = viewMaybeInside ? 1 : 0;
						shadowDrawSurf->shaderRegisters = NULL;

						shadowDrawSurf->linkChain = shader->TestMaterialFlag( MF_NOSELFSHADOW ) ? &vLight->localShadows : &vLight->globalShadows;
						shadowDrawSurf->nextOnLight = vEntity->drawSurfs;
						vEntity->drawSurfs = shadowDrawSurf;

						R_FreeStaticTriSurf( shadowTri );
					}
				}

				// Analytic soft shadows: collect this caster's silhouette EDGES in world space and link a
				// carrier surf into the light's chain. The interaction pixel shader evaluates coverage per
				// fragment against the exact receiver position (no screen-space reconstruction); the umbra
				// emerges from summing the edges. Same silhouette/winding as the stencil path.
				//
				// silEdges are a WEDGE-path requirement (the light-silhouette walk). FACE coverage streams the
				// raw triangles and needs no adjacency - and silEdges creation FAILS on non-2-manifold meshes
				// (rails, grates, decor), which silently dropped those casters from the soft stream and left
				// their shadows missing entirely (softgate: the erebus1_05 far-floor EXTENT defects).
				// BENCH reconstruction (com_softShadowGateBenchReplay): the captured caster entities ARE
				// the complete live soft-caster set (world faces included), so the loaded worldspawn must
				// NOT also soft-cast or every world face is double-counted. Exclude the static world model
				// from soft collection when this is set; the captured casters carry its contribution.
				extern idCVar r_softShadowBenchExcludeWorld;
				const bool swExcludeWorld = r_softShadowBenchExcludeWorld.GetBool()
											&& entityDef->parms.hModel != NULL && entityDef->parms.hModel->IsStaticWorldModel();
				// Phase 0 ceiling probe: drop static-caster x static-light collection so the GPU walk cost
				// attributable to the reusable static half can be measured (render is intentionally wrong).
				extern idCVar r_softShadowSkipStatic;
				const bool swSkipStatic = r_softShadowSkipStatic.GetBool() && R_SoftCasterIsStatic( entityDef, vLight );
				if( !swExcludeWorld && !swSkipStatic && r_useSoftShadowVolumes.GetBool() && ( tri->silEdges != NULL || r_softShadowFaceCoverage.GetBool() ) )
				{
					// WEDGE-SELECTOR method census (one-shot): accumulate this soft caster; report the last
					// fully-collected frame at probe-end (settled cap view). Read-only.
					{
						extern idCVar r_softShadowWedgeCensus;
						if( r_softShadowWedgeCensus.GetInteger() > 0 )
						{
							if( s_wcFrame != tr.frameCount )
							{
								if( s_wcFrame >= 0 && s_wc.casters > 0 ) { s_wcLast = s_wc; }
								s_wcFrame = tr.frameCount;
								memset( &s_wc, 0, sizeof( s_wc ) );
							}
							R_SoftWedgeCensusAccum( tri, lightDef, vLight, vEntity->modelMatrix );
						}
					}
					// BRUSH-RECOVERY: this area carries dmap'd convex hulls -> emit one analytic hull record
					// per hull (shared .w=-N encoding, FillHull) INSTEAD of this area's triangle stream, and
					// ONCE per light (the hulls are the whole area, not per surface). Needs face-coverage: the
					// .w=-N decode lives in that walk. Lossless: a brush is convex, hull == brush projection.
					// Gate the hull swap on the FULL precondition: area has hulls, face-coverage is on, AND this is
					// a SOFT light (penumbra > 0). A hard light (penumbra <= 0) or a hull-less area falls THROUGH
					// to the canonical triangle/silhouette path below - it must never be swallowed here casting
					// nothing. (The old form entered on swAreaHulls alone, then bailed inside for hard lights.)
					// PACKAGE B: enter also when the area has ONLY residuals (all brushes non-emittable); the hull
						// loop no-ops (swNumAreaHulls 0) and the residual triangles still stream below.
						if( ( swAreaHulls != NULL || swAreaResiduals != NULL ) && r_softShadowFaceCoverage.GetBool() && R_SoftPenumbraRadius( lightDef ) > 0.0f )
					{
						if( swHullLightsDone.FindIndex( vLight->lightDef->index ) == -1 )
						{
							{
								swHullLightsDone.Append( vLight->lightDef->index );
								extern int fe_softEdgesCollected;
								// PACKAGE B residual triangles: stream the area's NON-hull-emittable brushes ONCE per light, exactly like the
									// else-branch triangle path. A frame-alloc'd wrapper srfTriangles_t points at the loader-owned residual
									// arrays (stable for the map); R_CollectPenumbraFaces reads only numVerts/verts/numIndexes/indexes/
									// posedShadowVerts. Hulls(emittable)+residuals(non-emittable) = every brush ONCE (lossless).
									if( swAreaResiduals != NULL )
									{
										srfTriangles_t* rtri = ( srfTriangles_t* )R_FrameAlloc( sizeof( *rtri ), FRAME_ALLOC_UNKNOWN );
										memset( rtri, 0, sizeof( *rtri ) );
										rtri->numVerts = swAreaResiduals->verts.Num();
										rtri->verts = const_cast<idDrawVert*>( swAreaResiduals->verts.Ptr() );
										rtri->numIndexes = swAreaResiduals->indexes.Num();
										rtri->indexes = const_cast<triIndex_t*>( swAreaResiduals->indexes.Ptr() );
										rtri->posedShadowVerts = NULL;
										rtri->bounds.Clear();		// for the draw-sort depth-bounds key (main.cpp); verts are stable
										for( int v = 0; v < rtri->numVerts; v++ ) { rtri->bounds.AddPoint( rtri->verts[v].xyz ); }
										idVec4* rElems = NULL;
										int rEdges = 0;
										idVec4* rClusters = NULL;
										int rClu = 0;
										bool rIsBox = false;		// R_CollectPenumbraFaces may auto-reduce (proxy-box/coplanar); honor its tag like the else path
										R_CollectPenumbraFaces( entityDef, rtri, lightDef, R_SoftPenumbraRadius( lightDef ),
												vEntity->modelMatrix, &rElems, &rEdges, &rClusters, &rClu, &rIsBox );
										if( rEdges > 0 )
										{
											drawSurf_t* resSurf = ( drawSurf_t* )R_FrameAlloc( sizeof( *resSurf ), FRAME_ALLOC_DRAW_SURFACE );
											memset( resSurf, 0, sizeof( *resSurf ) );
											resSurf->softEdges = ( softShadowEdge_t* )rElems;
											resSurf->numSoftEdges = rEdges;
											resSurf->softClusters = rClusters;
											resSurf->numSoftClusters = rClu;
											resSurf->frontEndGeo = rtri;		// the residual triangles ARE this caster's ground-truth mesh (capture)
											resSurf->space = vEntity;
											resSurf->softIsBox = rIsBox;		// false for ordinary residual tris; honored if auto-reduced
											resSurf->scissorRect = vLight->scissorRect;
											resSurf->linkChain = &vLight->softShadowWedges;
											resSurf->nextOnLight = vEntity->drawSurfs;
											vEntity->drawSurfs = resSurf;
											fe_softEdgesCollected += rEdges;
										}
									}
									for( int hIdx = 0; hIdx < swNumAreaHulls; hIdx++ )
								{
									const areaShadowHull_t& hull = swAreaHulls[hIdx];
									// analytic record: .w=-N discriminator + N verts, padded to whole triangles.
									// Hulls are stored in map space; area world models are identity, so map == world.
									float slots[ 12 * 4 ];		// <=9 slots (N<=8 -> 3 tris); headroom
									const int numSlots = SoftHull_EmitRecord( hull.verts, hull.numVerts, slots );
									if( numSlots <= 0 )
									{
										continue;
									}
									const int numTris = numSlots / 3;
									idVec4* recs = ( idVec4* )R_FrameAlloc( numSlots * sizeof( idVec4 ), FRAME_ALLOC_UNKNOWN );
									idBounds hb;
									hb.Clear();
									for( int s = 0; s < numSlots; s++ )
									{
										recs[s].Set( slots[s * 4 + 0], slots[s * 4 + 1], slots[s * 4 + 2], slots[s * 4 + 3] );
										hb.AddPoint( recs[s].ToVec3() );
									}
									// one cluster, mirroring the box/poly path: (center,radius),(firstTri=0,numTris,0,0)
									idVec4* clu = ( idVec4* )R_FrameAlloc( 2 * sizeof( idVec4 ), FRAME_ALLOC_UNKNOWN );
									const idVec3 cc = ( hb[0] + hb[1] ) * 0.5f;
									clu[0].Set( cc.x, cc.y, cc.z, ( hb[1] - hb[0] ).Length() * 0.5f * 1.00001f );
									clu[1].Set( 0.0f, ( float )numTris, 0.0f, 0.0f );

									drawSurf_t* hullSurf = ( drawSurf_t* )R_FrameAlloc( sizeof( *hullSurf ), FRAME_ALLOC_DRAW_SURFACE );
									memset( hullSurf, 0, sizeof( *hullSurf ) );
									hullSurf->softEdges = ( softShadowEdge_t* )recs;
									hullSurf->numSoftEdges = numSlots;
									hullSurf->softClusters = clu;
									hullSurf->numSoftClusters = 1;
									hullSurf->frontEndGeo = tri;
									hullSurf->space = vEntity;
									hullSurf->softIsBox = true;		// analytic caster: flatten stamps a negative count -> high-bit tile tag -> .w=-N decode
									hullSurf->scissorRect = vLight->scissorRect;
									hullSurf->linkChain = &vLight->softShadowWedges;
									hullSurf->nextOnLight = vEntity->drawSurfs;
									vEntity->drawSurfs = hullSurf;
									fe_softEdgesCollected += numSlots;
								}
							}
						}
					}
					else
					{
					const int swCollectStart = Sys_Microseconds();
					softShadowEdge_t* sedges = NULL;
					int nedges = 0;
					// HYBRID WHITELIST: the wedge-form silhouette collected ALONGSIDE the face triangles, so both
					// representations of this caster coexist and the term CS can serve clean casters via the wedge
					softShadowEdge_t* wSedges = NULL;
					int wNedges = 0;
					// FRONT-FACE coverage streams the caster's triangles (accurate + stable); the default streams
					// the light silhouette (undershoots off-axis). Face mode uses the v2 float4-triple layout,
					// stored through the same drawSurf fields (count in FLOAT4 elements; see drawSurf_t).
					idVec4* faceClusters = NULL;
					int nClusters = 0;
					bool swFaceIsBox = false;			// analytic box caster (curated): tagged numTris<0 at flatten
					// WEDGE SELECTOR: a coplanar-convex eligible caster becomes ONE analytic HULL record (.w=-N)
					// instead of its triangle stream - the walk fills its exact silhouette via SoftScan_FillHull
					// into the shared scanline grid (O(verts), lossless: the hull certificate proved it fills its
					// convex hull, and grid-OR unions casters exactly). Reuses the softIsBox analytic record path.
					bool swHull = false;
					if( R_SoftWedgeSelectorActive() )
					{
						idVec3 hullV[SW_HULL_MAX_VERTS];
						int hn = 0, hvAll = 0;
						bool cvx = false;
						if( R_SoftWedgeMethod( tri, lightDef, vLight, vEntity->modelMatrix, hullV, hn, hvAll, cvx ) == SW_M_FILLHULL_CONVEX )
						{
							float hverts[SW_HULL_MAX_VERTS * 3];
							for( int i = 0; i < hn; i++ ) { hverts[i * 3 + 0] = hullV[i].x; hverts[i * 3 + 1] = hullV[i].y; hverts[i * 3 + 2] = hullV[i].z; }
							float slots[SW_HULL_MAX_VERTS * 4 + 8];			// <=3 tris -> 9 slots
							const int numSlots = SoftHull_EmitRecord( hverts, hn, slots );
							if( numSlots > 0 )
							{
								idVec4* recs = ( idVec4* )R_FrameAlloc( numSlots * sizeof( idVec4 ), FRAME_ALLOC_UNKNOWN );
								for( int i = 0; i < numSlots; i++ ) { recs[i].Set( slots[i * 4 + 0], slots[i * 4 + 1], slots[i * 4 + 2], slots[i * 4 + 3] ); }
								sedges = ( softShadowEdge_t* )recs;
								nedges = numSlots;
								swFaceIsBox = true;			// analytic record: flatten tags numTris<0; walk reads slot0.w=-N -> FillHull
								swHull = true;
								extern long g_swWedgeRoutedTris, g_swWedgeRoutedHullVerts;
								g_swWedgeRoutedTris += tri->numIndexes / 3;
								g_swWedgeRoutedHullVerts += hn;
							}
						}
					}
					if( swHull )
					{
						// hull record already assembled above
					}
					else if( r_softShadowFaceCoverage.GetBool() )
					{
						idVec4* faceElems = NULL;
						R_CollectPenumbraFaces( entityDef, tri, lightDef, R_SoftPenumbraRadius( lightDef ),
												vEntity->modelMatrix, &faceElems, &nedges, &faceClusters, &nClusters, &swFaceIsBox );
						sedges = ( softShadowEdge_t* )faceElems;
						extern idCVar r_softShadowWedgeWhitelist;
						if( r_softShadowWedgeWhitelist.GetBool() )
						{
							R_CollectPenumbraEdges( entityDef, tri, lightDef, R_SoftPenumbraRadius( lightDef ),
													vEntity->modelMatrix, &wSedges, &wNedges );
						}
					}
					else
					{
						R_CollectPenumbraEdges( entityDef, tri, lightDef, R_SoftPenumbraRadius( lightDef ),
												vEntity->modelMatrix, &sedges, &nedges );
					}
					tr.pc.softShadowMicroSec += Sys_Microseconds() - swCollectStart;
					R_SoftProxyProfile( entityDef, tri );
					extern int fe_softEdgesCollected;
					fe_softEdgesCollected += nedges;
					if( nedges > 0 )
					{
						drawSurf_t* edgeSurf = ( drawSurf_t* )R_FrameAlloc( sizeof( *edgeSurf ), FRAME_ALLOC_DRAW_SURFACE );
						memset( edgeSurf, 0, sizeof( *edgeSurf ) );
						edgeSurf->softEdges = sedges;
						edgeSurf->numSoftEdges = nedges;
						edgeSurf->softClusters = faceClusters;
						edgeSurf->numSoftClusters = nClusters;
						edgeSurf->frontEndGeo = tri;		// caster solid, for the scene-capture ground-truth mesh
						edgeSurf->space = vEntity;
						edgeSurf->softIsBox = swFaceIsBox;	// analytic box: flatten tags numTris<0 for the walk's FillBox
						edgeSurf->softWedgeEdges = wSedges;	// hybrid whitelist: wedge form riding beside the face stream
						edgeSurf->numSoftWedgeEdges = wNedges;
						edgeSurf->scissorRect = vLight->scissorRect;

						edgeSurf->linkChain = &vLight->softShadowWedges;
						edgeSurf->nextOnLight = vEntity->drawSurfs;
						vEntity->drawSurfs = edgeSurf;
					}
					}	// end else (triangle / silhouette path; hull path is the if above)
				}

				// PCSS LOCATOR ATLAS: the soft-wedge PCSS locator (r_shadowMapPCSS) samples the shadow ATLAS to
				// classify lit / penumbra / umbra, but the volume surf built above is material==NULL, which
				// ShadowMapPassFast skips - leaving the soft light's atlas EMPTY, so the locator reads far
				// everywhere and (with receiver depth beyond the far plane) false-shadows the whole frame. Also
				// build a real OCCLUDER surf (ambientCache + material, shadowCache 0) into globalShadows so the
				// atlas gets true occluder depth. The two passes over globalShadows separate cleanly by cache:
				// StencilShadowPass skips shadowCache==0 (this occluder), ShadowMapPassFast skips material==NULL
				// (the volume). Only needed when the atlas-locator is actually in use.
				extern idCVar r_useShadowAtlas;
				extern idCVar r_shadowMapPCSS;
				if( r_useShadowAtlas.GetBool() && r_shadowMapPCSS.GetBool() &&
						( surfInter == NULL || surfInter->lightTrisIndexCache > 0 ) )
				{
					drawSurf_t* occ = ( drawSurf_t* )R_FrameAlloc( sizeof( *occ ), FRAME_ALLOC_DRAW_SURFACE );
					memset( occ, 0, sizeof( *occ ) );
					if( surfInter != NULL )
					{
						occ->numIndexes = surfInter->numLightTrisIndexes;
						occ->indexCache = surfInter->lightTrisIndexCache;
					}
					else
					{
						if( !vertexCache.CacheIsCurrent( tri->indexCache ) )
						{
							tri->indexCache = vertexCache.AllocIndex( tri->indexes, tri->numIndexes );
						}
						occ->numIndexes = tri->numIndexes;
						occ->indexCache = tri->indexCache;
					}
					if( !vertexCache.CacheIsCurrent( tri->ambientCache ) )
					{
						if( shader->ReceivesLighting() && !tri->tangentsCalculated )
						{
							R_DeriveTangents( tri );
						}
						tri->ambientCache = vertexCache.AllocVertex( tri->verts, tri->numVerts );
					}
					if( occ->numIndexes > 0 && vertexCache.CacheIsCurrent( occ->indexCache ) && vertexCache.CacheIsCurrent( tri->ambientCache ) )
					{
						occ->ambientCache = tri->ambientCache;
						occ->shadowCache = 0;			// NOT a volume: StencilShadowPass skips shadowCache==0; the atlas draws it
						occ->frontEndGeo = tri;
						occ->space = vEntity;
						occ->material = shader;
						occ->scissorRect = vLight->scissorRect;
						occ->sort = 0.0f;
						if( shader->Coverage() == MC_PERFORATED )
						{
							R_SetupDrawSurfShader( occ, shader, renderEntity );
						}
						R_SetupDrawSurfJoints( occ, tri, shader );
						occ->linkChain = &vLight->globalShadows;
						occ->nextOnLight = vEntity->drawSurfs;
						vEntity->drawSurfs = occ;
						extern int fe_occludersBuilt;
						fe_occludersBuilt++;
					}
				}

				continue;	// stencil mode never uses the shadow-map occluder path
			}

			// RT shadows: put into the world TLAS EXACTLY the casters stencil shadows this light
			// from - gated on surfInter->numShadowIndexes > 0 (a precomputed shadow interaction
			// exists for THIS surface+light pair), the same criterion the stencil branch above uses.
			// This is the unification: same occluder set, so RT and stencil agree. Do NOT add every
			// shadow-casting surface unconditionally - that throws the whole level at each light
			// (surfaces it neither lights nor casts for become false occluders), which drowned the
			// flashlight in spurious shadow. Use the caster's FULL static geometry (frontEndGeo +
			// full ambient/index cache, NOT the light-frustum-culled lightTrisIndexCache subset,
			// whose missing triangles let shadow rays through -> cut-off shadows). AppendLightShadow-
			// Casters builds one BLAS per unique ambientCache and dedupes against the flood gather.
			// Static caches only: skinned/dynamic casters are handled by BuildSkinnedInstances /
			// out of M-scope (matches stencil, which also needs surfInter != NULL). Then skip the
			// shadow-map occluder path (RT visibility masks replace it).
			if( r_useRTShadows.GetBool() )
			{
				if( surfInter != NULL && surfInter->numShadowIndexes > 0 && tri->numIndexes > 0 &&
						vertexCache.CacheIsStatic( tri->ambientCache ) && vertexCache.CacheIsStatic( tri->indexCache ) )
				{
					drawSurf_t* shadowDrawSurf = ( drawSurf_t* )R_FrameAlloc( sizeof( *shadowDrawSurf ), FRAME_ALLOC_DRAW_SURFACE );
					shadowDrawSurf->numIndexes = tri->numIndexes;
					shadowDrawSurf->indexCache = tri->indexCache;
					shadowDrawSurf->ambientCache = tri->ambientCache;
					shadowDrawSurf->shadowCache = 0;
					shadowDrawSurf->jointCache = 0;
					shadowDrawSurf->frontEndGeo = tri;
					shadowDrawSurf->space = vEntity;
					shadowDrawSurf->material = shader;
					shadowDrawSurf->extraGLState = 0;
					shadowDrawSurf->scissorRect = vLight->scissorRect;
					shadowDrawSurf->sort = 0.0f;
					shadowDrawSurf->renderZFail = 0;
					shadowDrawSurf->shaderRegisters = NULL;

					// The shadow-MAP atlas pass still runs for these lights (RT does not yet
					// suppress it) and walks globalShadows: an alpha-tested caster reaches
					// ShadowMapPassPerforated, which dereferences shaderRegisters. Set the shader
					// (perforated) and joints exactly as the shadow-map occluder path below does,
					// or that pass segfaults on our bespoke drawSurf.
					if( shader->Coverage() == MC_PERFORATED )
					{
						R_SetupDrawSurfShader( shadowDrawSurf, shader, renderEntity );
					}
					R_SetupDrawSurfJoints( shadowDrawSurf, tri, shader );

					shadowDrawSurf->linkChain = &vLight->globalShadows;
					shadowDrawSurf->nextOnLight = vEntity->drawSurfs;
					vEntity->drawSurfs = shadowDrawSurf;
				}
				continue;
			}


			// RB: draw shadow occluder using shadow mapping
			// OPTIMIZE: check if projected occluder box intersects the view
			//
			//if( addInteractions && surfaceDirectlyVisible && shader->ReceivesLighting() )
			{
				// static interactions can commonly find that no triangles from a surface
				// contact the light, even when the total model does
				if( surfInter == NULL || surfInter->lightTrisIndexCache > 0 )
				{
					// create a drawSurf for this interaction
					drawSurf_t* shadowDrawSurf = ( drawSurf_t* )R_FrameAlloc( sizeof( *shadowDrawSurf ), FRAME_ALLOC_DRAW_SURFACE );

					if( surfInter != NULL )
					{
						// optimized static interaction
						shadowDrawSurf->numIndexes = surfInter->numLightTrisIndexes;
						shadowDrawSurf->indexCache = surfInter->lightTrisIndexCache;
					}
					else
					{
						// make sure we have an ambient cache and all necessary normals / tangents
						if( !vertexCache.CacheIsCurrent( tri->indexCache ) )
						{
							tri->indexCache = vertexCache.AllocIndex( tri->indexes, tri->numIndexes );
						}

						// throw the entire source surface at it without any per-triangle culling
						shadowDrawSurf->numIndexes = tri->numIndexes;
						shadowDrawSurf->indexCache = tri->indexCache;
					}

					if( !vertexCache.CacheIsCurrent( tri->ambientCache ) )
					{
						// we are going to use it for drawing, so make sure we have the tangents and normals
						if( shader->ReceivesLighting() && !tri->tangentsCalculated )
						{
							assert( tri->staticModelWithJoints == NULL );
							R_DeriveTangents( tri );

							// RB: this was hit by parametric particle models ..
							//assert( false );	// this should no longer be hit
							// RB end
						}
						tri->ambientCache = vertexCache.AllocVertex( tri->verts, tri->numVerts );
					}

					shadowDrawSurf->ambientCache = tri->ambientCache;
					shadowDrawSurf->frontEndGeo = tri;
					shadowDrawSurf->space = vEntity;
					shadowDrawSurf->material = shader;
					shadowDrawSurf->extraGLState = 0;
					shadowDrawSurf->scissorRect = vLight->scissorRect; // interactionScissor;
					shadowDrawSurf->sort = 0.0f;
					//shadowDrawSurf->shaderRegisters = baseDrawSurf->shaderRegisters; // TODO FIXME

					if( shader->Coverage() == MC_PERFORATED )
					{
						R_SetupDrawSurfShader( shadowDrawSurf, shader, renderEntity );
					}

					R_SetupDrawSurfJoints( shadowDrawSurf, tri, shader );

					// determine which linked list to add the shadow surface to

					//shadowDrawSurf->linkChain = shader->TestMaterialFlag( MF_NOSELFSHADOW ) ? &vLight->localShadows : &vLight->globalShadows;

					shadowDrawSurf->linkChain = &vLight->globalShadows;
					shadowDrawSurf->nextOnLight = vEntity->drawSurfs;

					vEntity->drawSurfs = shadowDrawSurf;

				}
			}
			// RB end
		}
	}
}

REGISTER_PARALLEL_JOB( R_AddSingleModel, "R_AddSingleModel" );

/*
=================
R_LinkDrawSurfToView

Als called directly by GuiModel
=================
*/
void R_LinkDrawSurfToView( drawSurf_t* drawSurf, viewDef_t* viewDef )
{
	// if it doesn't fit, resize the list
	if( viewDef->numDrawSurfs == viewDef->maxDrawSurfs )
	{
		drawSurf_t** old = viewDef->drawSurfs;
		int count;

		if( viewDef->maxDrawSurfs == 0 )
		{
			viewDef->maxDrawSurfs = INITIAL_DRAWSURFS;
			count = 0;
		}
		else
		{
			count = viewDef->maxDrawSurfs * sizeof( viewDef->drawSurfs[0] );
			viewDef->maxDrawSurfs *= 2;
		}
		viewDef->drawSurfs = ( drawSurf_t** )R_FrameAlloc( viewDef->maxDrawSurfs * sizeof( viewDef->drawSurfs[0] ), FRAME_ALLOC_DRAW_SURFACE_POINTER );
		memcpy( viewDef->drawSurfs, old, count );
	}

	viewDef->drawSurfs[viewDef->numDrawSurfs] = drawSurf;
	viewDef->numDrawSurfs++;
}

/*
===================
R_AddModels

The end result of running this is the addition of drawSurf_t to the
tr.viewDef->drawSurfs[] array and light link chains, along with
frameData and vertexCache allocations to support the drawSurfs.
===================
*/
void R_AddModels()
{
	SCOPED_PROFILE_EVENT( "R_AddModels" );

	// RB: already done in R_FillMaskedOcclusionBufferWithModels
	// tr.viewDef->viewEntitys = R_SortViewEntities( tr.viewDef->viewEntitys );

	// Diagnostic (r_softShadowDebugHash): the fingerprint must accumulate across ALL views of a frame
	// (there are typically 2: world + weapon), so reset/compare only at a FRAME boundary, not per
	// R_AddModels call. Comparing per-call would just alternate between the two views' partial sums.
	extern idSysInterlockedInteger tr_softWedgeGeomHash;
	extern idSysInterlockedInteger tr_softWedgeEdgeCount;
	extern idCVar r_softShadowDebugHash;
	if( r_softShadowDebugHash.GetInteger() >= 1 )
	{
		static int s_lastFrame = -1;
		static int s_prevTotal = 0;
		static int s_prevCount = 0;
		if( tr.frameCount != s_lastFrame )
		{
			const int total = tr_softWedgeGeomHash.GetValue();	// completed frame (all its views)
			const int count = tr_softWedgeEdgeCount.GetValue();
			if( s_lastFrame >= 0 && total != s_prevTotal )
			{
				// count delta => the SET of contributing edges changed (caster/light/view set);
				// count same => the per-edge WORLD values changed (pose/matrix), which is camera-independent
				// by construction and so should never move under a still scene + moving camera.
				common->Printf( "softwedge fingerprint DRIFT frame %i: %08x -> %08x  edges %i -> %i  (%s)\n",
								tr.frameCount, ( unsigned )s_prevTotal, ( unsigned )total, s_prevCount, count,
								count != s_prevCount ? "SET changed" : "VALUES changed" );
			}
			s_prevTotal = total;
			s_prevCount = count;
			tr_softWedgeGeomHash.SetValue( 0 );	// reset for the new frame
			tr_softWedgeEdgeCount.SetValue( 0 );
			s_lastFrame = tr.frameCount;
		}
	}

	//-------------------------------------------------
	// Go through each view entity that is either visible to the view, or to
	// any light that intersects the view (for shadows).
	//-------------------------------------------------

	if( r_useParallelAddModels.GetBool() )
	{
		for( viewEntity_t* vEntity = tr.viewDef->viewEntitys; vEntity != NULL; vEntity = vEntity->next )
		{
			tr.frontEndJobList->AddJob( ( jobRun_t )R_AddSingleModel, vEntity );
		}
		tr.frontEndJobList->Submit();
		tr.frontEndJobList->Wait();
	}
	else
	{
		for( viewEntity_t* vEntity = tr.viewDef->viewEntitys; vEntity != NULL; vEntity = vEntity->next )
		{
			R_AddSingleModel( vEntity );
		}
	}

	//-------------------------------------------------
	// Move the draw surfs to the view.
	//-------------------------------------------------

	tr.viewDef->numDrawSurfs = 0;	// clear the ambient surface list
	tr.viewDef->maxDrawSurfs = 0;	// will be set to INITIAL_DRAWSURFS on R_LinkDrawSurfToView

	for( viewEntity_t* vEntity = tr.viewDef->viewEntitys; vEntity != NULL; vEntity = vEntity->next )
	{
		// RB
		if( vEntity->drawSurfs != NULL )
		{
			tr.pc.c_visibleViewEntities++;
		}

		for( drawSurf_t* ds = vEntity->drawSurfs; ds != NULL; )
		{
			drawSurf_t* next = ds->nextOnLight;
			if( ds->linkChain == NULL )
			{
				R_LinkDrawSurfToView( ds, tr.viewDef );
			}
			else
			{
				ds->nextOnLight = *ds->linkChain;
				*ds->linkChain = ds;
			}
			ds = next;
		}

		vEntity->drawSurfs = NULL;
	}

	// Analytic soft shadows: flatten each light's per-caster silhouette edges into one contiguous
	// vertex-cache buffer. The interaction pixel shader loops these against the exact receiver world
	// position, evaluating penumbra coverage without any screen-space depth reconstruction.
	if( r_useSoftShadowVolumes.GetBool() )
	{
		// Hard per-frame budget so we can NEVER overflow the joint buffer (that overflow is a fatal
		// idLib::Error). softShadowEdge_t = 32 bytes; keep well under the 64MB joint buffer, leaving room
		// for skinning. Lights past the budget simply get no soft shadow this frame (approximation).
		const int SOFT_EDGE_FRAME_BUDGET = 400000;	// ~12.8 MB of edges, well under the joint buffer
		int edgesUsed = 0;
		const int swFlattenStart = Sys_Microseconds();

		extern idCVar r_softShadowFaceCoverage;
		const bool swFaceMode = r_softShadowFaceCoverage.GetBool();
		extern idCVar r_softShadowUmbraAccum;
		const bool swUmbraAccum = swFaceMode && r_softShadowUmbraAccum.GetBool();

		fe_softStaticCasters = fe_softDynCasters = 0;
		fe_softStaticRecords = fe_softDynRecords = 0;
		fe_softDynLightMoved = fe_softDynGeomMoved = 0;
		for( viewLight_t* vLight = tr.viewDef->viewLights; vLight != NULL; vLight = vLight->next )
		{
			vLight->softEdgeCache = 0;
			vLight->softEdgeCount = 0;
			vLight->softCasterCache = 0;
			vLight->softCasterCount = 0;
			vLight->softStaticCasterCount = 0;
			vLight->softStaticTriCount = 0;
			vLight->softSurfHash = 0;

			// CASTER-ORDER (r_softShadowDepthOrder): reorder softShadowWedges so the highest-impact casters
			// come first. The bin writes each tile list in stream order, so this makes the tile list
			// ~impact-ordered -> the walk's saturation/lit early-outs fire before the tail is visited.
			// Mode 1 = nearest-light first (legacy depth key); mode 2 = contribution order (angular size
			// from the light, descending). Caster-runs (contiguous same-space surfs) stay intact; only run
			// order changes. The walk's mask is a commutative UNION, so on its own this reorder is bit-exact.
			extern idCVar r_softShadowDepthOrder;
			const int swOrderMode = r_softShadowDepthOrder.GetInteger();
			if( swOrderMode != 0 && vLight->softShadowWedges != NULL && vLight->lightDef != NULL )
			{
				const idVec3 Lorg = vLight->lightDef->globalLightOrigin;
				struct SwRun { drawSurf_t* head; drawSurf_t* tail; float depth; };
				std::vector<SwRun> runs;
				for( drawSurf_t* s = vLight->softShadowWedges; s != NULL; )
				{
					const void* sp = s->space;
					float dep = 1e30f;
					if( s->softEdges != NULL && s->numSoftEdges > 0 )
					{
						const idVec4* e = ( const idVec4* )s->softEdges;
						dep = ( idVec3( e[0].x, e[0].y, e[0].z ) - Lorg ).LengthSqr();
					}
					SwRun run; run.head = s; run.tail = s;
					idBounds rb;
					rb.Clear();
					if( swOrderMode == 2 )
					{
						R_SoftAccumSampleBounds( ( const idVec4* )s->softEdges, s->numSoftEdges, rb );
					}
					while( s->nextOnLight != NULL && s->nextOnLight->space == sp )
					{
						s = s->nextOnLight; run.tail = s;
						if( swOrderMode == 2 )
						{
							R_SoftAccumSampleBounds( ( const idVec4* )s->softEdges, s->numSoftEdges, rb );
						}
					}
					// sort is ascending on .depth: mode 2 negates the score so biggest-first wins
					run.depth = ( swOrderMode == 2 ) ? -R_SoftAngularScore( rb, Lorg ) : dep;
					runs.push_back( run );
					s = s->nextOnLight;
				}
				std::sort( runs.begin(), runs.end(), []( const SwRun & a, const SwRun & b ) { return a.depth < b.depth; } );
				for( size_t i = 0; i < runs.size(); i++ )
				{
					runs[i].tail->nextOnLight = ( i + 1 < runs.size() ) ? runs[i + 1].head : NULL;
				}
				vLight->softShadowWedges = runs.empty() ? NULL : runs[0].head;
			}

			// CAMERA-INVARIANT static prefix (r_softShadowSurfCache): rebuild this light's caster chain so
			// its STATIC portion is the full cached set (key-sorted, deterministic) instead of only the
			// frustum/PVS subset the view collected this frame. Copies each static caster's world-space
			// wedge stream on first sight (see s_swStaticCasterCache), then re-emits the whole cached set
			// every frame - so idle camera sway no longer churns the prefix, the fingerprint stays put,
			// and the GPU cache stops wiping itself wholesale. The DYNAMIC runs pass through verbatim.
			{
				extern idCVar r_softShadowSurfCache, r_softShadowSurfCacheInvariant, r_softShadowContribCache;
				// The CONTRIBUTOR cache is a HARD dependent of the invariant prefix: its unions store
				// GLOBAL tri indices, so a view-culled (per-frame) static stream shifts every index
				// under camera motion and served unions FillTri arbitrary WRONG triangles. Played
				// live with surfCache 0 this was ants + banding + penumbra texel swimming at
				// gate-0-defects (every instrument was frozen-view or non-culling). Contrib must
				// never run without this block.
				if( ( ( r_softShadowSurfCache.GetBool() && r_softShadowSurfCacheInvariant.GetBool() )
						|| r_softShadowContribCache.GetInteger() != 0 ) && vLight->lightDef != NULL )
				{
					swStaticLightCache_t& sc = s_swStaticCasterCache[ vLight->lightDef->index ];

					// (1) split the collected chain into static (copied into the cache) vs dynamic (kept)
					drawSurf_t* dynHead = NULL;
					drawSurf_t* dynTail = NULL;
					for( drawSurf_t* s = vLight->softShadowWedges; s != NULL; )
					{
						const void* sp = s->space;
						drawSurf_t* runHead = s;
						drawSurf_t* runTail = s;
						while( runTail->nextOnLight != NULL && runTail->nextOnLight->space == sp ) { runTail = runTail->nextOnLight; }
						drawSurf_t* nextRun = runTail->nextOnLight;

						if( R_SoftCasterIsStatic( s->space->entityDef, vLight ) )
						{
							for( drawSurf_t* q = runHead; q != nextRun; q = q->nextOnLight )
							{
								if( q->softEdges == NULL || q->numSoftEdges <= 0 ) { continue; }
								const idVec4* e = ( const idVec4* )q->softEdges;
								const int eidx = q->space->entityDef != NULL ? q->space->entityDef->index : -1;
								const uint64_t vq = ( uint64_t )( int64_t )( e[0].x * 8.0f ) * 0x9E3779B97F4A7C15ull
													^ ( uint64_t )( int64_t )( e[0].y * 8.0f ) * 0x2545F4914F6CDD1Dull
													^ ( uint64_t )( int64_t )( e[0].z * 8.0f );
								const uint64_t key = ( ( uint64_t )( uint32_t )eidx << 32 ) ^ ( vq & 0xFFFFFFFFull );
								// PER-ENTITY UNIQUENESS (LINGER safety): a mover that paused gets copied at
								// pose A; pausing again at pose B mints a NEW content key - without this
								// purge the stale pose-A copy kept emitting as "static" forever (double
								// geometry: measured live serve-verify mismatches TRIPLED, texel swimming).
								// Non-world entities own exactly ONE cache entry; a different-key sibling is
								// the stale pose - erase it. World surfaces keep per-surface entries (one
								// entity, many keys - immobile, so the hazard cannot arise).
								const bool qIsWorld = q->space->entityDef != NULL && q->space->entityDef->parms.hModel != NULL
													  && q->space->entityDef->parms.hModel->IsStaticWorldModel();
								if( !qIsWorld && eidx >= 0 )
								{
									for( auto pit = sc.casters.begin(); pit != sc.casters.end(); )
									{
										if( pit->second.entityIndex == eidx && pit->second.key != key && !pit->second.isWorld )
										{
											pit = sc.casters.erase( pit );
										}
										else
										{
											++pit;
										}
									}
								}
								swStaticCaster_t& c = sc.casters[ key ];
								if( c.entityDef == NULL )		// first sight: copy the persistent stream
								{
									c.key = key;
									c.entityIndex = eidx;
									c.entityDef = q->space->entityDef;
									c.isWorld = qIsWorld;
									c.tris.assign( e, e + q->numSoftEdges );
									if( q->softClusters != NULL && q->numSoftClusters > 0 )
									{
										c.clusters.assign( q->softClusters, q->softClusters + q->numSoftClusters * 2 );
									}
								}
								c.lastSeenFrame = tr.frameCount;
							}
						}
						else	// dynamic run: keep verbatim - and PURGE the mover's stale static copies
						{
							// the entity is MOVING now: its cached "static" stream is a stale pose and
							// must leave the emission THIS frame, not after the 600-frame eviction
							const int didx = runHead->space->entityDef != NULL ? runHead->space->entityDef->index : -1;
							if( didx >= 0 )
							{
								for( auto pit = sc.casters.begin(); pit != sc.casters.end(); )
								{
									if( pit->second.entityIndex == didx && !pit->second.isWorld )
									{
										pit = sc.casters.erase( pit );
									}
									else
									{
										++pit;
									}
								}
							}
							runTail->nextOnLight = NULL;
							if( dynTail == NULL ) { dynHead = runHead; }
							else { dynTail->nextOnLight = runHead; }
							dynTail = runTail;
						}
						s = nextRun;
					}

					// (2) evict entries unseen for a while OR whose entity was freed/reused (the cached
					// entityDef pointer would dangle) - keeps the set bounded across a level walkthrough
					const idList<idRenderEntityLocal*, TAG_ENTITY>& edefs = vLight->lightDef->world->entityDefs;
					const int SW_STATIC_EVICT_FRAMES = 600;		// ~10 s: standing/looping an area never evicts
					for( auto it = sc.casters.begin(); it != sc.casters.end(); )
					{
						const int idx = it->second.entityIndex;
						const bool stale = tr.frameCount - it->second.lastSeenFrame > SW_STATIC_EVICT_FRAMES;
						const bool dangling = idx < 0 || idx >= edefs.Num() || edefs[idx] != it->second.entityDef;
						if( stale || dangling ) { it = sc.casters.erase( it ); }
						else { ++it; }
					}

					// (3) re-emit the whole cached set, key-sorted; one synth viewEntity per entityDef so
					// same-entity surfaces group as the view path did (world stays per-surface downstream)
					std::vector<const swStaticCaster_t*> ordered;
					ordered.reserve( sc.casters.size() );
					for( auto& kv : sc.casters ) { ordered.push_back( &kv.second ); }
					if( swOrderMode == 2 )
					{
						// contribution order, DESCENDING, key tie-break: score is a pure function of the
						// cached stream + light origin, so same set => same order every frame - the
						// determinism the contrib cache's global tri indices require, same as key order
						const idVec3 sLorg = vLight->lightDef->globalLightOrigin;
						std::vector<std::pair<float, const swStaticCaster_t*>> scored;
						scored.reserve( ordered.size() );
						for( const swStaticCaster_t* c : ordered )
						{
							idBounds b;
							b.Clear();
							R_SoftAccumSampleBounds( c->tris.data(), ( int )c->tris.size(), b );
							scored.emplace_back( R_SoftAngularScore( b, sLorg ), c );
						}
						std::sort( scored.begin(), scored.end(), []( const std::pair<float, const swStaticCaster_t*>& a, const std::pair<float, const swStaticCaster_t*>& b )
						{
							if( a.first != b.first )
							{
								return a.first > b.first;
							}
							return a.second->key < b.second->key;
						} );
						for( size_t i = 0; i < scored.size(); i++ ) { ordered[i] = scored[i].second; }
					}
					else
					{
						std::sort( ordered.begin(), ordered.end(), []( const swStaticCaster_t * a, const swStaticCaster_t * b )
						{
							return a->key < b->key;
						} );
					}

					std::unordered_map<idRenderEntityLocal*, viewEntity_t*> synthSpace;
					drawSurf_t* staticHead = NULL;
					drawSurf_t* staticTail = NULL;
					for( const swStaticCaster_t* c : ordered )
					{
						viewEntity_t*& vent = synthSpace[ c->entityDef ];
						if( vent == NULL )
						{
							vent = ( viewEntity_t* )R_ClearedFrameAlloc( sizeof( *vent ), FRAME_ALLOC_VIEW_ENTITY );
							vent->entityDef = c->entityDef;
							vent->index = c->entityIndex;
							vent->scissorRect = vLight->scissorRect;
						}
						drawSurf_t* ds = ( drawSurf_t* )R_FrameAlloc( sizeof( *ds ), FRAME_ALLOC_DRAW_SURFACE );
						memset( ds, 0, sizeof( *ds ) );
						ds->space = vent;
						ds->softEdges = ( softShadowEdge_t* )c->tris.data();
						ds->numSoftEdges = ( int )c->tris.size();
						ds->softClusters = c->clusters.empty() ? NULL : const_cast<idVec4*>( c->clusters.data() );
						ds->numSoftClusters = ( int )c->clusters.size() / 2;
						ds->scissorRect = vLight->scissorRect;
						if( staticTail == NULL ) { staticHead = ds; }
						else { staticTail->nextOnLight = ds; }
						staticTail = ds;
					}
					if( staticTail != NULL ) { staticTail->nextOnLight = dynHead; vLight->softShadowWedges = staticHead; }
					else { vLight->softShadowWedges = dynHead; }
				}
			}

			// SURFACE-FOLD CACHE (r_softShadowSurfCache, probe): order casters STATIC-FIRST so the static
			// set is a contiguous PREFIX of the caster table + tri stream - the cache's residual lists
			// store tri indices into that prefix and the dynamic remainder is walked exactly per frame.
			// The static prefix is additionally sorted by a content key (entity index, quantized first
			// vertex) so its emission order - and therefore the cached tri indices - is DETERMINISTIC
			// across frames even if the collection order wobbles with the camera; an order change would
			// otherwise silently scramble every cached residual list. The walk mask is a commutative
			// union, so on its own this reorder is bit-exact (same argument as the depth-order block
			// above; runs AFTER it, so the partition wins and depth order is moot with the cache on).
			// the CONTRIBUTOR cache needs the same static-first partition: without it the wedge chain
			// stays in collection order and the static prefix truncates at the first dynamic caster
			// (measured on the cap0007 bench with surf cache off: contrib serves saved almost nothing
			// because most static casters sat past the truncation point).
			extern idCVar r_softShadowSurfCache;
			extern idCVar r_softShadowContribCache;
			if( ( r_softShadowSurfCache.GetBool() || r_softShadowContribCache.GetInteger() != 0 )
					&& vLight->softShadowWedges != NULL && vLight->lightDef != NULL )
			{
				struct SwSRun { drawSurf_t* head; drawSurf_t* tail; bool isStatic; uint64_t key; float score; };
				std::vector<SwSRun> runs;
				for( drawSurf_t* s = vLight->softShadowWedges; s != NULL; )
				{
					const void* sp = s->space;
					SwSRun run;
					run.head = s;
					run.tail = s;
					run.isStatic = R_SoftCasterIsStatic( s->space->entityDef, vLight );
					run.score = 0.0f;
					uint64_t vq = 0;
					if( s->softEdges != NULL && s->numSoftEdges > 0 )
					{
						const idVec4* e = ( const idVec4* )s->softEdges;
						vq = ( uint64_t )( int64_t )( e[0].x * 8.0f ) * 0x9E3779B97F4A7C15ull
							 ^ ( uint64_t )( int64_t )( e[0].y * 8.0f ) * 0x2545F4914F6CDD1Dull
							 ^ ( uint64_t )( int64_t )( e[0].z * 8.0f );
					}
					run.key = ( ( uint64_t )( uint32_t )( s->space->entityDef != NULL ? s->space->entityDef->index : -1 ) << 32 ) ^ ( vq & 0xFFFFFFFFull );
					idBounds rb;
					rb.Clear();
					if( swOrderMode == 2 )
					{
						R_SoftAccumSampleBounds( ( const idVec4* )s->softEdges, s->numSoftEdges, rb );
					}
					while( s->nextOnLight != NULL && s->nextOnLight->space == sp )
					{
						s = s->nextOnLight;
						run.tail = s;
						if( swOrderMode == 2 )
						{
							R_SoftAccumSampleBounds( ( const idVec4* )s->softEdges, s->numSoftEdges, rb );
						}
					}
					if( swOrderMode == 2 )
					{
						run.score = R_SoftAngularScore( rb, vLight->lightDef->globalLightOrigin );
					}
					runs.push_back( run );
					s = s->nextOnLight;
				}
				std::stable_sort( runs.begin(), runs.end(), [swOrderMode]( const SwSRun & a, const SwSRun & b )
				{
					if( a.isStatic != b.isStatic )
					{
						return a.isStatic;    // static prefix first
					}
					if( !a.isStatic )
					{
						return false;	// dynamics keep their (possibly score-ordered) relative order
					}
					// static prefix: order must be DETERMINISTIC across frames (the caches store global
					// tri indices into it). Mode 2 = contribution desc with key tie-break (pure function
					// of stream + light origin, so as deterministic as the key order); else key order.
					if( swOrderMode == 2 && a.score != b.score )
					{
						return a.score > b.score;
					}
					return a.key < b.key;
				} );
				for( size_t i = 0; i < runs.size(); i++ )
				{
					runs[i].tail->nextOnLight = ( i + 1 < runs.size() ) ? runs[i + 1].head : NULL;
				}
				vLight->softShadowWedges = runs.empty() ? NULL : runs[0].head;
			}

			// Perf: prepend each caster's edge block with a HEADER record carrying the caster's
			// world-space bounding sphere, so the pixel shader can cheaply reject a whole caster that
			// cannot occlude a given fragment's light disk (a distant world surface then skips its
			// entire edge loop instead of an atan2 per edge). One header per caster; the shader marks
			// a header by e0.w < 0 (edges carry e0.w = silWeight >= 0). Records = edges + one header/caster.
			// One caster PER MODEL ENTITY, not per surface. A caster built from several surfaces (a tripod's
			// legs, a rock's faces) would otherwise become several casters, which the coverage shader unions by
			// max( occlusion ). At a joint no single surface occludes the whole light disk, so max() reports a
			// half-covered disk - a hairline lit crack running through solid umbra. Grouping every surface of
			// one entity under a single header makes the shader SUM their coverage integrals (each surface is a
			// separate closed chain; disjoint areas add, shared seams cancel), reconstructing the union. An
			// entity's edge surfaces are linked contiguously (all off one vEntity->drawSurfs), so a caster is a
			// run of consecutive surfaces with the same space.
			//
			// EXCEPTION: the static world model is a SINGLE space holding the whole level's surfaces, spread
			// far apart and not one coherent occluder. Grouping it would (a) give one caster a level-sized
			// bounding sphere so the shader-side cull never rejects it - every fragment loops every world edge
			// (huge perf hit) - and (b) SUM spatially-unrelated world surfaces, over-darkening large lit areas.
			// So each world surface stays its own caster (tight sphere, max-combine), exactly as before.
			int total = 0, numCasters = 0, totalClusters = 0;
			const void* prevSpace = NULL;
			for( const drawSurf_t* s = vLight->softShadowWedges; s != NULL; s = s->nextOnLight )
			{
				total += s->numSoftEdges;
				totalClusters += s->numSoftClusters;
				const bool isWorld = s->space->entityDef != NULL && s->space->entityDef->parms.hModel != NULL
									 && s->space->entityDef->parms.hModel->IsStaticWorldModel();
				const bool isNewCaster = isWorld || s->space != prevSpace;
				if( isNewCaster ) { numCasters++; prevSpace = s->space; }

				// Phase 0 attribution: classify this surface's records (and its caster) static vs dynamic.
				if( R_SoftCasterIsStatic( s->space->entityDef, vLight ) )
				{
					fe_softStaticRecords += s->numSoftEdges;
					if( isNewCaster ) { fe_softStaticCasters++; }
				}
				else
				{
					fe_softDynRecords += s->numSoftEdges;
					if( isNewCaster ) { fe_softDynCasters++; }
					if( vLight->lightHasMoved ) { fe_softDynLightMoved += s->numSoftEdges; }
					else { fe_softDynGeomMoved += s->numSoftEdges; }	// static light, moving/animated caster
				}
			}
			if( total <= 0 )
			{
				continue;	// no edges for this light
			}

			if( swFaceMode )
			{
				// ---- FACE stream v3: pure triangle stream (3 float4/tri) + caster table + CLUSTER table ----
				// No inline headers: the per-fragment walk loops the caster table (sphere cull, then the
				// caster's contiguous tri span) so the hot triangle loop carries no header branch, the
				// bin pass can pre-cull whole casters, and each triangle is one contiguous 48B load.
				// v3: each surface also carries a CLUSTER table (k-d spatial leaves of <=32 tris, built in
				// R_CollectPenumbraFaces); the flatten appends all cluster records RIGHT AFTER the caster
				// table in the SAME allocation, so consumers find them at casterBase + numCasters*2 with
				// no extra plumbing. Caster c1.zw = ( firstCluster, numClusters ) into that block; cluster
				// c1.x is rebased from surface-local to the light's tri stream here.
				// 'total' is already in float4 elements. Budget is bytes-equivalent to the wedge path's:
				// records are 32B, float4s are 16B.
				const int casterElems = numCasters * 2;
				const int clusterElems = totalClusters * 2;
				if( edgesUsed + ( total + casterElems + clusterElems + 1 ) / 2 > SOFT_EDGE_FRAME_BUDGET )
				{
					tr.pc.c_softShadowDroppedEdges += total;	// over budget -> this light gets no soft shadow
					continue;
				}
				edgesUsed += ( total + casterElems + clusterElems + 1 ) / 2;

				// ---- r_softShadowUmbraAccum: per-surface cull masks (cached; volatile surfaces excluded) ----
				// A surface participates only when its content hash matched last frame (statics; the bench's
				// reconstructed casters). The joint cull recomputes only when the STABLE set changes.
				std::vector<const std::vector<unsigned char>*> swMasks;
				if( swUmbraAccum && vLight->lightDef != NULL )
				{
					swUmbraLightCache_t& cache = s_swUmbraCache[vLight->lightDef->index];
					std::vector<std::pair<const drawSurf_t*, uint64_t>> surfKeys;
					for( const drawSurf_t* s = vLight->softShadowWedges; s != NULL; s = s->nextOnLight )
					{
						surfKeys.push_back( { s, SwUmbraSurfKey( ( const idVec4* )s->softEdges, s->numSoftEdges / 3 ) } );
					}
					extern idCVar r_softShadowUmbraAccumMargin;
					// cull against an INFLATED disk: only drop a triangle when even the enlarged disk is fully
					// blocked -> boundary slivers keep their shadow with margin (the gate's arbiter tolerance)
					const float swRlight = R_SoftPenumbraRadius( vLight->lightDef )
										   * Max( 1.0f, r_softShadowUmbraAccumMargin.GetFloat() );
					uint64_t comboKey = 0x9E3779B97F4A7C15ull ^ ( uint64_t )( int64_t )( swRlight * 64.0f );
					int nStable = 0;
					for( auto& sk : surfKeys )
					{
						if( cache.prevKeys.count( sk.second ) ) { comboKey ^= sk.second * 0x2545F4914F6CDD1Dull; nStable++; }
					}
					comboKey ^= ( uint64_t )nStable << 1;
					if( comboKey != cache.comboKey && nStable > 0 )
					{
						std::vector<swUmbraCullSurf_t> cullSurfs;
						for( auto& sk : surfKeys )
						{
							if( !cache.prevKeys.count( sk.second ) ) { continue; }
							cullSurfs.push_back( { sk.second, ( const idVec4* )sk.first->softEdges, sk.first->numSoftEdges / 3 } );
						}
						cache.masks.clear();
						SwUmbraCullCompute( cullSurfs, vLight->lightDef->globalLightOrigin, swRlight, cache.masks );
						cache.comboKey = comboKey;
						fe_softUmbraRecomputes++;
					}
					swMasks.reserve( surfKeys.size() );
					for( auto& sk : surfKeys )
					{
						const std::vector<unsigned char>* m = NULL;
						if( cache.prevKeys.count( sk.second ) )
						{
							auto it = cache.masks.find( sk.second );
							if( it != cache.masks.end() && ( int )it->second.size() == sk.first->numSoftEdges / 3 ) { m = &it->second; }
						}
						swMasks.push_back( m );
						cache.curKeys.insert( sk.second );
					}
					cache.prevKeys.swap( cache.curKeys );
					cache.curKeys.clear();
				}

				idVec4* triFlat = ( idVec4* )R_FrameAlloc( ( total + 1 ) * sizeof( idVec4 ), FRAME_ALLOC_UNKNOWN );	// +1: zero pad for the pair-based capture copy
				idVec4* casFlat = ( idVec4* )R_FrameAlloc( ( casterElems + clusterElems ) * sizeof( idVec4 ), FRAME_ALLOC_UNKNOWN );
				idVec4* cluFlat = casFlat + casterElems;		// cluster block rides after the casters
				int nElems = 0;
				int nCas = 0;
				int nClu = 0;
				const void* curSpace = NULL;
				int  openFirstTri = 0;
				int  openFirstClu = 0;
				bool casterOpen = false;
				int  swSurfIdx = 0;
				// surface-fold cache: the static-first prefix bookkeeping. nCasStatic/nStaticTris track the
				// contiguous static prefix as casters close; swSurfFold fingerprints the static surfaces in
				// emission order (order-SENSITIVE on purpose: the cached residual lists index this order).
				int  nCasStatic = 0;
				int  nStaticTris = 0;
				bool swSeenDynCaster = false;
				bool swCurCasterStatic = false;
				bool swCurCasterBox = false;			// analytic box caster: its 3 tri-slots are 8 box corners
				uint64_t swSurfFold = 0;
				idVec3 gmn( 1e30f, 1e30f, 1e30f ), gmx( -1e30f, -1e30f, -1e30f );
				for( const drawSurf_t* s = vLight->softShadowWedges; s != NULL; s = s->nextOnLight )
				{
					const bool isWorld = s->space->entityDef != NULL && s->space->entityDef->parms.hModel != NULL
										 && s->space->entityDef->parms.hModel->IsStaticWorldModel();
					if( isWorld || s->space != curSpace )	// world surfaces never group (each is its own caster)
					{
						if( casterOpen )
						{
							const idVec3 c = ( gmn + gmx ) * 0.5f;
							const float  rad = ( gmx - gmn ).Length() * 0.5f;
							casFlat[nCas * 2 + 0] = idVec4( c.x, c.y, c.z, rad );
							casFlat[nCas * 2 + 1] = idVec4( ( float )openFirstTri,
															swCurCasterBox ? -( float )( nElems / 3 - openFirstTri ) : ( float )( nElems / 3 - openFirstTri ),
															( float )openFirstClu, ( float )( nClu - openFirstClu ) );
							nCas++;
							if( swCurCasterStatic && !swSeenDynCaster )
							{
								nCasStatic = nCas;    // the static prefix grows while no dynamic caster closed yet
								nStaticTris = nElems / 3;
							}
						}
						openFirstTri = nElems / 3;
						openFirstClu = nClu;
						casterOpen = true;
						gmn.Set( 1e30f, 1e30f, 1e30f );
						gmx.Set( -1e30f, -1e30f, -1e30f );
						curSpace = s->space;
						swCurCasterStatic = R_SoftCasterIsStatic( s->space->entityDef, vLight );
						swCurCasterBox = false;			// reset for the new caster; OR'd across its surfaces below
						if( !swCurCasterStatic )
						{
							swSeenDynCaster = true;    // prefix ends: later statics (shouldn't happen post-sort) stay "dynamic" = exact walk
						}
						else if( !swSeenDynCaster && s->softEdges != NULL && s->numSoftEdges > 0 )
						{
							// fingerprint the static surface: entity + record count + quantized first vertex
							// (catches moves/content swaps; ORDER-sensitive so index scrambles drop the cache)
							const idVec4* he = ( const idVec4* )s->softEdges;
							uint64_t h = ( uint64_t )( uint32_t )( s->space->entityDef != NULL ? s->space->entityDef->index : -1 );
							h = h * 0x9E3779B97F4A7C15ull + ( uint64_t )s->numSoftEdges;
							h = h * 0x9E3779B97F4A7C15ull + ( uint64_t )( int64_t )( he[0].x * 8.0f ) + ( ( uint64_t )( int64_t )( he[0].y * 8.0f ) << 20 ) + ( ( uint64_t )( int64_t )( he[0].z * 8.0f ) << 40 );
							swSurfFold = swSurfFold * 0x2545F4914F6CDD1Dull + h;
						}
					}
					if( s->softIsBox ) { swCurCasterBox = true; }		// analytic box surf -> tag this caster
					const std::vector<unsigned char>* swM = ( swSurfIdx < ( int )swMasks.size() ) ? swMasks[swSurfIdx] : NULL;
					swSurfIdx++;
					if( swM != NULL && s->numSoftClusters > 0 )
					{
						// r_softShadowUmbraAccum: cluster-driven masked emit. Clusters partition the surface's
						// tris into contiguous runs in emission order (R_CollectPenumbraFaces), so skipping
						// culled tris keeps each cluster's kept subset contiguous; the rebuilt record keeps the
						// original sphere (conservative bound for a subset). Empty clusters are dropped.
						const idVec4* src = ( const idVec4* )s->softEdges;
						const int numTrisSurf = s->numSoftEdges / 3;
						for( int k = 0; k < s->numSoftClusters; k++ )
						{
							const idVec4 c0 = s->softClusters[k * 2 + 0];
							const idVec4 c1s = s->softClusters[k * 2 + 1];
							const int first = ( int )c1s.x, cnt = ( int )c1s.y;
							const int runStart = nElems / 3;
							for( int t = first; t < first + cnt && t < numTrisSurf; t++ )
							{
								if( ( *swM )[t] ) { fe_softUmbraCulledTris++; continue; }
								for( int e = 0; e < 3; e++ )
								{
									const idVec4& v = src[t * 3 + e];
									gmn.x = Min( gmn.x, v.x );	gmx.x = Max( gmx.x, v.x );
									gmn.y = Min( gmn.y, v.y );	gmx.y = Max( gmx.y, v.y );
									gmn.z = Min( gmn.z, v.z );	gmx.z = Max( gmx.z, v.z );
									triFlat[nElems++] = v;
								}
							}
							const int kept = nElems / 3 - runStart;
							if( kept > 0 )
							{
								cluFlat[nClu * 2 + 0] = c0;
								cluFlat[nClu * 2 + 1] = idVec4( ( float )runStart, ( float )kept, c1s.z, c1s.w );
								nClu++;
							}
						}
						continue;
					}
					// this surface's clusters, with firstTri rebased into the light's tri stream
					const int surfBaseTri = nElems / 3;
					for( int k = 0; k < s->numSoftClusters; k++ )
					{
						cluFlat[nClu * 2 + 0] = s->softClusters[k * 2 + 0];
						idVec4 c1 = s->softClusters[k * 2 + 1];
						c1.x += ( float )surfBaseTri;
						cluFlat[nClu * 2 + 1] = c1;
						nClu++;
					}
					const idVec4* src = ( const idVec4* )s->softEdges;
					for( int i = 0; i < s->numSoftEdges; i++ )
					{
						const idVec4& v = src[i];
						gmn.x = Min( gmn.x, v.x );	gmx.x = Max( gmx.x, v.x );	// every float4's xyz is a vertex; w (triRad/0) ignored
						gmn.y = Min( gmn.y, v.y );	gmx.y = Max( gmx.y, v.y );
						gmn.z = Min( gmn.z, v.z );	gmx.z = Max( gmx.z, v.z );
						triFlat[nElems++] = v;
					}
				}
				if( casterOpen )
				{
					const idVec3 c = ( gmn + gmx ) * 0.5f;
					const float  rad = ( gmx - gmn ).Length() * 0.5f;
					casFlat[nCas * 2 + 0] = idVec4( c.x, c.y, c.z, rad );
					casFlat[nCas * 2 + 1] = idVec4( ( float )openFirstTri,
													swCurCasterBox ? -( float )( nElems / 3 - openFirstTri ) : ( float )( nElems / 3 - openFirstTri ),
													( float )openFirstClu, ( float )( nClu - openFirstClu ) );
					nCas++;
					if( swCurCasterStatic && !swSeenDynCaster )
					{
						nCasStatic = nCas;
						nStaticTris = nElems / 3;
					}
				}

				// surface-fold cache: finalize the static-set fingerprint. Folding the emitted static
				// counts catches umbra-accum mask recomputes / content-count changes the per-surface
				// fold misses; folding the light origin + cache params drops the cache on any of them.
				{
					extern idCVar r_softShadowSurfCache, r_softShadowSurfCacheTexel, r_softShadowSurfCacheSecondThr;
					extern idCVar r_softShadowContribCache;
					// the contributor cache needs the same static-prefix count + fingerprint/generation
					// plumbing (its keys carry the generation; its serve walks the dynamic suffix)
					if( ( r_softShadowSurfCache.GetBool() || r_softShadowContribCache.GetInteger() != 0 ) && nCasStatic > 0 && nStaticTris > 0 )
					{
						// CAMERA-INVARIANT static-set fingerprint. The old hash folded the VIEW-CULLED static
						// counts (nCasStatic/nStaticTris/swSurfFold), so every camera move re-fingerprinted the
						// light -> generation bump -> after 64 bumps the whole table GC-wiped, and on a moving
						// cinematic the cache never stayed warm (measured: hit ~0%, one GC clear per run). Base
						// the fingerprint instead on the SORTED static interacting-entity indices from the
						// light's interaction chain (NOT view-culled) - the r_softShadowInvProbe below proved
						// this stays stable under camera motion. A static caster genuinely added/removed/settled
						// changes the set (R_SoftCasterIsStatic drops a moved one), so real changes still drop
						// the cache; pure camera motion no longer does.
						uint64_t h = 1469598103934665603ull;
						if( vLight->lightDef != NULL )
						{
							std::vector<int> statics;
							for( idInteraction* it = vLight->lightDef->firstInteraction; it != NULL; it = it->lightNext )
							{
								if( it->IsEmpty() || it->IsDeferred() || it->entityDef == NULL ) { continue; }
								if( !R_SoftCasterIsStatic( it->entityDef, vLight ) ) { continue; }
								statics.push_back( it->entityDef->index );
							}
							std::sort( statics.begin(), statics.end() );
							for( int idx : statics ) { h = ( h ^ ( uint64_t )idx ) * 1099511628211ull; }
						}
						h = h * 0x9E3779B97F4A7C15ull + ( uint64_t )( int64_t )( vLight->globalLightOrigin.x * 8.0f )
							+ ( ( uint64_t )( int64_t )( vLight->globalLightOrigin.y * 8.0f ) << 20 )
							+ ( ( uint64_t )( int64_t )( vLight->globalLightOrigin.z * 8.0f ) << 40 );
						h = h * 0x9E3779B97F4A7C15ull + ( uint64_t )( int64_t )( R_SoftPenumbraRadius( vLight->lightDef ) * 64.0f );
						h = h * 0x9E3779B97F4A7C15ull + ( uint64_t )r_softShadowSurfCacheTexel.GetInteger();
						h = h * 0x9E3779B97F4A7C15ull + ( uint64_t )( int64_t )( r_softShadowSurfCacheSecondThr.GetFloat() * 4096.0f );
						if( h == 0 )
						{
							h = 1;
						}
						vLight->softSurfHash = h;
						vLight->softStaticCasterCount = nCasStatic;
						vLight->softStaticTriCount = nStaticTris;

						// CAMERA-INVARIANCE PROBE (r_rtAccelDebug): the view-culled softSurfHash (h) churns as
						// the camera moves; a CAMERA-INVARIANT fingerprint of this light's STATIC caster set -
						// the SORTED set of static interacting entity indices from the interaction chain (not
						// view-culled) - should stay stable if nothing actually moves. If invariant is stable
						// while h churns, camera-independent collection is the sound fix. Diagnostic only.
						extern idCVar r_softShadowInvProbe;
						if( r_softShadowInvProbe.GetBool() && vLight->lightDef != NULL )
						{
							static std::unordered_map<int, uint64_t> swProbeH, swProbeI;
							static std::unordered_map<int, int> swChurnH, swChurnI;
							std::vector<int> statics;
							int nMovingCasters = 0;			// casters MODIFIED this frame (the shakers) - excluded from static
							const char* firstMover = "";
							for( idInteraction* it = vLight->lightDef->firstInteraction; it != NULL; it = it->lightNext )
							{
								if( it->IsEmpty() || it->IsDeferred() || it->entityDef == NULL ) { continue; }
								idRenderEntityLocal* e = it->entityDef;
								const bool castsShadow = e->parms.hModel != NULL && e->parms.hModel->ModelHasShadowCastingSurfaces();
								if( castsShadow && e->lastModifiedFrameNum == tr.frameCount )
								{
									nMovingCasters++;
									if( firstMover[0] == '\0' && e->parms.hModel != NULL ) { firstMover = e->parms.hModel->Name(); }
								}
								if( !R_SoftCasterIsStatic( e, vLight ) ) { continue; }
								statics.push_back( e->index );
							}
							std::sort( statics.begin(), statics.end() );
							uint64_t inv = 1469598103934665603ull;
							for( int idx : statics ) { inv = ( inv ^ ( uint64_t )idx ) * 1099511628211ull; }
							const int li = vLight->lightDef->index;
							const bool hadH = swProbeH.count( li ) != 0;
							const uint64_t pH = swProbeH[li], pI = swProbeI[li];
							if( hadH && pH != h ) { swChurnH[li]++; }
							if( hadH && pI != inv ) { swChurnI[li]++; }
							// DECOMPOSE which fingerprint component churns: surface-fold (set/order),
							// counts (nCasStatic/nStaticTris), or the quantized light origin.
							static std::unordered_map<int, uint64_t> swPrevFold, swPrevCounts, swPrevOrg;
							const uint64_t curFold = swSurfFold;
							const uint64_t curCounts = ( ( uint64_t )nCasStatic << 32 ) | ( uint64_t )( uint32_t )nStaticTris;
							const uint64_t curOrg = ( uint64_t )( int64_t )( vLight->globalLightOrigin.x * 8.0f )
													^ ( ( uint64_t )( int64_t )( vLight->globalLightOrigin.y * 8.0f ) << 21 )
													^ ( ( uint64_t )( int64_t )( vLight->globalLightOrigin.z * 8.0f ) << 42 );
							const bool foldCh = hadH && swPrevFold[li] != curFold;
							const bool cntCh = hadH && swPrevCounts[li] != curCounts;
							const bool orgCh = hadH && swPrevOrg[li] != curOrg;
							swPrevFold[li] = curFold; swPrevCounts[li] = curCounts; swPrevOrg[li] = curOrg;
							// throttle: accumulate churn every frame but PRINT at most once per second (one
							// burst per second, one line per light that changed since the last tick).
							static int swProbeTickMs = 0, swProbeTickFrame = -1;
							static bool swProbeAllow = false;
							if( swProbeTickFrame != tr.frameCount )
							{
								const int nowMs = Sys_Milliseconds();
								swProbeAllow = ( nowMs - swProbeTickMs >= 1000 );
								if( swProbeAllow ) { swProbeTickMs = nowMs; }
								swProbeTickFrame = tr.frameCount;
							}
							if( swProbeAllow && ( !hadH || pH != h || pI != inv || nMovingCasters > 0 || vLight->lightHasMoved ) )
							{
								common->Printf( "[surfinv] light %d: viewHash churns=%d [fold=%d cnt=%d org=%d] | INVARIANT churns=%d | %d static | lightHasMoved=%d | %d MOVING%s%s\n",
												li, swChurnH[li], foldCh ? 1 : 0, cntCh ? 1 : 0, orgCh ? 1 : 0,
												swChurnI[li], ( int )statics.size(), vLight->lightHasMoved ? 1 : 0,
												nMovingCasters, nMovingCasters > 0 ? " first=" : "", firstMover );
							}
							swProbeH[li] = h; swProbeI[li] = inv;
						}
					}
				}

				// EMITTED counts (== the pre-pass totals when r_softShadowUmbraAccum is off - byte-identical
				// path; smaller when the cull dropped tris/clusters). The cluster block still starts at
				// casterElems = numCasters*2 (casters are never dropped), matching softtile_bin's
				// casterBase + numCasters*2, so consumers need no change.
				vLight->softEdgeCache = vertexCache.AllocJoint( triFlat, nElems, sizeof( idVec4 ) );
				vLight->softEdgeCount = nElems;					// FACE mode: count in FLOAT4 elements
				vLight->softCasterCache = vertexCache.AllocJoint( casFlat, casterElems + nClu * 2, sizeof( idVec4 ) );
				vLight->softCasterCount = nCas;

				// HYBRID WHITELIST wedge block: a SECOND pass over the same chain with the SAME caster-grouping
				// condition, so wedge caster k IS caster-table entry k (the correspondence the term CS's
				// per-caster dispatch needs). Layout = the legacy inline-header wedge stream (header pair +
				// edge pairs); the header sphere is COPIED from casFlat so cull decisions agree between the
				// two walks. Casters with no wedge form (hull/box/none collected) emit an edgeCount-0 header:
				// alignment holds and the term CS treats them as scanline-only.
				{
					extern idCVar r_softShadowWedgeWhitelist;
					if( r_softShadowWedgeWhitelist.GetBool() && nCas > 0 )
					{
						int wRecords = nCas;						// one header pair per caster
						for( const drawSurf_t* s = vLight->softShadowWedges; s != NULL; s = s->nextOnLight )
						{
							wRecords += s->numSoftWedgeEdges;
						}
						if( edgesUsed + wRecords <= SOFT_EDGE_FRAME_BUDGET )
						{
							edgesUsed += wRecords;
							softShadowEdge_t* wFlat = ( softShadowEdge_t* )R_FrameAlloc( wRecords * sizeof( softShadowEdge_t ), FRAME_ALLOC_UNKNOWN );
							int wN = 0;
							int wCas = 0;
							int wHeaderIdx = -1;
							const void* wCurSpace = NULL;
							for( const drawSurf_t* s = vLight->softShadowWedges; s != NULL; s = s->nextOnLight )
							{
								const bool wIsWorld = s->space->entityDef != NULL && s->space->entityDef->parms.hModel != NULL
													  && s->space->entityDef->parms.hModel->IsStaticWorldModel();
								if( wIsWorld || s->space != wCurSpace )	// EXACT mirror of the caster-table grouping
								{
									if( wHeaderIdx >= 0 )
									{
										wFlat[wHeaderIdx].e1.y = ( float )( wN - wHeaderIdx - 1 );	// backfill edgeCount
									}
									wHeaderIdx = wN++;
									// header sphere = the caster table's (bit-identical cull between the walks)
									const idVec4& wc0 = casFlat[wCas * 2 + 0];
									wFlat[wHeaderIdx].e0 = idVec4( wc0.x, wc0.y, wc0.z, -1.0f );
									wFlat[wHeaderIdx].e1 = idVec4( wc0.w, 0.0f, 0.0f, ( float )wCas );
									wCas++;
									wCurSpace = s->space;
								}
								for( int i = 0; i < s->numSoftWedgeEdges; i++ )
								{
									wFlat[wN++] = s->softWedgeEdges[i];
								}
							}
							if( wHeaderIdx >= 0 )
							{
								wFlat[wHeaderIdx].e1.y = ( float )( wN - wHeaderIdx - 1 );
							}
							if( wCas == nCas )		// alignment is the whole contract: on mismatch drop the block
							{
								vLight->softWedgeCache = vertexCache.AllocJoint( wFlat, wN, sizeof( softShadowEdge_t ) );
								vLight->softWedgeCount = wN;
							}
							else
							{
								edgesUsed -= wRecords;
								common->Printf( "[wedgewl] light caster mismatch (wedge %d vs table %d) - block dropped\n", wCas, nCas );
							}
						}
						else
						{
							tr.pc.c_softShadowDroppedEdges += wRecords;	// budget: drop ONLY the wedge block (scanline unaffected)
						}
					}
				}

				// WORLD-CELL LIT CLASSIFIER (r_softShadowClassify, M1): build a dense lit/penumbra class grid
				// over the light's world bounds from the just-assembled world tri stream (triFlat = 3 float4/
				// tri), pack it into the joint buffer (read via t_SoftEdges in the term CS), and record the
				// grid params so the term CS can skip provably-lit fragments. Rebuilt every frame in M1.
				vLight->softClassifyDims[3] = 0;			// default: no classifier -> full walk
				extern idCVar r_softShadowClassify, r_softShadowClassifyCell;
				if( r_softShadowClassify.GetBool() && nElems >= 3 )
				{
					const idBounds& lb = vLight->globalLightBounds;		// light world influence AABB (snapshot)
					const float dmn[3] = { lb[0].x, lb[0].y, lb[0].z };
					const float dmx[3] = { lb[1].x, lb[1].y, lb[1].z };
					const float lo[3]  = { vLight->globalLightOrigin.x, vLight->globalLightOrigin.y, vLight->globalLightOrigin.z };
					static idList<idVec4> swClsPacked;		// frontend is single-threaded per view; reused scratch
					extern idCVar r_rtAccelDebug;
					softClassifyGrid_t g;
					if( R_SoftClassifyBuild( lo, R_SoftPenumbraRadius( vLight->lightDef ), dmn, dmx,
											 ( const idVec4* )triFlat, nElems / 3,
											 Max( 1.0f, ( float )r_softShadowClassifyCell.GetInteger() ), swClsPacked, g ) )
					{
						vLight->softClassifyCache = vertexCache.AllocJoint( swClsPacked.Ptr(), swClsPacked.Num(), sizeof( idVec4 ) );
						vLight->softClassifyAabbCell[0] = g.aabbMin[0];	vLight->softClassifyAabbCell[1] = g.aabbMin[1];
						vLight->softClassifyAabbCell[2] = g.aabbMin[2];	vLight->softClassifyAabbCell[3] = g.cellSize;
						vLight->softClassifyDims[0] = g.dims[0];	vLight->softClassifyDims[1] = g.dims[1];
						vLight->softClassifyDims[2] = g.dims[2];	vLight->softClassifyDims[3] = 1;
						if( r_rtAccelDebug.GetBool() )
						{
							common->Printf( "SoftClassify: dims %dx%dx%d cell %.0f | lit %d pen %d umbra %d (%.0f%% lit, %.0f%% umbra) | tris %d aabb (%.0f %.0f %.0f)-(%.0f %.0f %.0f)\n",
											g.dims[0], g.dims[1], g.dims[2], g.cellSize, g.nLit, g.nPen - g.nUmbra, g.nUmbra,
											100.0f * g.nLit / Max( 1, g.nLit + g.nPen ),
											100.0f * g.nUmbra / Max( 1, g.nLit + g.nPen ), nElems / 3,
											dmn[0], dmn[1], dmn[2], dmx[0], dmx[1], dmx[2] );
						}
					}
					else if( r_rtAccelDebug.GetBool() )
					{
						common->Printf( "SoftClassify: build FAILED (budget/degenerate) tris %d\n", nElems / 3 );
					}
				}

				if( R_SoftShadowCaptureArmed() )
				{
					triFlat[nElems].Zero();						// pad the odd tail for the pair-based copy
					R_CaptureLightEdges( vLight, ( const softShadowEdge_t* )triFlat, ( nElems + 1 ) / 2 );
				}

				tr.pc.c_softShadowLights++;
				tr.pc.c_softShadowCasters += nCas;
				tr.pc.c_softShadowEdges += nElems;
				tr.pc.c_softShadowMaxEdgesPerLight = Max( tr.pc.c_softShadowMaxEdgesPerLight, nElems );
				continue;
			}

			// ---- legacy WEDGE stream: inline caster headers + silhouette edge records ----
			const int records = total + numCasters;
			if( edgesUsed + records > SOFT_EDGE_FRAME_BUDGET )
			{
				tr.pc.c_softShadowDroppedEdges += total;	// over budget -> this light gets no soft shadow
				continue;
			}
			edgesUsed += records;

			softShadowEdge_t* flat = ( softShadowEdge_t* )R_FrameAlloc( records * sizeof( softShadowEdge_t ), FRAME_ALLOC_UNKNOWN );
			int n = 0;
			float casterId = 0.0f;	// tag each caster's edges so the shader can group + combine per caster
			const void* curSpace = NULL;
			int   headerIdx = -1;			// record index of the current entity's header (reserved, backfilled on close)
			idVec3 gmn( 1e30f, 1e30f, 1e30f ), gmx( -1e30f, -1e30f, -1e30f );	// entity bounding box (for the sphere)
			// WEDGE TILE-BIN caster table (2 float4/caster): ( centre.xyz, radius ), ( headerRecIdx, -edgeCount, 0, 0 ).
			// numTris = -edgeCount < 0 tags it ANALYTIC so softtile_bin appends ONE tile entry per surviving caster
			// (its sphere-cone cull, reused verbatim); the wedge PS then walks only the tile's casters' edge spans.
			std::vector<idVec4> swCasterTbl;
			swCasterTbl.reserve( ( size_t )numCasters * 2 );
			for( const drawSurf_t* s = vLight->softShadowWedges; s != NULL; s = s->nextOnLight )
			{
				const bool isWorld = s->space->entityDef != NULL && s->space->entityDef->parms.hModel != NULL
									 && s->space->entityDef->parms.hModel->IsStaticWorldModel();
				if( isWorld || s->space != curSpace )	// world surfaces never group (each is its own caster)
				{
					if( headerIdx >= 0 )	// close the previous entity: its sphere is now known
					{
						const idVec3 c = ( gmn + gmx ) * 0.5f;
						const float  rad = ( gmx - gmn ).Length() * 0.5f;
						flat[headerIdx].e0 = idVec4( c.x, c.y, c.z, -1.0f );		// e0 = ( centre, -1 marker )
						flat[headerIdx].e1 = idVec4( rad, ( float )( n - headerIdx - 1 ), 0.0f, casterId );	// e1 = ( radius, edgeCount, 0, casterId )
						swCasterTbl.push_back( idVec4( c.x, c.y, c.z, rad ) );
						swCasterTbl.push_back( idVec4( ( float )headerIdx, -( float )( n - headerIdx - 1 ), 0.0f, 0.0f ) );	// c1.x=headerRec, c1.y=-edgeCount (analytic tag)
						casterId += 1.0f;
					}
					headerIdx = n++;		// reserve this entity's header slot
					gmn.Set( 1e30f, 1e30f, 1e30f );
					gmx.Set( -1e30f, -1e30f, -1e30f );
					curSpace = s->space;
				}

				for( int i = 0; i < s->numSoftEdges; i++ )
				{
					const idVec4& e0 = s->softEdges[i].e0;
					const idVec4& e1 = s->softEdges[i].e1;
					gmn.x = Min( gmn.x, Min( e0.x, e1.x ) );	gmx.x = Max( gmx.x, Max( e0.x, e1.x ) );
					gmn.y = Min( gmn.y, Min( e0.y, e1.y ) );	gmx.y = Max( gmx.y, Max( e0.y, e1.y ) );
					gmn.z = Min( gmn.z, Min( e0.z, e1.z ) );	gmx.z = Max( gmx.z, Max( e0.z, e1.z ) );

					flat[n] = s->softEdges[i];
					// e1.w carries this caster's HEADER record index so the band prepass VS (softband.vs.hlsl)
					// can fetch the caster's bounding-sphere centre for the near/far volume caps. The coverage
					// pixel shader detects caster boundaries by the e0.w<0 header, not by e1.w, so this is free.
					flat[n].e1.w = ( float )headerIdx;
					n++;
				}
			}
			if( headerIdx >= 0 )	// close the final entity
			{
				const idVec3 c = ( gmn + gmx ) * 0.5f;
				const float  rad = ( gmx - gmn ).Length() * 0.5f;
				flat[headerIdx].e0 = idVec4( c.x, c.y, c.z, -1.0f );
				flat[headerIdx].e1 = idVec4( rad, ( float )( n - headerIdx - 1 ), 0.0f, casterId );	// e1.y = edgeCount (shader jump-skip)
					swCasterTbl.push_back( idVec4( c.x, c.y, c.z, rad ) );
					swCasterTbl.push_back( idVec4( ( float )headerIdx, -( float )( n - headerIdx - 1 ), 0.0f, 0.0f ) );
				casterId += 1.0f;
			}
			// AllocJoint (not AllocVertex): the joint buffer is the SRV-capable StructuredBuffer the
			// interaction pixel shader can read; the vertex buffer is not bound as an SRV.
			vLight->softEdgeCache = vertexCache.AllocJoint( flat, records, sizeof( softShadowEdge_t ) );
			vLight->softEdgeCount = records;
			// WEDGE TILE-BIN: publish the per-caster sphere table so the backend can run softtile_bin over the
			// wedge's casters (analytic-tagged) and the PS walks only each tile's casters. 0 casters => no table.
			if( !swCasterTbl.empty() )
			{
				vLight->softCasterCache = vertexCache.AllocJoint( swCasterTbl.data(), ( int )swCasterTbl.size(), sizeof( idVec4 ) );
				vLight->softCasterCount = numCasters;
			}

			if( R_SoftShadowCaptureArmed() )
			{
				R_CaptureLightEdges( vLight, flat, records );	// retain a CPU copy before flat[] is dropped
			}

			tr.pc.c_softShadowLights++;
			tr.pc.c_softShadowCasters += numCasters;
			tr.pc.c_softShadowEdges += total;
			tr.pc.c_softShadowMaxEdgesPerLight = Max( tr.pc.c_softShadowMaxEdgesPerLight, total );
		}

		tr.pc.softShadowMicroSec += Sys_Microseconds() - swFlattenStart;
	}
}
