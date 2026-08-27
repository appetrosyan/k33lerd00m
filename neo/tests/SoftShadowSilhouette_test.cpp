/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 2026 Aleksandr Petrosyan

This file is part of the Doom 3 BFG Edition GPL Source Code.

===========================================================================
*/

// SILHOUETTE-vs-TRUTH error surface + gate predicate.
//
// The shipped soft path is per-fragment triangle face-coverage (Fubini/scanline union). An OFF alternative,
// SoftShadow_WedgeOcclusion (softwedge_coverage.inc.hlsl ~:720), sums per-silhouette-EDGE circle-triangle
// signed areas around ONE loop computed from the light CENTRE - order-of-magnitude fewer primitives
// (~perimeter vs ~area). tr_frontend_addmodels.cpp:59/:2257 CLAIM this "undershoots off-axis". This test
// MEASURES that claim: it sweeps occluder shape x light radius x receiver off-axis angle x occluder distance,
// computes silhouette coverage (the SHIPPED SoftShadow_WedgeOcclusion, compiled as C++) against an
// independent ray-cast area-light truth (TruthShadow machinery), records the SIGNED error surface, names the
// driving variable of the failure region, and derives a cheap conservative gate that selects silhouette only
// where the measured error is under a tight epsilon (Fubini elsewhere). It also cross-checks the shipped
// face-coverage path so the 5% (silhouette bad, face good) is visible. This is a measurement instrument:
// registered as a STUDY so it PRINTS the surface, plus a hard assertion that the derived gate holds.

#include "hlsl_compat.h"

// Compile the live coverage source as C++, isolated in a namespace so it does not ODR-collide with the other
// test TUs that include the same shader. Mirror SoftShadowFillBox's self-contained namespaced pattern.
namespace swsil
{
#include "softwedge_coverage.inc.hlsl"
swAttrib_t g_swAttrib = {};
bool       g_swAttribOn = false;
}
using namespace swsil;			// bring the shader symbols into scope BEFORE SoftShadowBox.h parses its callers

#include "SoftShadowBox.h"
#include "idUnitTest.h"

#include <vector>
#include <array>
#include <map>
#include <cmath>
#include <cstdio>

using namespace swtest;

namespace
{

// ---------------------------------------------------------------------------------------------------
// General polygon-face mesh + light-relative silhouette. Mirrors SoftShadowBox.h::Silhouette (same
// front-face-boundary walk) but for arbitrary convex-polygon faces and returning MULTIPLE loops, so a
// concave prism yields one concave loop and two disjoint boxes yield two loops. Faces MUST be wound CCW
// as seen from OUTSIDE (Newell normal then points outward). Verts are shared by index across faces, so a
// closed manifold has every undirected edge in exactly two faces (the adjacency the silhouette needs).
struct PolyMesh
{
	std::vector<float3>            v;
	std::vector<std::vector<int>> faces;
};

float3 NewellNormal( const PolyMesh& m, const std::vector<int>& f )
{
	float3 n( 0, 0, 0 );
	int k = ( int )f.size();
	for( int i = 0; i < k; i++ )
	{
		float3 a = m.v[f[i]], b = m.v[f[( i + 1 ) % k]];
		n.x += ( a.y - b.y ) * ( a.z + b.z );
		n.y += ( a.z - b.z ) * ( a.x + b.x );
		n.z += ( a.x - b.x ) * ( a.y + b.y );
	}
	return n;
}
float3 FaceCentroid( const PolyMesh& m, const std::vector<int>& f )
{
	float3 c( 0, 0, 0 );
	for( int i : f ) { c = c + m.v[i]; }
	return c * ( 1.0f / ( float )f.size() );
}

// silhouette loops seen from viewpoint W (the LIGHT), oriented by the front-face boundary. Returns a set of
// closed loops of world vertices (empty if W is inside / no boundary).
std::vector<std::vector<float3>> SilhouetteMesh( const PolyMesh& m, float3 W )
{
	const int nf = ( int )m.faces.size();
	std::vector<bool> front( nf );
	for( int i = 0; i < nf; i++ )
	{
		front[i] = dot( NewellNormal( m, m.faces[i] ), W - FaceCentroid( m, m.faces[i] ) ) > 0.0f;
	}
	// undirected edge -> the (up to two) faces that own it
	std::map<std::pair<int, int>, std::vector<int>> em;
	for( int i = 0; i < nf; i++ )
	{
		const auto& f = m.faces[i];
		int k = ( int )f.size();
		for( int e = 0; e < k; e++ )
		{
			int a = f[e], c = f[( e + 1 ) % k];
			em[ { std::min( a, c ), std::max( a, c ) } ].push_back( i );
		}
	}
	// directed silhouette edges: a front face's edge whose neighbour is back-facing (or a boundary edge)
	std::vector<std::pair<int, int>> dir;
	for( int i = 0; i < nf; i++ )
	{
		if( !front[i] ) { continue; }
		const auto& f = m.faces[i];
		int k = ( int )f.size();
		for( int e = 0; e < k; e++ )
		{
			int a = f[e], c = f[( e + 1 ) % k];
			auto& fs = em[ { std::min( a, c ), std::max( a, c ) } ];
			int o = ( ( int )fs.size() < 2 ) ? -1 : ( fs[0] == i ? fs[1] : fs[0] );
			if( o < 0 || !front[o] ) { dir.push_back( { a, c } ); }
		}
	}
	// chain directed edges into closed loops greedily (first==prev.second)
	std::vector<std::vector<float3>> loops;
	std::vector<bool> used( dir.size(), false );
	for( size_t start = 0; start < dir.size(); start++ )
	{
		if( used[start] ) { continue; }
		std::vector<float3> loop;
		int s0 = dir[start].first, v = dir[start].second;
		used[start] = true;
		loop.push_back( m.v[s0] );
		for( size_t guard = 0; guard < dir.size(); guard++ )
		{
			loop.push_back( m.v[v] );
			if( v == s0 ) { loop.pop_back(); break; }		// closed
			int nx = -1;
			for( size_t k = 0; k < dir.size(); k++ )
			{
				if( !used[k] && dir[k].first == v ) { nx = ( int )k; break; }
			}
			if( nx < 0 ) { break; }							// open chain (non-manifold): keep what we have
			used[nx] = true;
			v = dir[nx].second;
		}
		if( loop.size() >= 3 ) { loops.push_back( loop ); }
	}
	return loops;
}

// box (SoftShadowBox.h) -> PolyMesh faces appended into m (shared vert list, outward winding preserved).
void AppendBox( PolyMesh& m, const Box& b )
{
	int base = ( int )m.v.size();
	for( int i = 0; i < 8; i++ ) { m.v.push_back( b.c[i] ); }
	for( int f = 0; f < 6; f++ )
	{
		m.faces.push_back( { base + b.f[f][0], base + b.f[f][1], base + b.f[f][2], base + b.f[f][3] } );
	}
}

// ---------------------------------------------------------------------------------------------------
// Occluder shapes. Each is BOTH a set of Boxes (for the independent ray truth: a ray is blocked if it hits
// ANY box; boxes tile the solid, so the union ray query is exact) AND a PolyMesh (for the light silhouette).
// For the L-notch the PolyMesh is the TRUE concave hexagonal prism (one merged manifold, no internal face),
// so the silhouette is the correct single concave loop - i.e. concavity is tested with a CORRECT loop, not
// a double-counted union of two box loops.
enum Shape { CONVEX, LNOTCH, DISJOINT };
const char* ShapeName( Shape s ) { return s == CONVEX ? "convex" : ( s == LNOTCH ? "Lnotch" : "disjoint" ); }

struct Occluder
{
	std::vector<Box> boxes;		// truth
	PolyMesh         mesh;		// silhouette
};

// Concave L, extruded along X in [-hx,hx]. Cross-section in (y,z): full block y[-6,6] z[-4,0] plus top-left
// y[-6,0] z[0,4] -> the top-right quarter y[0,6] z[0,4] is the notch. Hexagon (CCW in y,z), outward faces.
Occluder MakeLNotch( float hx )
{
	Occluder o;
	// truth: two tiling boxes (share the z=0 face over y[-6,0]; union = exact L)
	o.boxes.push_back( MakeBox( float3( 0, 0, -2 ), float3( hx, 6, 2 ) ) );	// bottom slab  y[-6,6] z[-4,0]
	o.boxes.push_back( MakeBox( float3( 0, -3, 2 ), float3( hx, 3, 2 ) ) );	// top-left     y[-6,0] z[0,4]
	// silhouette mesh: explicit hexagonal prism
	const float cs[6][2] = { { -6, -4 }, { 6, -4 }, { 6, 0 }, { 0, 0 }, { 0, 4 }, { -6, 4 } };	// (y,z) CCW
	PolyMesh& m = o.mesh;
	for( int e = 0; e < 2; e++ )				// two end caps at x = -hx (0..5) and +hx (6..11)
	{
		float x = e ? hx : -hx;
		for( int i = 0; i < 6; i++ ) { m.v.push_back( float3( x, cs[i][0], cs[i][1] ) ); }
	}
	// end caps: -x cap faces -X (wind so Newell points -X); +x cap faces +X
	m.faces.push_back( { 5, 4, 3, 2, 1, 0 } );			// x=-hx cap, outward = -X
	m.faces.push_back( { 6, 7, 8, 9, 10, 11 } );		// x=+hx cap, outward = +X
	for( int i = 0; i < 6; i++ )						// 6 side quads, outward
	{
		int a = i, b = ( i + 1 ) % 6;
		m.faces.push_back( { a, b, 6 + b, 6 + a } );
	}
	return o;
}

Occluder MakeConvex( float3 h )
{
	Occluder o;
	Box b = MakeBox( float3( 0, 0, 0 ), h );
	o.boxes.push_back( b );
	AppendBox( o.mesh, b );
	return o;
}

Occluder MakeDisjoint( float hx )
{
	Occluder o;
	Box a = MakeBox( float3( 0,  5, 0 ), float3( hx, 3, 4 ) );
	Box b = MakeBox( float3( 0, -5, 0 ), float3( hx, 3, 4 ) );		// gap in y between them
	o.boxes = { a, b };
	AppendBox( o.mesh, a );
	AppendBox( o.mesh, b );
	return o;
}

// independent ray-cast area-light truth over a UNION of boxes (1 = lit). Mirrors TruthShadow but the ray is
// blocked if it hits ANY box; boxes tile the solid so this is exact ground truth for the concave/disjoint set.
float TruthShadowBoxes( float3 P, float3 L, float r, const std::vector<Box>& boxes, int N = 96 )
{
	float3 toL = L - P;
	float dist = len3( toL );
	float3 nrm = toL * ( 1.0f / dist );
	float3 up = ( std::fabs( nrm.z ) > 0.9f ) ? v3( 0, 1, 0 ) : v3( 0, 0, 1 );
	float3 u = normalize( cross( up, nrm ) );
	float3 vv = cross( nrm, u );
	int inside = 0, total = 0;
	for( int iy = 0; iy < N; iy++ )
		for( int ix = 0; ix < N; ix++ )
		{
			float du = ( ix + 0.5f ) / N * 2 - 1, dv = ( iy + 0.5f ) / N * 2 - 1;
			if( du * du + dv * dv > 1.0f ) { continue; }
			total++;
			float3 Dp = L + u * ( du * r ) + vv * ( dv * r );
			bool hit = false;
			for( const Box& b : boxes ) { if( RayHitsBox( P, Dp - P, b ) ) { hit = true; break; } }
			if( hit ) { inside++; }
		}
	return total ? 1.0f - ( float )inside / total : 1.0f;
}

// silhouette OCCLUSION (1 = fully shadowed) via the shipped SoftShadow_WedgeOcclusion, from the light-centre
// silhouette loops. swCentreLit = 0 (general path: no AAM illusory-winding subtraction). outLoops (optional)
// receives the number of silhouette loops (chains) - a caster-emit-time quantity the gate uses.
float SilhouetteOcclusion( const Occluder& o, float3 P, float3 L, float r, int* outLoops = nullptr )
{
	auto loops = SilhouetteMesh( o.mesh, L );
	std::vector<std::vector<float3>> nonEmpty;
	for( auto& l : loops ) { if( l.size() >= 3 ) { nonEmpty.push_back( l ); } }
	if( outLoops ) { *outLoops = ( int )nonEmpty.size(); }
	if( nonEmpty.empty() ) { return 0.0f; }
	std::vector<float4> rec = BuildCaster( nonEmpty );
	if( rec.empty() ) { return 0.0f; }
	SoftEdgeBuffer buf{ rec.data(), ( int )rec.size() };
	return saturate( SoftShadow_WedgeOcclusion( P, L, r, 0, ( int )( rec.size() / 2 ), 0.0f, buf ) );
}

// shipped FACE-COVERAGE occlusion (r_softShadowFaceCoverage path) over the union of boxes, for the
// cross-check: where silhouette drifts but face stays accurate is exactly the 5% that needs Fubini.
float FaceOcclusionBoxes( const std::vector<Box>& boxes, float3 P, float3 L, float r )
{
	FaceStreamCPU s;
	for( const Box& b : boxes ) { s.Append( BuildFaceCasterUnit( b ) ); }
	SoftEdgeBuffer buf{ s.buf.data(), ( int )s.buf.size() };
	return saturate( SoftShadow_FaceCoverage( P, L, r, s.triBase(), 0, s.nCasters, SoftRotAngle( P ), buf ) );
}

// off-axis angle (deg) of the receiver->light axis from world +Z, and the light disk half-angle sinA.
float OffAxisDeg( float3 P, float3 L )
{
	float3 d = normalize( L - P );
	return std::acos( std::fmin( 1.0f, std::fabs( d.z ) ) ) * ( 180.0f / 3.14159265f );
}

struct Row
{
	Shape shape; float r; float s; float coff;
	float offAxis; float sinA; float distPL; int loops;
	float occSil, occFace, occTruth;
	float errSil, errFace;			// signed: occ - truth (positive = over-shadow, negative = UNDER-shadow)
};

} // namespace

STUDY_TEST( SoftShadowSilhouette, error_surface )
{
	// ---- SCENE ----------------------------------------------------------------------------------------
	// Occluder centred at origin. Receiver P and light L symmetric about the occluder so the light-centre
	// ray stays inside the caster (a real umbra core) for every config; the tilt parameter s swings both
	// laterally in +/-Y, driving the receiver OFF-AXIS (grazing view of the silhouette). coff pushes the
	// occluder toward the receiver (contact / foreshortening). r = light-disk radius (sharp -> wide).
	const float Rs[]  = { 2.0f, 6.0f, 14.0f, 30.0f };
	const float Ss[]  = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };		// tilt: 0 = on-axis, 1 ~= 59 deg off-axis
	const float Coff[] = { 0.0f, 18.0f };						// occluder z-shift toward receiver (contact)
	const Shape shapes[] = { CONVEX, LNOTCH, DISJOINT };

	std::vector<Row> rows;
	for( Shape sh : shapes )
	{
		Occluder occ = ( sh == CONVEX ) ? MakeConvex( float3( 6, 6, 4 ) )
					 : ( sh == LNOTCH ) ? MakeLNotch( 6.0f )
										: MakeDisjoint( 6.0f );
		for( float coff : Coff )
			for( float s : Ss )
			{
				float3 P = float3( 0,  50.0f * s, -30.0f - coff );
				float3 L = float3( 0, -50.0f * s,  30.0f );
				for( float r : Rs )
				{
					Row row;
					row.shape = sh; row.r = r; row.s = s; row.coff = coff;
					row.offAxis = OffAxisDeg( P, L );
					row.distPL  = len3( L - P );
					row.sinA    = std::fmin( 1.0f, r / row.distPL );
					row.occSil  = SilhouetteOcclusion( occ, P, L, r, &row.loops );
					row.occFace = FaceOcclusionBoxes( occ.boxes, P, L, r );
					row.occTruth = 1.0f - TruthShadowBoxes( P, L, r, occ.boxes );
					row.errSil  = row.occSil  - row.occTruth;
					row.errFace = row.occFace - row.occTruth;
					rows.push_back( row );
				}
			}
	}

	// ---- SANITY: the ORACLE is TruthShadow (ray-cast). The shipped face path is the intended FALLBACK, not
	// ground truth; measurement shows it carries its OWN error (up to ~0.29 at near-contact wide light, where
	// its fixed disk-sample count saturates), so this is a loose harness guard only, not an accuracy claim.
	double worstFace = 0; for( const Row& r : rows ) { worstFace = std::fmax( worstFace, std::fabs( r.errFace ) ); }
	std::printf( "\n  (face-path worst |err| vs truth = %.3f - the fallback is NOT exact either)\n", worstFace );
	for( const Row& r : rows ) { CHECK_NEAR( r.errFace, 0.0f, 0.35 ); }

	// ---- ERROR SURFACE (printed) ----------------------------------------------------------------------
	std::printf( "\n  shape     r    off  sinA  |  occSil occFace occTru | errSil  errFace\n" );
	std::printf(   "  ------------------------------------------------------------------------\n" );
	for( const Row& r : rows )
	{
		std::printf( "  %-8s %4.0f %5.1f %4.2f |  %5.3f  %5.3f  %5.3f | %+6.3f  %+6.3f\n",
					 ShapeName( r.shape ), r.r, r.offAxis, r.sinA,
					 r.occSil, r.occFace, r.occTruth, r.errSil, r.errFace );
	}

	// ---- DRIVER ATTRIBUTION: bucket the SIGNED silhouette error by candidate driver. ------------------
	auto meanAbs = [&]( bool ( *pred )( const Row& ) )
	{
		double s = 0; int n = 0; for( const Row& r : rows ) { if( pred( r ) ) { s += std::fabs( r.errSil ); n++; } }
		return n ? s / n : 0.0;
	};
	auto meanSigned = [&]( bool ( *pred )( const Row& ) )
	{
		double s = 0; int n = 0; for( const Row& r : rows ) { if( pred( r ) ) { s += r.errSil; n++; } }
		return n ? s / n : 0.0;
	};
	std::printf( "\n  DRIVER ATTRIBUTION (mean |errSil|, mean signed errSil):\n" );
	std::printf( "    on-axis  (off< 5)   : %.3f  %+.3f\n", meanAbs( []( const Row& r ) { return r.offAxis <  5.0f; } ), meanSigned( []( const Row& r ) { return r.offAxis <  5.0f; } ) );
	std::printf( "    grazing  (off>=40)  : %.3f  %+.3f\n", meanAbs( []( const Row& r ) { return r.offAxis >= 40.0f; } ), meanSigned( []( const Row& r ) { return r.offAxis >= 40.0f; } ) );
	std::printf( "    small disk (sinA<.1): %.3f  %+.3f\n", meanAbs( []( const Row& r ) { return r.sinA <  0.1f; } ), meanSigned( []( const Row& r ) { return r.sinA <  0.1f; } ) );
	std::printf( "    wide disk (sinA>=.3): %.3f  %+.3f\n", meanAbs( []( const Row& r ) { return r.sinA >= 0.3f; } ), meanSigned( []( const Row& r ) { return r.sinA >= 0.3f; } ) );
	std::printf( "    convex              : %.3f  %+.3f\n", meanAbs( []( const Row& r ) { return r.shape == CONVEX; } ), meanSigned( []( const Row& r ) { return r.shape == CONVEX; } ) );
	std::printf( "    Lnotch(concave)     : %.3f  %+.3f\n", meanAbs( []( const Row& r ) { return r.shape == LNOTCH; } ), meanSigned( []( const Row& r ) { return r.shape == LNOTCH; } ) );
	std::printf( "    disjoint(2 loops)   : %.3f  %+.3f\n", meanAbs( []( const Row& r ) { return r.shape == DISJOINT; } ), meanSigned( []( const Row& r ) { return r.shape == DISJOINT; } ) );
	std::printf( "    single-loop         : %.3f  %+.3f\n", meanAbs( []( const Row& r ) { return r.loops == 1; } ), meanSigned( []( const Row& r ) { return r.loops == 1; } ) );
	std::printf( "    multi-loop          : %.3f  %+.3f\n", meanAbs( []( const Row& r ) { return r.loops >  1; } ), meanSigned( []( const Row& r ) { return r.loops >  1; } ) );
	std::printf( "    sinA<=.10 & 1 loop  : %.3f  %+.3f\n", meanAbs( []( const Row& r ) { return r.loops == 1 && r.sinA <= 0.10f; } ), meanSigned( []( const Row& r ) { return r.loops == 1 && r.sinA <= 0.10f; } ) );
	std::printf( "    sinA> .10 & 1 loop  : %.3f  %+.3f\n", meanAbs( []( const Row& r ) { return r.loops == 1 && r.sinA >  0.10f; } ), meanSigned( []( const Row& r ) { return r.loops == 1 && r.sinA >  0.10f; } ) );
	std::printf( "  NOTE: off-axis is NOT the driver - on-axis is WORSE than grazing (refutes the tr_frontend_addmodels.cpp:59 comment).\n"
				 "        Drivers = light angular radius sinA (mid-range ~0.12-0.23 worst, UNDERSHOOT) and silhouette LOOP COUNT.\n"
				 "        A concave/disjoint caster presents as >1 silhouette loop; single-loop small-light stays exact (|err|<=0.006).\n" );

	// ---- GATE PREDICATE -------------------------------------------------------------------------------
	// DERIVED FROM THE SURFACE ABOVE. The driver is the LIGHT ANGULAR RADIUS sinA = r/distPL (both cheap:
	// caster-emit or per-fragment) together with the SILHOUETTE LOOP COUNT (a caster-emit-time integer).
	// The measurement shows:
	//   * off-axis angle is NOT the driver (on-axis is worst) - the fold-score idea was wrong, dropped.
	//   * a SINGLE loop stays exact while sinA is small (|err| <= 0.006 across the sweep).
	//   * a wide light (large sinA) makes the light-centre silhouette UNDER-cover the disk (undershoot),
	//     and a concave OR disjoint caster presents as >1 silhouette loop, whose signed-area sum mis-counts
	//     the inter-loop / notch penumbra even at small sinA - the loop count captures BOTH bad shapes.
	// So the conservative gate is: SILHOUETTE trusted iff  loops == 1  AND  sinA <= SINA_THR ; else Fubini.
	// Every single-loop config at sinA <= 0.10 measured |err| <= ~0.006 (a 10x margin under EPS 0.06). There
	// are NO single-loop configs with sinA in (0.10, 0.12], so the threshold sits in a clean gap - margin.
	const double EPS       = 0.06;		// same tight coverage tolerance the FillBox parity test uses
	const double SINA_THR  = 0.10;		// disk half-angle gate; safe-region worst |err| ~0.006 << EPS

	auto safe = [&]( const Row& r )		// the derived predicate (loop count + angular light radius)
	{
		return r.loops == 1 && r.sinA <= ( float )SINA_THR;
	};

	// calibration: worst silhouette error inside vs outside the predicate, and the failure region's driver.
	double worstIn = 0, worstOut = 0; int nIn = 0, nOut = 0;
	Row failWorst = rows[0]; double failMag = -1;
	for( const Row& r : rows )
	{
		if( safe( r ) ) { worstIn = std::fmax( worstIn, std::fabs( r.errSil ) ); nIn++; }
		else            { worstOut = std::fmax( worstOut, std::fabs( r.errSil ) ); nOut++; if( std::fabs( r.errSil ) > failMag ) { failMag = std::fabs( r.errSil ); failWorst = r; } }
	}
	std::printf( "\n  GATE: silhouette trusted iff  loops==1 && sinA <= %.2f   (EPS=%.2f)\n", SINA_THR, EPS );
	std::printf( "    safe region  : %d configs, worst |errSil| = %.3f\n", nIn, worstIn );
	std::printf( "    fubini region: %d configs, worst |errSil| = %.3f\n", nOut, worstOut );
	std::printf( "    worst overall failure: %-8s r=%.0f off=%.1f sinA=%.2f loops=%d errSil=%+.3f (face %+.3f)\n",
				 ShapeName( failWorst.shape ), failWorst.r, failWorst.offAxis, failWorst.sinA,
				 failWorst.loops, failWorst.errSil, failWorst.errFace );

	// ---- ASSERTIONS (red-until-green) -----------------------------------------------------------------
	// (1) The general SilhouetteMesh must agree with the proven Box silhouette on the convex on-axis case
	//     (harness self-check: the silhouette input is correct, so a drift is the METHOD, not a bad loop).
	{
		Box b = MakeBox( float3( 0, 0, 0 ), float3( 6, 6, 4 ) );
		float3 P( 0, 0, -30 ), L( 0, 0, 30 );
		Occluder occ = MakeConvex( float3( 6, 6, 4 ) );
		float genOcc = SilhouetteOcclusion( occ, P, L, 6.0f );
		std::vector<float4> rec = BuildCaster( { Silhouette( b, L ) } );
		SoftEdgeBuffer buf{ rec.data(), ( int )rec.size() };
		float boxOcc = saturate( SoftShadow_WedgeOcclusion( P, L, 6.0f, 0, ( int )( rec.size() / 2 ), 0.0f, buf ) );
		CHECK_NEAR( genOcc, boxOcc, 1e-4 );
	}

	// (2) INSIDE the gate the silhouette is accurate (the "95%"): every safe config under EPS.
	for( const Row& r : rows ) { if( safe( r ) ) { CHECK_NEAR( r.errSil, 0.0, EPS ); } }

	// (3) The gate is NON-TRIVIAL: it must accept a real slab of the single-loop small-light configs, not
	//     degenerate to "reject everything". The sweep is deliberately adversarial (half its configs are
	//     wide lights sinA up to 0.5, rare in game); the admitted slab is every single-loop config with a
	//     realistic small light. (Guards against a vacuously-green predicate.)
	CHECK( nIn >= 18 );

	// (4) The failure region EXISTS and is CHARACTERISED (documented, not a bug): at least one config drifts
	//     well past EPS, proving silhouette genuinely needs a Fubini fallback there.
	CHECK( worstOut > 2.0 * EPS );
}
