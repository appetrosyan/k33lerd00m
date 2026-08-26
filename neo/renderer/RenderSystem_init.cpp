/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2014-2024 Robert Beckebans
Copyright (C) 2014-2016 Kot in Action Creative Artel
Copyright (C) 2022 Stephen Pridham

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

#include "imgui.h"

#if defined(USE_INTRINSICS_SSE)
	#if MOC_MULTITHREADED
		#include "CullingThreadPool.h"
	#else
		#include "../libs/moc/MaskedOcclusionCulling.h"
	#endif
#endif

#include "RenderCommon.h"
#include "RenderCapture.h"

#include "sys/DeviceManager.h"

// RB begin
#if defined(_WIN32)

	// Vista OpenGL wrapper check
	#include "../sys/win32/win_local.h"
#endif
// RB end

// foresthale 2014-03-01: fixed custom screenshot resolution by doing a more direct render path
#define BUGFIXEDSCREENSHOTRESOLUTION 1
#ifdef BUGFIXEDSCREENSHOTRESOLUTION
	#include "../framework/Common_local.h"
#endif


// DeviceContext bypasses RenderSystem to work directly with this
idGuiModel* tr_guiModel;

// functions that are not called every frame
glconfig_t	glConfig;

idCVar r_requestStereoPixelFormat( "r_requestStereoPixelFormat", "1", CVAR_RENDERER, "Ask for a stereo GL pixel format on startup" );
idCVar r_debugContext( "r_debugContext", "0", CVAR_RENDERER, "Enable various levels of context debug." );

#if defined( _WIN32 )
	idCVar r_graphicsAPI( "r_graphicsAPI", "dx12", CVAR_RENDERER | CVAR_INIT | CVAR_ARCHIVE | CVAR_NEW, "Specifies the graphics api to use (dx12, vulkan)" );
#else
	idCVar r_graphicsAPI( "r_graphicsAPI", "vulkan", CVAR_RENDERER | CVAR_ROM | CVAR_STATIC | CVAR_NEW, "Specifies the graphics api to use (vulkan)" );
#endif

idCVar r_useValidationLayers( "r_useValidationLayers", "1", CVAR_INTEGER | CVAR_INIT | CVAR_NEW, "1 is just the NVRHI and 2 will turn on additional DX12, VK validation layers" );

// RB: disabled 16x MSAA
#if ID_MSAA
	idCVar r_antiAliasing( "r_antiAliasing", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER | CVAR_NEW, " 0 = None\n 1 = TAA 1x\n 2 = TAA + SMAA 1x\n 3 = MSAA 2x\n 4 = MSAA 4x\n", 0, ANTI_ALIASING_MSAA_4X );
#else
	idCVar r_antiAliasing( "r_antiAliasing", "2", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER | CVAR_NEW, " 0 = None\n 1 = SMAA 1x\n 2 = TAA", 0, ANTI_ALIASING_TAA );
#endif
// RB end

// SSAA supersampling: render the 3D scene at nativeRes * scale and downsample. Snapped to
// {1.0, 1.5, 2.0} by R_SSAAScale(). Changing it reallocates the scene render targets.
idCVar r_ssaaScale( "r_ssaaScale", "1.0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT | CVAR_NEW, "supersample the 3D scene: 1 = off, 1.5, 2.0", 1.0f, 2.0f );
idCVar r_vidMode( "r_vidMode", "0", CVAR_ARCHIVE | CVAR_RENDERER | CVAR_INTEGER, "fullscreen video mode number" );
idCVar r_displayRefresh( "r_displayRefresh", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_NOCHEAT, "optional display refresh rate option for vid mode", 0.0f, 240.0f );
// SRS - redefined mode -2 to be borderless fullscreen, implemented borderless modes -2 and -1 for Windows and linux/macOS (SDL)
// DG: add mode -2 for SDL, also defaulting to windowed mode, as that causes less trouble on linux
idCVar r_fullscreen( "r_fullscreen", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "-2 = borderless fullscreen, -1 = borderless window, 0 = windowed, 1 = full screen on monitor 1, 2 = full screen on monitor 2, etc" );
// DG end
idCVar r_customWidth( "r_customWidth", "1280", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "custom screen width. set r_vidMode to -1 to activate" );
idCVar r_customHeight( "r_customHeight", "720", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "custom screen height. set r_vidMode to -1 to activate" );
idCVar r_windowX( "r_windowX", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "Non-fullscreen parameter" );
idCVar r_windowY( "r_windowY", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "Non-fullscreen parameter" );
idCVar r_windowWidth( "r_windowWidth", "1280", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "Non-fullscreen parameter" );
idCVar r_windowHeight( "r_windowHeight", "720", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "Non-fullscreen parameter" );

idCVar r_useViewBypass( "r_useViewBypass", "1", CVAR_RENDERER | CVAR_INTEGER, "bypass a frame of latency to the view" );
idCVar r_useLightPortalFlow( "r_useLightPortalFlow", "1", CVAR_RENDERER | CVAR_BOOL, "use a more precise area reference determination" );
idCVar r_singleTriangle( "r_singleTriangle", "0", CVAR_RENDERER | CVAR_BOOL, "only draw a single triangle per primitive" );
idCVar r_checkBounds( "r_checkBounds", "0", CVAR_RENDERER | CVAR_BOOL, "compare all surface bounds with precalculated ones" );

idCVar r_useNodeCommonChildren( "r_useNodeCommonChildren", "1", CVAR_RENDERER | CVAR_BOOL, "stop pushing reference bounds early when possible" );
idCVar r_useShadowSurfaceScissor( "r_useShadowSurfaceScissor", "1", CVAR_RENDERER | CVAR_BOOL, "scissor shadows by the scissor rect of the interaction surfaces" );

idCVar r_maxAnisotropicFiltering( "r_maxAnisotropicFiltering", "16", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "limit aniso filtering (reduces texture crawl on grazing surfaces)" );
idCVar r_useTrilinearFiltering( "r_useTrilinearFiltering", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "Extra quality filtering" );
// RB: not used anymore
idCVar r_lodBias( "r_lodBias", "0.5", CVAR_RENDERER | CVAR_ARCHIVE, "UNUSED: image lod bias" );
// RB end

idCVar r_useStateCaching( "r_useStateCaching", "1", CVAR_RENDERER | CVAR_BOOL, "avoid redundant state changes in GL_*() calls" );

idCVar r_znear( "r_znear", "3", CVAR_RENDERER | CVAR_FLOAT, "near Z clip plane distance", 0.001f, 200.0f );

idCVar r_swapInterval( "r_swapInterval", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "0 = tear, 1 = swap-tear where available, 2 = always v-sync" );

idCVar r_gamma( "r_gamma", "1.0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "changes gamma tables", 0.5f, 3.0f );
idCVar r_brightness( "r_brightness", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT, "changes gamma tables", 0.5f, 2.0f );

idCVar r_skipStaticInteractions( "r_skipStaticInteractions", "0", CVAR_RENDERER | CVAR_BOOL, "skip interactions created at level load" );
idCVar r_skipDynamicInteractions( "r_skipDynamicInteractions", "0", CVAR_RENDERER | CVAR_BOOL, "skip interactions created after level load" );
idCVar r_skipSuppress( "r_skipSuppress", "0", CVAR_RENDERER | CVAR_BOOL, "ignore the per-view suppressions" );
idCVar r_skipPostProcess( "r_skipPostProcess", "0", CVAR_RENDERER | CVAR_BOOL, "skip all post-process renderings except bloom" );
idCVar r_skipBloom( "r_skipBloom", "0", CVAR_RENDERER | CVAR_BOOL, "Skip bloom" );
idCVar r_skipInteractions( "r_skipInteractions", "0", CVAR_RENDERER | CVAR_BOOL, "skip all light/surface interaction drawing" );
idCVar r_skipDynamicTextures( "r_skipDynamicTextures", "0", CVAR_RENDERER | CVAR_BOOL, "don't dynamically create textures" );
idCVar r_skipCopyTexture( "r_skipCopyTexture", "0", CVAR_RENDERER | CVAR_BOOL, "do all rendering, but don't actually copyTexSubImage2D" );
idCVar r_skipBackEnd( "r_skipBackEnd", "0", CVAR_RENDERER | CVAR_BOOL, "don't draw anything" );
idCVar r_skipRender( "r_skipRender", "0", CVAR_RENDERER | CVAR_BOOL, "skip 3D rendering, but pass 2D" );
idCVar r_skipTranslucent( "r_skipTranslucent", "0", CVAR_RENDERER | CVAR_BOOL, "skip the translucent interaction rendering" );
idCVar r_skipAmbient( "r_skipAmbient", "0", CVAR_RENDERER | CVAR_BOOL, "bypasses all non-interaction drawing" );
idCVar r_skipNewAmbient( "r_skipNewAmbient", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE, "bypasses all vertex/fragment program ambient drawing" );
idCVar r_skipBlendLights( "r_skipBlendLights", "0", CVAR_RENDERER | CVAR_BOOL, "skip all blend lights" );
idCVar r_skipFogLights( "r_skipFogLights", "0", CVAR_RENDERER | CVAR_BOOL, "skip all fog lights" );
idCVar r_skipDeforms( "r_skipDeforms", "0", CVAR_RENDERER | CVAR_BOOL, "leave all deform materials in their original state" );
idCVar r_skipFrontEnd( "r_skipFrontEnd", "0", CVAR_RENDERER | CVAR_BOOL, "bypasses all front end work, but 2D gui rendering still draws" );
idCVar r_skipUpdates( "r_skipUpdates", "0", CVAR_RENDERER | CVAR_BOOL, "1 = don't accept any entity or light updates, making everything static" );
idCVar r_skipDecals( "r_skipDecals", "0", CVAR_RENDERER | CVAR_BOOL, "skip decal surfaces" );
idCVar r_skipOverlays( "r_skipOverlays", "0", CVAR_RENDERER | CVAR_BOOL, "skip overlay surfaces" );
idCVar r_skipSpecular( "r_skipSpecular", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_CHEAT | CVAR_ARCHIVE, "use black for specular1" );
idCVar r_skipBump( "r_skipBump", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE, "uses a flat surface instead of the bump map" );
idCVar r_skipDiffuse( "r_skipDiffuse", "0", CVAR_RENDERER | CVAR_INTEGER, "use black for diffuse" );
idCVar r_skipSubviews( "r_skipSubviews", "0", CVAR_RENDERER | CVAR_INTEGER, "1 = don't render any gui elements on surfaces" );
idCVar r_skipGuiShaders( "r_skipGuiShaders", "0", CVAR_RENDERER | CVAR_INTEGER, "1 = skip all gui elements on surfaces, 2 = skip drawing but still handle events, 3 = draw but skip events", 0, 3, idCmdSystem::ArgCompletion_Integer<0, 3> );
idCVar r_skipParticles( "r_skipParticles", "0", CVAR_RENDERER | CVAR_INTEGER, "1 = skip all particle systems", 0, 1, idCmdSystem::ArgCompletion_Integer<0, 1> );
idCVar r_skipShadows( "r_skipShadows", "0", CVAR_RENDERER | CVAR_BOOL  | CVAR_ARCHIVE, "disable shadows" );

idCVar r_useLightPortalCulling( "r_useLightPortalCulling", "1", CVAR_RENDERER | CVAR_INTEGER, "0 = none, 1 = cull frustum corners to plane, 2 = exact clip the frustum faces", 0, 2, idCmdSystem::ArgCompletion_Integer<0, 2> );
idCVar r_useLightAreaCulling( "r_useLightAreaCulling", "1", CVAR_RENDERER | CVAR_BOOL, "0 = off, 1 = on" );
idCVar r_useLightScissors( "r_useLightScissors", "3", CVAR_RENDERER | CVAR_INTEGER, "0 = no scissor, 1 = non-clipped scissor, 2 = near-clipped scissor, 3 = fully-clipped scissor", 0, 3, idCmdSystem::ArgCompletion_Integer<0, 3> );
idCVar r_useEntityPortalCulling( "r_useEntityPortalCulling", "1", CVAR_RENDERER | CVAR_INTEGER, "0 = none, 1 = cull frustum corners to plane, 2 = exact clip the frustum faces", 0, 2, idCmdSystem::ArgCompletion_Integer<0, 2> );
idCVar r_clear( "r_clear", "2", CVAR_RENDERER | CVAR_NOCHEAT, "force screen clear every frame, 1 = purple, 2 = black, 'r g b' = custom" );

idCVar r_offsetFactor( "r_offsetfactor", "0", CVAR_RENDERER | CVAR_FLOAT, "polygon offset parameter" );
// RB: offset factor was 0, and units were -600 which caused some very ugly polygon offsets on Android so I reverted the values to the same as in Q3A
#if defined(__ANDROID__)
	idCVar r_offsetUnits( "r_offsetunits", "-2", CVAR_RENDERER | CVAR_FLOAT, "polygon offset parameter" );
#else
	idCVar r_offsetUnits( "r_offsetunits", "-600", CVAR_RENDERER | CVAR_FLOAT, "polygon offset parameter" );
#endif
// RB end

idCVar r_subviewOnly( "r_subviewOnly", "0", CVAR_RENDERER | CVAR_BOOL, "1 = don't render main view, allowing subviews to be debugged" );
idCVar r_testGamma( "r_testGamma", "0", CVAR_RENDERER | CVAR_FLOAT, "if > 0 draw a grid pattern to test gamma levels", 0, 195 );
idCVar r_testGammaBias( "r_testGammaBias", "0", CVAR_RENDERER | CVAR_FLOAT, "if > 0 draw a grid pattern to test gamma levels" );
idCVar r_lightScale( "r_lightScale", "3", CVAR_ARCHIVE | CVAR_RENDERER | CVAR_FLOAT, "all light intensities are multiplied by this", 0, 100 );
idCVar r_flareSize( "r_flareSize", "1", CVAR_RENDERER | CVAR_FLOAT, "scale the flare deforms from the material def" );

idCVar r_useScissor( "r_useScissor", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_NOCHEAT, "scissor clip as portals and lights are processed" );
idCVar r_useLightDepthBounds( "r_useLightDepthBounds", "1", CVAR_RENDERER | CVAR_BOOL, "use depth bounds test on lights to reduce both shadow and interaction fill" );
idCVar r_useShadowDepthBounds( "r_useShadowDepthBounds", "1", CVAR_RENDERER | CVAR_BOOL, "use depth bounds test on individual shadow volumes to reduce shadow fill" );

idCVar r_screenFraction( "r_screenFraction", "100", CVAR_RENDERER | CVAR_INTEGER, "for testing fill rate, the resolution of the entire screen can be changed" );
idCVar r_usePortals( "r_usePortals", "1", CVAR_RENDERER | CVAR_BOOL, " 1 = use portals to perform area culling, otherwise draw everything" );
idCVar r_singleLight( "r_singleLight", "-1", CVAR_RENDERER | CVAR_INTEGER, "suppress all but one light" );
idCVar r_singleEntity( "r_singleEntity", "-1", CVAR_RENDERER | CVAR_INTEGER, "suppress all but one entity" );
idCVar r_singleEnvprobe( "r_singleEnvprobe", "-1", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "suppress all but one environment probe" );
idCVar r_singleSurface( "r_singleSurface", "-1", CVAR_RENDERER | CVAR_INTEGER, "suppress all but one surface on each entity" );
idCVar r_singleArea( "r_singleArea", "0", CVAR_RENDERER | CVAR_BOOL, "only draw the portal area the view is actually in" );
idCVar r_orderIndexes( "r_orderIndexes", "1", CVAR_RENDERER | CVAR_BOOL, "perform index reorganization to optimize vertex use" );
idCVar r_lightAllBackFaces( "r_lightAllBackFaces", "1", CVAR_RENDERER | CVAR_BOOL, "light all the back faces, even when they would be shadowed" );

// visual debugging info
idCVar r_showPortals( "r_showPortals", "0", CVAR_RENDERER | CVAR_BOOL, "draw portal outlines in color based on passed / not passed" );
idCVar r_showUnsmoothedTangents( "r_showUnsmoothedTangents", "0", CVAR_RENDERER | CVAR_BOOL, "if 1, put all nvidia register combiner programming in display lists" );
idCVar r_showSilhouette( "r_showSilhouette", "0", CVAR_RENDERER | CVAR_BOOL, "highlight edges that are casting shadow planes" );
idCVar r_showVertexColor( "r_showVertexColor", "0", CVAR_RENDERER | CVAR_BOOL, "draws all triangles with the solid vertex color" );
idCVar r_showUpdates( "r_showUpdates", "0", CVAR_RENDERER | CVAR_BOOL, "report entity and light updates and ref counts" );
idCVar r_showDynamic( "r_showDynamic", "0", CVAR_RENDERER | CVAR_BOOL, "report stats on dynamic surface generation" );
idCVar r_showTrace( "r_showTrace", "0", CVAR_RENDERER | CVAR_INTEGER, "show the intersection of an eye trace with the world", idCmdSystem::ArgCompletion_Integer<0, 2> );
idCVar r_showIntensity( "r_showIntensity", "0", CVAR_RENDERER | CVAR_BOOL, "draw the screen colors based on intensity, red = 0, green = 128, blue = 255" );
idCVar r_showLights( "r_showLights", "0", CVAR_RENDERER | CVAR_INTEGER, "1 = just print volumes numbers, highlighting ones covering the view, 2 = also draw planes of each volume, 3 = also draw edges of each volume", 0, 3, idCmdSystem::ArgCompletion_Integer<0, 3> );
idCVar r_showShadows( "r_showShadows", "0", CVAR_RENDERER | CVAR_INTEGER, "1 = visualize the stencil shadow volumes, 2 = draw filled in", 0, 3, idCmdSystem::ArgCompletion_Integer<0, 3> );
idCVar r_showLightScissors( "r_showLightScissors", "0", CVAR_RENDERER | CVAR_BOOL, "show light scissor rectangles" );
idCVar r_showLightCount( "r_showLightCount", "0", CVAR_RENDERER | CVAR_INTEGER, "1 = colors surfaces based on light count, 2 = also count everything through walls, 3 = also print overdraw", 0, 3, idCmdSystem::ArgCompletion_Integer<0, 3> );
idCVar r_showViewEntitys( "r_showViewEntitys", "0", CVAR_RENDERER | CVAR_INTEGER, "1 = displays the bounding boxes of all view models, 2 = print index numbers" );
idCVar r_showTris( "r_showTris", "0", CVAR_RENDERER | CVAR_INTEGER, "enables wireframe rendering of the world, 1 = only draw visible ones, 2 = draw all front facing, 3 = draw all, 4 = draw with alpha", 0, 4, idCmdSystem::ArgCompletion_Integer<0, 4> );
idCVar r_showSurfaceInfo( "r_showSurfaceInfo", "0", CVAR_RENDERER | CVAR_BOOL, "show surface material name under crosshair" );
idCVar r_showNormals( "r_showNormals", "0", CVAR_RENDERER | CVAR_FLOAT, "draws wireframe normals" );
idCVar r_showMemory( "r_showMemory", "0", CVAR_RENDERER | CVAR_BOOL, "print frame memory utilization" );
idCVar r_showCull( "r_showCull", "0", CVAR_RENDERER | CVAR_BOOL, "report sphere and box culling stats" );
idCVar r_showAddModel( "r_showAddModel", "0", CVAR_RENDERER | CVAR_BOOL, "report stats from tr_addModel" );
idCVar r_showDepth( "r_showDepth", "0", CVAR_RENDERER | CVAR_BOOL, "display the contents of the depth buffer and the depth range" );
idCVar r_showSurfaces( "r_showSurfaces", "0", CVAR_RENDERER | CVAR_BOOL, "report surface/light/shadow counts" );
idCVar r_showPrimitives( "r_showPrimitives", "0", CVAR_RENDERER | CVAR_INTEGER, "report drawsurf/index/vertex counts" );
idCVar r_showEdges( "r_showEdges", "0", CVAR_RENDERER | CVAR_BOOL, "draw the sil edges" );
idCVar r_showTexturePolarity( "r_showTexturePolarity", "0", CVAR_RENDERER | CVAR_BOOL, "shade triangles by texture area polarity" );
idCVar r_showTangentSpace( "r_showTangentSpace", "0", CVAR_RENDERER | CVAR_INTEGER, "shade triangles by tangent space, 1 = use 1st tangent vector, 2 = use 2nd tangent vector, 3 = use normal vector", 0, 3, idCmdSystem::ArgCompletion_Integer<0, 3> );
idCVar r_showDominantTri( "r_showDominantTri", "0", CVAR_RENDERER | CVAR_BOOL, "draw lines from vertexes to center of dominant triangles" );
idCVar r_showTextureVectors( "r_showTextureVectors", "0", CVAR_RENDERER | CVAR_FLOAT, " if > 0 draw each triangles texture (tangent) vectors" );
idCVar r_showOverDraw( "r_showOverDraw", "0", CVAR_RENDERER | CVAR_INTEGER, "1 = geometry overdraw, 2 = light interaction overdraw, 3 = geometry and light interaction overdraw", 0, 3, idCmdSystem::ArgCompletion_Integer<0, 3> );
// RB begin
idCVar r_showShadowMaps( "r_showShadowMaps", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "" );
idCVar r_showShadowMapLODs( "r_showShadowMapLODs", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "" );
// RB end

idCVar r_useEntityCallbacks( "r_useEntityCallbacks", "1", CVAR_RENDERER | CVAR_BOOL, "if 0, issue the callback immediately at update time, rather than defering" );

idCVar r_showSkel( "r_showSkel", "0", CVAR_RENDERER | CVAR_INTEGER, "draw the skeleton when model animates, 1 = draw model with skeleton, 2 = draw skeleton only", 0, 2, idCmdSystem::ArgCompletion_Integer<0, 2> );
idCVar r_jointNameScale( "r_jointNameScale", "0.02", CVAR_RENDERER | CVAR_FLOAT, "size of joint names when r_showskel is set to 1" );
idCVar r_jointNameOffset( "r_jointNameOffset", "0.5", CVAR_RENDERER | CVAR_FLOAT, "offset of joint names when r_showskel is set to 1" );

idCVar r_debugLineDepthTest( "r_debugLineDepthTest", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "perform depth test on debug lines" );
idCVar r_debugLineWidth( "r_debugLineWidth", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL, "width of debug lines" );
idCVar r_debugArrowStep( "r_debugArrowStep", "120", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER, "step size of arrow cone line rotation in degrees", 0, 120 );
idCVar r_debugPolygonFilled( "r_debugPolygonFilled", "1", CVAR_RENDERER | CVAR_BOOL, "draw a filled polygon" );

idCVar r_materialOverride( "r_materialOverride", "", CVAR_RENDERER, "overrides all materials", idCmdSystem::ArgCompletion_Decl<DECL_MATERIAL> );

idCVar r_debugRenderToTexture( "r_debugRenderToTexture", "0", CVAR_RENDERER | CVAR_INTEGER, "" );

idCVar stereoRender_enable( "stereoRender_enable", "0", CVAR_INTEGER | CVAR_ARCHIVE, "1 = side-by-side compressed, 2 = top and bottom compressed, 3 = side-by-side, 4 = 720 frame packed, 5 = interlaced, 6 = OpenGL quad buffer" );
idCVar stereoRender_swapEyes( "stereoRender_swapEyes", "0", CVAR_BOOL | CVAR_ARCHIVE, "reverse eye adjustments" );
idCVar stereoRender_deGhost( "stereoRender_deGhost", "0.05", CVAR_FLOAT | CVAR_ARCHIVE, "subtract from opposite eye to reduce ghosting" );

idCVar r_useVirtualScreenResolution( "r_useVirtualScreenResolution", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE | CVAR_NEW, "do 2D rendering at 640x480 and stretch to the current resolution" );

// RB: shadow mapping parameters
idCVar r_useShadowAtlas( "r_useShadowAtlas", "1", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "" );
idCVar r_shadowMapAtlasSize( "r_shadowMapAtlasSize", "8192", CVAR_RENDERER | CVAR_INTEGER | CVAR_INIT | CVAR_NEW, "size of the shadowmap atlas (launch-only). Bigger = the fit-budget affords larger per-light tiles -> sharper shadows in busy scenes. 8192 ~= 256MB depth; 16384 ~= 1GB. In a many-light scene 8192 caps tiles to 256px; 16384 restores 1024px." );
idCVar r_shadowMapFrustumFOV( "r_shadowMapFrustumFOV", "92", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "oversize FOV for point light side matching" );
idCVar r_shadowMapSingleSide( "r_shadowMapSingleSide", "-1", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "only draw a single side (0-5) of point lights" );
idCVar r_shadowMapImageSize( "r_shadowMapImageSize", "1024", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "", 128, 2048 );
idCVar r_shadowMapJitterScale( "r_shadowMapJitterScale", "3", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT | CVAR_NEW, "scale factor for jitter offset (Vogel-disk kernel radius, in shadow texels)" );
//idCVar r_shadowMapBiasScale( "r_shadowMapBiasScale", "0.0001", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "scale factor for jitter bias" );
idCVar r_shadowMapRandomizeJitter( "r_shadowMapRandomizeJitter", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "randomly offset jitter texture each draw" );
idCVar r_shadowMapSamples( "r_shadowMapSamples", "3", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER | CVAR_NEW, "Vogel-disk PCF taps per shadow lookup (fewer = crisper; 3 is the tuned crisp default, 16 the old soft look)", 1, 64 );
idCVar r_shadowMapPCSS( "r_shadowMapPCSS", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL | CVAR_NEW, "PCSS on the shadow-map path: a blocker-search estimates an average blocker depth and scales the PCF filter radius by the similar-triangles penumbra width, so shadows contact-harden (sharp at contact, soft with distance). Reads the raw depth atlas (non-comparison). Intended as the cheap conservative PENUMBRA LOCATOR for the analytic soft-shadow hybrid. 0 = fixed-radius PCF; 1 = PCSS." );
idCVar r_shadowMapPCSSScale( "r_shadowMapPCSSScale", "4", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT | CVAR_NEW, "PCSS light-source size / penumbra scale (drives both the blocker-search radius and the penumbra width). Larger = softer, wider penumbra. Tune per content.", 0.0f, 64.0f );
idCVar r_softShadowMapLod( "r_softShadowMapLod", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER | CVAR_NEW, "shadow-atlas LOD (tile resolution) pinned for soft-wedge PCSS point lights, since standalone PCSS uses the map AS the shadow: 0=1024 (sharpest, ~6x1024^2 atlas/light), 1=512, 2/3=256, 4=128. Lower = crisper penumbra but more atlas budget. Live; takes effect next frame.", 0, 4 );
idCVar r_shadowMapPCSSBias( "r_shadowMapPCSSBias", "0.5", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT | CVAR_NEW, "PCSS normal-offset, in shadow texels: lifts the receiver sample along its surface normal toward the light before projecting, so self-shadow is avoided without a depth bias that would recede the contact (peter-pan). ~0.5 = 2 texels. Too low self-shadows (acne); too high leaks (shadow pulls away).", 0.0f, 8.0f );
idCVar r_shadowMapPCSSAnalyticContact( "r_shadowMapPCSSAnalyticContact", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL | CVAR_NEW, "PCSS quality tier: over the WHOLE PCSS penumbra, run the exact analytic silhouette-edge integral (physically correct contact hardening + a staircase-free edge) and take the more-occluded of it and the PCSS PCF, so edge-less dynamic casters keep their atlas shadow. Must cover the whole penumbra - PCSS and the analytic disagree ~21% in the soft interior, so a partial band would seam. Costly (O(silhouette edges) per penumbra fragment; ~+26ms @2560x1440 measured, reclaimed via r_softShadowVRS); off = cheap standalone PCSS, whose grazing-floor acne bands/staircase ARE the measured in-game turd/step defects (softgate). DEFAULT ON: the exact analytic is the shipped soft-shadow term wherever edge records exist; PCSS remains the edge-less-caster fallback. Needs r_useSoftShadowVolumes 1." );
idCVar r_shadowMapSplits( "r_shadowMapSplits", "3", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "number of splits for cascaded shadow mapping with parallel lights", 0, 4 );
idCVar r_shadowMapSplitWeight( "r_shadowMapSplitWeight", "0.9", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "" );
idCVar r_shadowMapLodScale( "r_shadowMapLodScale", "1.4", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "" );
idCVar r_shadowMapLodBias( "r_shadowMapLodBias", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "" );
idCVar r_shadowMapPolygonFactor( "r_shadowMapPolygonFactor", "2", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "polygonOffset factor for drawing shadow buffer" );
idCVar r_dxShadowMapPolygonOffset( "r_dxShadowMapPolygonOffset", "0.1", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "polygonOffset units for drawing shadow buffer" );
idCVar r_vkShadowMapPolygonOffset( "r_vkShadowMapPolygonOffset", "1", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "polygonOffset units for drawing shadow buffer" );
idCVar r_shadowMapOccluderFacing( "r_shadowMapOccluderFacing", "2", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "0 = front faces, 1 = back faces, 2 = twosided" );
idCVar r_shadowMapRegularDepthBiasScale( "r_shadowMapRegularDepthBiasScale", "0.999", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "shadowmap bias to fight shadow acne for point and spot lights" );
idCVar r_shadowMapSunDepthBiasScale( "r_shadowMapSunDepthBiasScale", "0.999", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "shadowmap bias to fight shadow acne for cascaded shadow mapping with parallel lights" );

// RB: HDR parameters
idCVar r_hdrAutoExposure( "r_hdrAutoExposure", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "EXPENSIVE: enables adapative HDR tone mapping otherwise the exposure is derived by r_exposure" );
idCVar r_hdrAdaptionRate( "r_hdrAdaptionRate", "1", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "The rate of adapting the hdr exposure value`. Defaulted to a second." );
idCVar r_hdrMinLuminance( "r_hdrMinLuminance", "0.02", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "" );
idCVar r_hdrMaxLuminance( "r_hdrMaxLuminance", "0.5", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "" );
idCVar r_hdrKey( "r_hdrKey", "0.015", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "magic exposure key that works well with Doom 3 maps" );
idCVar r_useBloom( "r_useBloom", "1", CVAR_ARCHIVE | CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "add a glare/bloom halo around over-bright (emissive) surfaces" );
idCVar r_hdrContrastDynamicThreshold( "r_hdrContrastDynamicThreshold", "1.0", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "if auto exposure is on, absolute scene luminance above this blooms (1.0 = brighter than SDR white)" );
idCVar r_hdrContrastStaticThreshold( "r_hdrContrastStaticThreshold", "1.0", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "if auto exposure is off, absolute scene luminance above this blooms (1.0 = brighter than SDR white)" );
idCVar r_hdrContrastOffset( "r_hdrContrastOffset", "1.0", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "soft-knee rolloff width for the bloom brightpass; smaller = sharper glare onset" );
idCVar r_hdrGlarePasses( "r_hdrGlarePasses", "8", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "how many times the bloom blur is rendered offscreen. number should be even" );
idCVar r_hdrDebug( "r_hdrDebug", "0", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "show scene luminance as heat map" );

idCVar r_ldrContrastThreshold( "r_ldrContrastThreshold", "1.1", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "" );
idCVar r_ldrContrastOffset( "r_ldrContrastOffset", "3", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "" );

// The old single r_useFilmicPostFX master is split into individual effect toggles.
idCVar r_filmicChromaticAberration( "r_filmicChromaticAberration", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL | CVAR_NEW, "post-process lens chromatic aberration (RGB colour fringing toward the screen edges)" );

idCVar r_forceAmbient( "r_forceAmbient", "0.5", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT | CVAR_NEW, "render additional ambient pass to make the game less dark", 0.0f, 1.0f );

idCVar r_useSSAO( "r_useSSAO", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL | CVAR_NEW, "use screen space ambient occlusion to darken corners" );
idCVar r_ssaoDebug( "r_ssaoDebug", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "" );
idCVar r_ssaoFiltering( "r_ssaoFiltering", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "" );
idCVar r_useHierarchicalDepthBuffer( "r_useHierarchicalDepthBuffer", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "" );

idCVar r_pbrDebug( "r_pbrDebug", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "show which materials have PBR support (green = PBR, red = oldschool D3)" );
idCVar r_showViewEnvprobes( "r_showViewEnvprobes", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "1 = displays the bounding boxes of all view environment probes, 2 = show irradiance" );
idCVar r_showLightGrid( "r_showLightGrid", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "show Quake 3 style light grid points" );

idCVar r_useLightGrid( "r_useLightGrid", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "" );

idCVar r_useDDGI( "r_useDDGI", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL | CVAR_NEW, "ray-traced dynamic diffuse global illumination probes (replaces the baked light grid; requires ray query support; set at startup)" );
idCVar r_useRTReflections( "r_useRTReflections", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL | CVAR_NEW, "ray-traced reflections for low-roughness surfaces (requires ray query support; set at startup)" );
idCVar r_useRTShadows( "r_useRTShadows", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL | CVAR_NEW, "ray-traced hard shadows instead of shadow maps for point + spot lights (requires ray query support; live toggle)" );

// Soft shadow VOLUMES (penumbra wedges, Assarsson & Akenine-Moller) - a world-space, view-
// independent penumbra grown off the stencil silhouette. The hard stencil volume stamps the
// umbra into the light visibility buffer (globalImages->rtShadowMaskImage); a wedge per
// silhouette edge writes the fractional penumbra coverage; the interaction multiplies the
// buffer. Default OFF (WIP): nothing touches the baseline shadow paths until it is proven.
idCVar r_useSoftShadowVolumes( "r_useSoftShadowVolumes", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL | CVAR_NEW, "soft shadow volumes (penumbra wedges off the stencil silhouette) - world-space penumbra, no RT, no crawl (WIP)" );
idCVar r_shadowPenumbraSize( "r_shadowPenumbraSize", "8", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT | CVAR_NEW, "soft shadows: DEFAULT/CAP light emitter radius in world units. Per-light: an authored penumbraSize entity key wins outright; else r_shadowPenumbraAuto derives a per-light radius capped by this value; else (auto 0) every light uses exactly this global (the legacy behaviour, pinned by the gate).", 0.0f, 128.0f );
idCVar r_shadowPenumbraAuto( "r_shadowPenumbraAuto", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL | CVAR_NEW, "derive each light's soft-shadow emitter radius from its own extents when no penumbraSize entity key is authored (task #105: lights sized by their physical emitters). Clamped to [1, r_shadowPenumbraSize] so no light softens MORE than the legacy global. 0 = legacy single global. The gate/repro pin 0: .cap fixtures embed the radius they were captured at." );
idCVar r_shadowPenumbraAutoScale( "r_shadowPenumbraAutoScale", "0.04", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT | CVAR_NEW, "r_shadowPenumbraAuto heuristic: emitter radius = smallest light extent * this, clamped to [1, r_shadowPenumbraSize]. Doom 3 fixtures are small relative to their light volumes: 0.04 puts a 100u accent light at 4u and lets big room lights hit the legacy 8u cap. Penumbra width, cull-cone radius and tile-list population all scale with the emitter radius.", 0.001f, 1.0f );
idCVar r_softShadowFaceCoverage( "r_softShadowFaceCoverage", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL | CVAR_NEW, "soft shadow volumes (DEFAULT): stream the caster's TRIANGLES and take the union coverage of the light disk by casting disk-sample rays against them (SoftShadow_FaceCoverage), instead of the light-silhouette edge integral. This is a low-sample area-light ray query, so it matches the RT reference: a broad penumbra where the silhouette path undershoots off-axis (too-narrow, over-sharp), a hole-free umbra that EMERGES from coverage saturation on open/non-manifold Doom3 geometry (the edge integral drained), no phantom band wedges (the band prepass is bypassed in this mode), and camera-independence. A per-triangle cone/slab cull keeps the cost near the old edge path. Set 0 for the legacy light-silhouette wedge fallback. Needs r_useSoftShadowVolumes 1." );
idCVar r_shadowPenumbraMinWidth( "r_shadowPenumbraMinWidth", "4", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT | CVAR_NEW, "soft shadow volumes: floor on penumbra half-width in world units. Near contact the half-width shrinks toward 0, making coverage hypersensitive to sub-pixel wobble in the depth-reconstructed receiver (motion churn). Flooring it caps the coverage gradient; raise to calm churn, lower for sharper contact.", 0.0f, 64.0f );
idCVar r_softShadowProxyBox( "r_softShadowProxyBox", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "soft shadow volumes: replace a box-shaped caster mesh (its verts hug their AABB) with its 12-triangle AABB proxy at penumbra collection. The mesh is a lossy discretization of that box, so the proxy is equal-or-better and slashes the per-fragment walk record count (a 2000-tri crate -> 12 tris). Detailed/round/L-shaped meshes fail the box-ness test and keep their triangles. Gate-verified (softshadow-cull-lever). 0 = off." );
idCVar r_softShadowUmbraAccumAggressive( "r_softShadowUmbraAccumAggressive", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "umbra-accumulation cull: also run the CONSERVATIVE cell aggregate (light sphere covered by a ball grid; a triangle is culled only when every cell is provably fully blocked by some kept blocker - multi-blocker union with no sampling anywhere; study-audited at zero unblocked rays). Adds a few points of cull over the plain certificates. 0 = certificates only." );
idCVar r_softShadowUmbraAccumMargin( "r_softShadowUmbraAccumMargin", "1.15", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "soft shadow umbra-accumulation cull: the cull tests occlusion of a light disk INFLATED by this factor - a triangle is only dropped when even the enlarged disk is fully blocked, so boundary slivers keep their shadow with margin. 1.0 = exact disk (aggressive), higher = safer/less culled.", 1.0f, 2.0f );
idCVar r_softShadowUmbraAccum( "r_softShadowUmbraAccum", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "soft shadow volumes: per light, drop caster triangles whose whole soft shadow falls inside the shadow of nearer kept geometry (same-light depth-ordered umbra accumulation, validated offline at 31-36% of walk records with every residual over-cull below the walk's own 1/16-disk resolution). Results cached per light; surfaces whose stream changed since the previous frame are excluded (kept and not used as blockers), so moving casters neither pop nor cull. The one-time per-light compute runs when a light's stable caster set first appears (bench warm-up frames / level start). 0 = off." );
idCVar r_softShadowProxyBoxGap( "r_softShadowProxyBoxGap", "0.06", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "soft shadow proxy: box-ness threshold. A mesh is boxed only if its worst vertex sits within this fraction of the AABB diagonal from the nearest AABB face (0 = perfect box). Raise to box more aggressively, lower to be stricter.", 0.0f, 0.5f );
idCVar r_softShadowProxyProfile( "r_softShadowProxyProfile", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "soft shadow proxy: one-shot per-model profiler. Over one frame of penumbra collection, tally each distinct caster MODEL (by name) - its records (tris x times cast), its worst-of-8 AABB-CORNER gap (min vertex->corner distance / diagonal: ~0 = right-angle box, larger = rounded/beveled corners), and its worst face gap - then print the table sorted by records and auto-reset. Use it to find which erebus meshes are true right-angle boxes worth proxying. 1 = print and stay (interactive); 2 = print and quit (headless one-shot); 0 = off.", 0, 2 );
idCVar r_softShadowProxyModel( "r_softShadowProxyModel", "", CVAR_RENDERER | CVAR_NEW, "soft shadow proxy: CURATED single-model box swap. When set to an exact model name (e.g. models/mapobjects/xyz.lwo) or entity name (e.g. func_static_54348), that model - and only that model - is replaced by its 12-tri AABB box at penumbra collection. No geometric test: the specific mesh is identified offline (via r_softShadowProxyProfile) and named here. Empty = no curated swap." );
idCVar r_softShadowProxyInflate( "r_softShadowProxyInflate", "0", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "soft shadow proxy: size adjustment for the CURATED box (r_softShadowProxyModel only). The AABB is scaled about its centre by (1 + this): positive grows the box (cover an occluder the raw AABB under-shoots), negative shrinks it (the AABB corners overhang the true shape). 0 = exact AABB.", -0.9f, 2.0f );
idCVar r_softShadowDebugHash( "r_softShadowDebugHash", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "diagnostic: 1 = print when the per-frame penumbra-wedge fingerprint drifts; 2 = also print per-caster/light lines" );
idCVar r_softShadowTwoSided( "r_softShadowTwoSided", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "diagnostic: soft shadow volumes wedge rasterisation. 1 = two-sided (front+back, coverage halved); 0 = single-sided (hand-wound facing, full coverage). Two-sided was adopted to fight a flicker later traced to a use-after-free - toggle to re-evaluate whether it is still needed." );
idCVar r_softShadowShowMask( "r_softShadowShowMask", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "diagnostic: blit the raw signed penumbra-wedge accumulator (pre-resolve) over the scene, to see wedge coverage directly instead of the lit result" );
idCVar r_softShadowDebugShader( "r_softShadowDebugShader", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "diagnostic for the per-fragment soft path: 0 = real coverage; 1 = flat 0.3 (is the soft path applied?); 2 = frac(worldP.x/64) (is receiver world pos sane?); 3 = swN>0 test (did the edge count reach the shader?)" );
idCVar r_softShadowKeepOffViewCasters( "r_softShadowKeepOffViewCasters", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "soft shadow volumes: keep shadow casters that are outside the view frustum but whose penumbra reaches into it. Off-view casters are otherwise culled by their HARD-shadow bounds, but the penumbra flares beyond those bounds, so culling pops whole wedges in/out as the camera moves. 0 to restore the cull (A/B the flicker)." );
idCVar r_softShadowWedgeFade( "r_softShadowWedgeFade", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "diagnostic: soft shadow volumes silhouette-weight fade. 1 = ramp a wedge in over ~9 degrees as its edge firms into a silhouette; 0 = full weight always (binary silhouette set). The ramp was added to fight wedge popping later traced to a use-after-free." );
idCVar r_softShadowColinearTol( "r_softShadowColinearTol", "0", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "soft shadow volumes: merge consecutive silhouette edges whose mid-vertex deviates from the chord by at most this fraction of the chord length. 0 = merge only EXACTLY colinear runs (bit-exact, e.g. flat faces); raise for more decimation (perspective preserves lines so small values stay below coverage float precision, but nonzero is not strictly lossless).", 0.0f, 0.2f );
idCVar r_softShadowBackfaceCull( "r_softShadowBackfaceCull", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "analytic soft shadows: skip the coverage integral for receivers facing AWAY from the light (N.L <= 0) in the compute term pass. Lossless - the interaction masks both diffuse and specular by saturate(N.L) (half-Lambert off), so a back-facing receiver contributes exactly 0 regardless of the shadow term; softpos writes the world shading normal (its 2nd MRT, same bump decode the interaction uses) so this N.L matches the interaction's ldotN and the ldotN=0 boundary contribution vanishes. Corpus gate 0 defects. 0 = always integrate (A/B toggle)." );
idCVar r_softShadowAreaCull( "r_softShadowAreaCull", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "analytic soft shadows: PORTAL-AREA dead-work cull in the compute term pass. World geometry is one entityDef per portal area (models _area%i); a light's interaction chain contains an area's world entity iff the portal flood (FlowLightThroughPortals) reached it. World fragments in areas the light never reached have NO interaction draw under that light, so their term is provably never read - skip the walk there (term 1.0). Lossless for the skipped class. softpos encodes areaNum+1 alongside the texel-key axis in its normal .w (enc = axis + 4*areaP1, fp16-exact); the term CB carries a 128-bit area mask, all-ones (cull inert) unless this cvar arms it. Non-world receivers (areaP1 0) never skip. 0 = off, 1 = on.", 0, 1 );
idCVar r_softShadowUmbraTiles( "r_softShadowUmbraTiles", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "analytic soft shadows: whole-tile UMBRA sentinel in the tile-bin prepass. While binning, if one triangle provably blocks the ENTIRE light disk for every receiver in a 16x16 tile (triangle-plane separation + per-edge planes tangent to the light sphere, every test carrying the tile's world radius - the classic inner-penumbra construction, fully conservative), the tile is marked umbra and the term/interaction consumers write occlusion 1 without walking the list: the exact value the saturated integral produces. 0 = always walk (A/B toggle)." );
idCVar r_softShadowTileK( "r_softShadowTileK", "256", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER | CVAR_NEW, "analytic soft shadows: per-tile index capacity = the tile-buffer STRIDE (count word + K indices per 16x16 tile). It is BOTH the write cap and the spill threshold: a tile with more than K surviving triangles spills its FULL list to the cluster region and the consumer walks that instead. Smaller K packs the tile slots denser (cache-friendlier walk), larger K spills less - LOSSLESS either way (spill = same conservative integral over k-d cluster leaves, bit-exact). Measured on the per-cap-ISOLATED bench (RADV/RDNA3, 16x16 tiles): 256 best (512 worse on most heavy caps, 128 worse). NOTE the tile-size interplay: at 8x8 tiles the sweep favoured 128 - re-sweep if SW_TILE_SIZE changes, and calibrate per device. ARCHIVE cvar: benches must +set it explicitly or the saved config wins.", 1, 512 );
idCVar r_softShadowDepthOrder( "r_softShadowDepthOrder", "2", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "analytic soft shadows: caster-run emission order, so the walk's saturation/lit early-outs fire before the tail. 0 = collection order; 1 = nearest-light first (legacy depth key); 2 = contribution order - angular size seen from the light (radius/distance), DESCENDING, biggest occluders first (default; 2026-08-26 bench: umbra-dominated caps -21..-34%, pure-penumbra heavies +0..+8% - see task #109). Any mode is union-exact on its own (commutative walk); only the early-out landing point moves.", 0, 2 );
idCVar r_softShadowCullBeforeLoad( "r_softShadowCullBeforeLoad", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "analytic soft shadows (A/B lever): the tile-bin stores each list entry's (centroid, tight radius) as 4 fp16 in a parallel buffer, and the per-fragment walk runs the cone cull from that SEQUENTIAL read - only scatter-loading the 3 verts for cull survivors. Bit-identical (conservative fp16 inflate; gate 0). Measured NET-NEGATIVE on the current walk (the vertex gather is L0-cheap so the deferral saves nothing, while the bin write + walk read add cost) - kept off. Retained because a fewer-survivors change (shorter loop) could flip the balance. 1 = on (bin writes the parallel buffer, walk defers)." );
idCVar r_softShadowInvProbe( "r_softShadowInvProbe", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "diagnostic: per soft light, print the view-culled softSurfHash CHURN count vs a CAMERA-INVARIANT fingerprint (sorted static interacting-entity indices from the interaction chain) churn count, whenever either changes. If viewHash churns climb with camera motion while INVARIANT stays at 0, the surf-cache clears are pure view-culling and camera-independent static collection is the sound fix. Prints only on change; leave 0 in the game.", 0, 1 );
idCVar r_softShadowSamples( "r_softShadowSamples", "16", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "analytic soft shadows: disk RAY COUNT for the coverage term (compute path). Valid: 0, 8, 16, 32. 0 = soft coverage OFF (term 1.0, no penumbra). 8/16/32 select a COMPILE PERMUTATION of the same fp16-packed tile-binned walk (SW_FACE_SAMPLES), so cost is MONOTONIC (8 < 16 < 32, 32 ~2x 16) - not a fork. Fewer samples = coarser 1/N quantum (more banding, cheaper); more = finer (less banding, dearer). The banding doubles as a free readout of the term value. 16 = shipped default (byte-identical). Other values snap to the nearest of 8/16/32; changing it recompiles the term pipeline (a brief hitch). The interaction fragment path (r_softShadowCompute 0) stays at 16. Needs r_softShadowCompute 1.", 0, 32 );
idCVar r_softShadowRotGrid( "r_softShadowRotGrid", "0", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "analytic soft shadows: TEMPORAL-STABILITY of the penumbra. The per-fragment sample-rotation is a very high-frequency hash of the receiver world position; under TAA jitter each screen pixel samples a subpixel-different world point per frame, so the rotation - and the 1/16 coverage quantum - flips every frame and TAA cannot resolve the stepped term (the crawling 'fingerprint'). Snapping the world position to THIS grid (world units) before the hash makes the rotation constant within a cell, so subpixel jitter keeps the same quantum and the bands stop crawling (they become static). Neighbours a cell apart still decorrelate. Try 0.25-1.0. 0 = exact per-position (crawls under TAA). Needs r_softShadowCompute 1.", 0.0f, 8.0f );
idCVar r_softShadowTermBlur( "r_softShadowTermBlur", "0", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "analytic soft shadows: TEMPORAL-STABILITY blur of the term atlas (softblur.cs). The 16-sample coverage quantum shows as a fixed-pattern 'fingerprint' that swims under camera motion; a Gaussian blur whose radius scales with the LOCAL PENUMBRA WIDTH (read from the term gradient - wide/shallow ramp = big radius, hard edge = small) dissolves the steps without softening contact edges. This value = the MAX blur radius in pixels (0 = off). The gather is clamped to each light's atlas rect. Not lossless (the term changes); off by default. Try 4-8. Needs r_softShadowCompute 1.", 0.0f, 32.0f );
idCVar r_softShadowTermBlurScale( "r_softShadowTermBlurScale", "2.0", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "analytic soft shadows: term-blur radius scale. Per-pixel radius = this / |term gradient|, clamped to [1, r_softShadowTermBlur]. Higher = wider blur for a given penumbra slope (more smoothing, softer). The band spacing is ~penumbraWidth/16, so a small multiple of the inverse gradient targets the bands. Only matters when r_softShadowTermBlur > 0.", 0.1f, 32.0f );
idCVar r_softShadowTermSlotCols( "r_softShadowTermSlotCols", "4", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "analytic soft shadows: term-atlas slot grid COLUMNS (atlas width = render width * cols). The scissor packer shelves light rects into this area; more area = more lights on the wave32 compute TERM path (cacheable) instead of the wave64 in-shader integral. Cost = VRAM (R16F, so cols*rows * ~4 MB at 1080p). Clamped so the atlas fits the device texture limit (16384). Takes effect next view (atlas rebuilds). Needs r_softShadowCompute 1.", 1, 8 );
idCVar r_softShadowTermSlotRows( "r_softShadowTermSlotRows", "3", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "analytic soft shadows: term-atlas slot grid ROWS (atlas height = render height * rows). See r_softShadowTermSlotCols.", 1, 12 );
idCVar r_softShadowLitEarlyOut( "r_softShadowLitEarlyOut", "0", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "analytic soft shadows: INTENSITY lit early-out. In the term compute pass, skip the coverage walk (writing term 1.0 = lit) where the light's falloff*projection product at the receiver is below this threshold - the analytic penumbra stretches far into regions the light barely reaches, and the shadow there is multiplied by a near-zero contribution, so cutting it is visually lossless (not bit-exact - the term hash changes). Tune up until the far penumbra fringe disappears without eating visible shadow; typical 0.01-0.05. 0 = exact (skip only where the light is exactly zero). Needs r_softShadowCompute 1.", 0.0f, 1.0f );
idCVar r_softShadowClassify( "r_softShadowClassify", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "analytic soft shadows (M1): per-light WORLD-CELL lit/penumbra classifier. Builds a dense class grid over each light's world bounds (conservative per-triangle cone cull vs the cell AABB, same math as the tile bin) and skips the per-fragment coverage walk for provably-LIT cells - measured 81-86% of the walk WORK is lit fragments that walk ~90 triangles and block nothing. LOSSLESS (lit = integral 0, all-or-nothing, rotation-independent; penumbra walks exactly). 0 = off." );
idCVar r_softShadowClassifyCell( "r_softShadowClassifyCell", "32", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "analytic soft shadows: classifier world-cell size in units. Finer captures more lit-work (16u ~88-91%, 32u ~82-83%) but costs more cells/memory/build; the knee is 16-32u. M2 auto-tunes this live per scene/device/resolution.", 2, 256 );
idCVar r_softShadowStaticStats( "r_softShadowStaticStats", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "Phase 0 attribution: print the per-view soft-shadow caster/record split into CACHEABLE-STATIC (immobile caster AND immobile light -> the analytic term is frame-invariant, re-collected and re-walked every frame for nothing) vs DYNAMIC. Dynamic is further split into light-moved vs caster-moved. Records = tri-stream float4 elements, the walk-relevant mass. Measures the ceiling a cross-frame static cache could remove. Frontend writes, backend prints. Diagnostic only." );
idCVar r_softShadowSkipStatic( "r_softShadowSkipStatic", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "Phase 0 CEILING probe: skip analytic soft-shadow collection for CACHEABLE-STATIC caster x light pairs (immobile both). The render is intentionally WRONG (static shadows missing) - the point is to read the GPU walk timer with those casters removed; the 0->1 delta is the exact GPU cost attributable to the reusable static half. Leave 0 in the game." );
// ---- surface-fold cache PROBE (plan noble-sniffing-rose): playtestable freeze+linear-plane cache ----
// World-anchored per-texel cache of the STATIC part of the coverage integral: per texel store 4 corner
// values of F = totalStaticCoverage - residualCoverage (exact at corners), plus the RESIDUAL occluder
// list (occluders whose solo coverage 2nd-derivative across the texel exceeds the threshold). A cached
// fragment computes bilerp(F corners) + exact walk (residual static + dynamic casters, per-fragment
// rotation). Lazy build: miss -> shipped exact walk + enqueue; a budgeted build CS fills texels across
// frames (static light x static geometry -> built once, reused). APPROXIMATE by design - the verdict is
// the playtest (texel shimmer? border seams?) + the gate defect count.
idCVar r_softShadowSurfCache( "r_softShadowSurfCache", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "analytic soft shadows (PROBE): world-anchored surface-texel cache of the static coverage term (freeze + linear fold, corner-correction reconstruction). Miss path stays the exact shipped walk, so toggling live is always safe; cached texels are approximate (the probe's point). 0 = off (shipped)." );
idCVar r_softShadowSurfCacheInvariant( "r_softShadowSurfCacheInvariant", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "surface-fold cache: rebuild each light's static prefix from a persistent camera-INDEPENDENT set (all ever-seen static casters, key-sorted) instead of the view-culled subset. Kills fingerprint churn from idle camera sway, BUT emits the light's FULL static caster set every frame (MEASURED 2026-08-21: ~6x the view-visible walk in erebus1's many-light rooms -> steady-state 114ms vs ~19ms, and the governor cannot warm out of a slow baseline). Default OFF until the walk-stream is trimmed for warm lights. 1 = experiment." );
idCVar r_softShadowSurfCacheTexel( "r_softShadowSurfCacheTexel", "8", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "surface-fold cache: texel size G in world units. Finer = less fold error + fewer border seams but more texels to build/store. Changing it drops the whole cache (lazy rebuild).", 2, 128 );
idCVar r_softShadowSurfCacheSecondThr( "r_softShadowSurfCacheSecondThr", "0.01", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "surface-fold cache: per-occluder 2nd-derivative fold threshold (coverage units across the texel). The build classifies with the CONTINUOUS closed-form solo coverage (not the 1/N sample quantum), so 0.01 folds any occluder whose contribution is linear across the texel to ~1% coverage - genuinely-linear occluders read ~0. KEEP IT TIGHT: raising it folds curved (2nd/3rd-order) occluders that bilerp cannot reproduce (baked-lighting-class instability). Changing it drops the cache." );
idCVar r_softShadowSurfCacheBudget( "r_softShadowSurfCacheBudget", "4096", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "surface-fold cache: max texels BUILT per frame (each ~5 full walks; requests beyond the budget stay on the exact miss path and retry). Higher warms the cache faster at a build-frame cost.", 64, 65536 );
idCVar r_softShadowSurfCacheCap( "r_softShadowSurfCacheCap", "1048576", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "surface-fold cache: hash-table capacity in texel slots (32 B each; default 1M = 32 MB). Power of two. Full table = new texels stay on the exact miss path.", 65536, 8388608 );
idCVar r_softShadowSurfCacheWarmBudget( "r_softShadowSurfCacheWarmBudget", "65536", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "surface-fold cache WARM path: max receiver texels ONE light claims+builds per warm dispatch (the queue capacity and the build thread count). A light with more receiver texels caches its first this-many and the rest stay on the exact miss walk. Higher lifts the hit rate on large lights but lengthens the warm-at-load burst (each light is one bounded submit+wait) - keep well under a GPU-watchdog dispatch. Pair with a larger …Cap so the whole map's warmed texels fit the shared table.", 4096, 1048576 );
idCVar r_softShadowSurfCachePoolCap( "r_softShadowSurfCachePoolCap", "4194304", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "surface-fold cache: residual-index pool capacity in uints (4 B each; default 4M = 16 MB). Overflow marks further texels walk-always (exact).", 65536, 33554432 );
idCVar r_softShadowSurfCacheErrTol( "r_softShadowSurfCacheErrTol", "0.05", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "surface-fold cache SELF-GATE: the build measures its own reconstruction error at the texel center (corners are exact by construction) and refuses to cache texels that miss the exact coverage by more than this (they stay WALK-ALWAYS = exact). Lower = fewer visible fold defects, more exact-walked texels. ~0.06 = one coverage quantum. Changing it drops the cache." );
idCVar r_softShadowSurfCacheReduced( "r_softShadowSurfCacheReduced", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "surface-fold cache REDUCED-SET mode (grid-or-fold-study, task #49). Instead of folding affine occluders into a bilinear 4-corner F scalar (the mid-penumbra error source), the build keeps EVERY occluder whose cell-max solo coverage clears r_softShadowSurfCacheReducedCutoff as a REPROJECTABLE residual and the hit walks them exactly at the fragment - no bilinear approximation, no umbra break, and the >K texels are capped not abandoned. MEASURED (erebus1_07/09 CPU study): reaches the grid@P discretisation ceiling ~94% at K=32-64 and beats both the grid and bilerp in the visible mid-penumbra. Bounded K caps the walk-always spike (the max/p99 lever). Changing it drops the cache. 0 = shipped fold behaviour." );
idCVar r_softShadowSurfCacheReducedK( "r_softShadowSurfCacheReducedK", "64", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "surface-fold cache reduced-set: max occluders kept + walked per texel (the top-K cap). MEASURED: K=32 ~90%, K=64 ~94% (the discretisation ceiling); K>=64 adds nothing. Texels needing more than K above-cutoff occluders fall to the exact walk (rare). Higher K = more accurate + bigger per-fragment walk on the heaviest texels. Changing it drops the cache.", 1, 1024 );
idCVar r_softShadowSurfCacheReducedCutoff( "r_softShadowSurfCacheReducedCutoff", "0.003", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "surface-fold cache reduced-set: solo-coverage cutoff (coverage units) below which an occluder is dropped from the kept set. The union is dominated by a few large occluders; the tiny-solo tail is redundant (already covered), so dropping it bounds the kept set to the few that matter WITHOUT a GPU sort. Lower = keeps more of the tail (more accurate, larger set); higher = tighter set (cheaper, risks dropping a real contributor). Changing it drops the cache." );
idCVar r_softShadowSurfCacheSkipBin( "r_softShadowSurfCacheSkipBin", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "surface-fold cache: SKIP the per-light tile-bin dispatch for lights whose cache is fully prewarmed. DEFAULT OFF - MEASURED 2026-08-20 (cap0064 bench): the miss fallback is the UNBINNED full walk, and when the seed's casters!=receivers assumption breaks (the bench replay excludes worldspawn from casting, so the world RECEIVERS are never seeded) every fragment misses and TERM exploded 4.61 -> 53.35 ms. The 2.7 ms bin saving needs a BOUNDED miss fallback (or a measured hit-rate gate) before this can default on. 1 = experiment only." );
idCVar r_softShadowSurfCachePrewarm( "r_softShadowSurfCachePrewarm", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "surface-fold cache: PREWARM the cache before the player sees it. On first sight of a light's static set (map load, mover settle) a seed pass claims every texel of the static geometry with a TRIANGLE-PLANE anchor (exact, deterministic - better than the lazy path's fragment-claim anchor), then the build sweeps the table window-by-window until done. 0 = lazy fragment claims only (visible warm-up pops)." );
idCVar r_softShadowSurfCachePrewarmBudget( "r_softShadowSurfCachePrewarmBudget", "65536", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "surface-fold cache prewarm: table slots swept per frame while the GPU has headroom (load/menu/idle frames). Default sweeps a 1M-slot table in 16 frames.", 4096, 1048576 );
idCVar r_softShadowSurfCacheGateMargin( "r_softShadowSurfCacheGateMargin", "0", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "cache-economics TIER-1 gate (task #112): serve a warm texel only when its residual count + probe overhead is under margin x the fragment's tile-list length - i.e. only when serving is provably cheaper than the direct walk. Captures the inversion case: a fat list with a thin residual set (high fold share) serves; dense genuine occlusion walks. The margin doubles near r_softShadowNearRadius (serving must win decisively near the viewer) and relaxes to this base value with distance. 0 = off (always serve on hit, shipped behavior). Typical 1.0.", 0, 8 );
idCVar r_softShadowSurfCacheMinROI( "r_softShadowSurfCacheMinROI", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "cache-economics TIER-2 build gate (task #112): a texel whose build FOLDED fewer than this many occluders (= walk work actually removed) is written WALK-ALWAYS instead of BUILT and emits no residuals - it could never pay for its serve, so its pool space and build budget go to texels that can. 0 = off (build everything, shipped behavior). Typical 8-16.", 0, 1024 );
idCVar r_softShadowSurfCacheEmergencyFPS( "r_softShadowSurfCacheEmergencyFPS", "20", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "surface-fold cache EMERGENCY build (task #87): when the last frame's GPU time is worse than this framerate, the frame is unshippable anyway - dedicate it to cache creation: runtime warm builds FULL-DRAIN their queued texels (all windows, not the first WarmBudget slice) and the prewarm sweep runs the whole table. The question a playtest then answers is not 'is it fast' but 'does it CONVERGE within seconds'. 0 = off (drip budgets).", 0, 240 );
idCVar r_softShadowSurfCacheGpuBudgetUs( "r_softShadowSurfCacheGpuBudgetUs", "8333", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "surface-fold cache prewarm governor: last-frame GPU time (microseconds) under which the prewarm runs at full budget (default 8333 = the 120-FPS line - 'spare compute'). Busier frames drip at the lazy budget so gameplay never hitches on the sweep. Values near the max effectively FORCE full-rate prewarm (the gate uses this: the build frames themselves exceed any realistic frame budget, which would otherwise throttle the sweep mid-warm and make the warm population run-dependent).", 1000, 1000000 );
idCVar r_softShadowSurfCacheViz( "r_softShadowSurfCacheViz", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "surface-fold cache probe visualisation: 0 = off; 1 = checker-tint CACHED texels by cell parity (seam/texel-structure inspection - misses stay untinted); 2 = darken cached texels uniformly (coverage inspection: how much of the frame is served by the cache); 3 = SAVING heatmap (brightness = folded static occluders removed from the walk = per-pixel time saved, 16+ = full bright); 4 = COST-CLASS heatmap (hit full-bright, anchor-reject 0.75, walk-always 0.5, miss 0.25 - where the overhead-payers cluster); 5-8 = miss-split instruments (see softterm.cs.hlsl); 9 = WALK-COST heatmap (log2 fill count 0.25..1.0, zero-walk class bands 0.04 early-out / 0.08 umbra-tile / 0.12 cache-hit / 0.16 other).", 0, 9 );
idCVar r_softShadowSurfCacheTileDyn( "r_softShadowSurfCacheTileDyn", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "surface-fold cache: on a cached HIT, walk the DYNAMIC casters via this fragment's tile-binned list (dynamic tris only) instead of the whole untiled dynamic suffix. Default ON - MEASURED 2026-08-22: the untiled hit-path dynamic walk cost a hit MORE than a tiled miss when dynamic casters are many (the RoE intro cinematic), making the cache a net loss; tiling the hit path's dynamic walk is what lets a hit actually be cheaper than a miss. 0 = old untiled walk (A/B baseline)." );
idCVar r_softShadowSurfCacheWarmLightsPerFrame( "r_softShadowSurfCacheWarmLightsPerFrame", "1", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "surface-fold cache: how many lights the camera-independent drain (re)warms per frame. Default 1 (historically kept to avoid a GPU-watchdog trip on many-caster lights). Higher clears the post-invalidation / post-GC re-warm backlog faster - after a table GC every warmed light re-queues, and at 1/frame that is ~149 frames of stale misses that drag the mean hit rate. Raise to warm the backlog in fewer frames now the term is cheap; too high risks a watchdog trip on a many-caster light in one frame.", 1, 64 );
idCVar r_softShadowSurfCacheMinLight( "r_softShadowSurfCacheMinLight", "500", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "surface-fold cache PER-LIGHT gate: only lights with at least this many STATIC casters run on the surf-cache pipeline; lighter lights use the shipped pipeline. The surf permutation carries a fixed ~0.8ms/frame occupancy tax (extra bindings lower wave occupancy) that caching recoups only on expensive lights - so cheap lights should not pay it. Trades the cache's max-spike win (kept on heavy lights) against the mean-floor tax (removed from cheap lights). MEASURED on the RoE cinematic: 500 keeps the full term-max win (24.2->21.1 ms) while cutting the mean penalty from +0.86 to +0.29 ms vs cache-all. 0 = off (every warmed light uses the cache).", 0, 100000 );
idCVar r_softShadowSurfCacheMinCost( "r_softShadowSurfCacheMinCost", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "surface-fold cache COST GATE: a tile with fewer than this many binned occluders bypasses the cache entirely - no table probe, no slot claim, no build request - and takes the exact walk directly. A cached HIT still walks the residual + dynamic casters, so on a cheap tile the probe + fall-through costs MORE than just walking; this stops paying the lookup where it cannot beat recomputing, and frees the per-frame build budget to warm only the expensive tiles (which drive p99/max). 0 = off (cache every eligible texel, the old behaviour). Tune to the measured hit-vs-walk break-even.", 0, 512 );
idCVar r_softShadowTermLevels( "r_softShadowTermLevels", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "DIAGNOSTIC: quantize the soft-shadow COMPUTE-atlas visibility term to N levels before storing it in the R16F atlas (coarsen the atlas storage). Isolates the atlas-storage / term-precision hypothesis for penumbra banding - lower N until the banding matches what you see; if it never does until very low N, the bands are not term precision (look to Fubini chords or the lighting). Affects only the ATLAS lights (compute path), not the in-shader overflow lights. 0 = off (full precision).", 0, 4096 );
idCVar r_softShadowScanRotate( "r_softShadowScanRotate", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "GATE POSITIVE CONTROL (diagnostic): 1 re-injects the per-fragment rotation into the Fubini scanline term (compute/atlas path), deliberately reproducing the per-pixel 'ants' grain the shipped path (0) removes. Exists so the gate's GateGrain detector can be VALIDATED to fire on the known-bad state - a grain detector that never sees grain is untrustworthy. 0 = shipped (rotation zeroed, no grain).", 0, 1 );
idCVar r_softShadowNearRadius( "r_softShadowNearRadius", "0", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "analytic soft shadows: PLAYER-PROXIMITY exactness guarantee. WIDENED (task #112 tier 3): additionally per-FRAGMENT - receiver points within this distance of the view origin skip the surf-cache probe entirely (exact walk, zero cached-state dependency = popping impossible where most visible), and the tier-1 gate margin doubles near this boundary. Original per-LIGHT semantics kept: soft lights whose origin is within this many world units of the view origin are FORCED onto the exact, non-cached atlas walk - never contributor/surf-cache served (the cache can serve a stale/empty union on a moving camera = the 'area fully lit / skipped shadow' defect) and never lit-early-out blanked. Far lights keep the cache. Also enables the missing-shadow DIAGNOSTIC (com_showFPS >= 2): counts near lights that have a silhouette stream but zero gathered casters (a shadow that should exist but was dropped). 0 = off (shipped, byte-identical). Typical: 384 (immediate vicinity). Larger = more lights forced exact (dearer, safer near the player).", 0, 8192 );
idCVar r_softShadowMinDnRatio( "r_softShadowMinDnRatio", "0.04", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "analytic soft shadows: clamp the light-disk projection denominator dn to at least this fraction of the receiver->light distance. FIXES the grazing-surface 'ants' grain: a near-contact occluder (small dn) projects as distPL/dn, whose per-pixel gradient grows ~1/dn^2, so on a grazing receiver (swP moves fast per pixel) its projected interval swings incoherently -> real high-frequency coverage the exact scanline resolves as grain (the 16-sample path averages it away). Clamping dn caps the amplification. Small approximation: softens only the very sharpest contact hardening. 0 = exact (grain returns, LOSSLESS). 0.04 = default (2x the ~0.02 grain-free knee measured on cap0015; near-lossless). Compute/atlas path only (the in-shader overflow path stays exact). LOSSY, so runtime-toggle-able: set 0 for the exact path.", 0.0f, 1.0f );
idCVar r_softShadowScanChords( "r_softShadowScanChords", "16", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "Fubini scanline CHORD COUNT for the coverage integral (compute/atlas path, SW_SCAN_CHORDS). Valid: 4, 8, 16, 32. The chord count is the penumbra LEVEL count along the sweep axis: too few terraces the gradient (banding, worst at the sparse extreme chords). 4 = coarse (diagnostic: worsens the banding to confirm the source); 16 = default (banding gone in playtest); 32 = visually lossless. Higher = dearer (more per-triangle chord fills + wider grid). Other values snap to the nearest of 4/8/16/32; changing it recompiles the term pipeline (a brief hitch). The in-shader overflow-light path is compile-time 16 (the include default). Needs r_softShadowScanline 1 + r_softShadowCompute 1.", 0, 32 );
idCVar r_softShadowContribCache( "r_softShadowContribCache", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "analytic soft shadows: EVALUATE-ONCE contributor cache (scanline only). Per (world cell, light) the first K fragments run the full walk and RECORD their solo-contributing static triangles (the fragments ARE the evaluation points - no heuristic); at K evaluations the cell serves the recorded union (measured 7-17 tris vs 148-520 walked survivors, coverage-exact at K=64 on the held-out study) + live dynamic casters. Warm-up is incremental (bounded recording pool). All fallbacks are the exact walk. 0 = off (shipped); 1 = on; 2 = SERVE-VERIFY (every served fragment also runs the plain walk, tallies term mismatches into the table header, and RENDERS THE PLAIN TERM - the cache's live self-check instrument).", 0, 2 );
idCVar r_softShadowContribG( "r_softShadowContribG", "8", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "contributor cache: world cell size in units. The held-out study measured the union nearly G-invariant (4..16); 8 balances table load vs warm-up count. Changing it orphans the cache (new keys)." );
idCVar r_softShadowContribPool( "r_softShadowContribPool", "4096", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "contributor cache: max cells RECORDING at once (the incremental warm-up bound). Cells beyond it walk exactly with zero overhead until slots free; caps the recording atomics so entering a fresh area never hitches.", 64, 1048576 );
idCVar r_softShadowContribCap( "r_softShadowContribCap", "262144", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "contributor cache: hash-table capacity in cell slots (power of two; 272 B each, default 256k = ~71 MB). Full table: new cells stay on the exact walk.", 4096, 4194304 );
idCVar r_softShadowContribRefine( "r_softShadowContribRefine", "256", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "contributor cache: 1-in-N served fragments run the exact full walk as a continuous validator (appends missing contributors, revokes stale unions). The full-caster record walk is expensive, so N trades convergence speed vs steady overhead. 0 = validator off (unions freeze once built).", 0, 4096 );
idCVar r_softShadowContribLinger( "r_softShadowContribLinger", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "soft-shadow static-caster classification linger: a light+entity pair unmodified for this many frames counts STATIC (cacheable) even if it moved earlier or its model is dynamic (continuous/particle models stay excluded). Movement reclassifies immediately; the fingerprint/generation machinery re-records - bounded and exact, just slower while things move. 0 = strict legacy rule (ever-moved lights never cache; static-model entities only).", 0, 100000 );
idCVar r_softShadowScanline( "r_softShadowScanline", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "soft-shadow coverage: replace the 16-sample disk union with the FUBINI scanline (an 8-chord x 32-bit interval grid, exact 1D interval-union per chord, OR-unioned across triangles). CPU-validated (SoftShadowUnionGap_test): 96%/94% of penumbra samples within 0.06 of the ray union vs 83% for the best scalar, banding-reduced (no 1/N quantum along a chord), fewer geometric ops than 16 Moller-Trumbore tests. DEFAULT ON (2026-08-25): the 16-sample path's 1/16 quantum was the playtest banding/ants; both the compute term AND the interaction in-shader integral (interactionSM SW_SCANLINE=1) now run Fubini so the whole frame is one exact algorithm - no light silently samples. 0 = legacy sampled walk. Gate must stay green." );
idCVar r_softShadowSurfCacheGrid( "r_softShadowSurfCacheGrid", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "surface-fold cache GRID mode (grid-or-fold Phase 2, task #49): cache the FUBINI 8-chord x 32-bit bit-grid per texel (a parallel buffer) instead of the 4 scalar-corner fold. The build rasterises every static occluder into the grid by OR - fixed 8 uints regardless of occluder count, umbra/silhouette/tilt safe - so it NEVER frees a claimed slot (the scalar fold freed ~72% of claimed texels on curved geometry as walk-always/too-curved, the dominant in-game cache miss). A cached hit ORs the frozen static grid with the fragment's live dynamic-caster grid: term = 1 - popcount((static|dyn)&diskMask)/diskBits. Needs r_softShadowSurfCache 1; recompiles the build+term pipelines. 0 = scalar-corner fold (shipped). A/B lever; gate must stay green." );
idCVar r_softShadowSurfCacheDump( "r_softShadowSurfCacheDump", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "surface-fold cache MEASUREMENT (leave 0): at the end of the warm-at-load burst, read the whole warmed table back to the CPU and print the per-texel SAVING distribution - foldedCount histogram, biggest saving + its world cell, and per-light Sum-folded ranked against each light's static caster/tri cost. One-shot, blocking; for the offline perf investigation only." );
idCVar r_softShadowSurfCacheForceWalk( "r_softShadowSurfCacheForceWalk", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "surface-fold cache DEBUG probe-tax isolation (measurement only, leave 0): 1 = run the full table probe on every gated fragment then FORCE the exact walk instead of the cached hit (run minus cache-off = SETUP + probe-loop tax); 2 = SKIP the probe loop entirely, keeping only the gated-fragment setup (normal load + key derive) then walk (run minus cache-off = SETUP tax alone). Delta of the two isolates the 16-slot table-probe memory cost. 3 = FORCE-HIT ceiling: every gated fragment is served as if its texel were built with ZERO static work (no probe, no grid/residuals; the dynamic tile walk still runs) - the UNREALISTIC 100%-hit upper bound on any hit-rate improvement; the image is wrong, only the frame time means anything. 4 = MISS-ONLY force-hit: the probe runs and real hits serve normally, only fall-through fragments are force-hit - delta vs 0 prices the misses' static walk, delta vs 3 prices the real serve cost.", 0, 4 );
idCVar r_softShadowTileBin( "r_softShadowTileBin", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "analytic soft shadows: per-light compute prepass that bins the caster-triangle stream into 16x16 screen tiles (depth-bounded receiver volume per tile, same conservative cone/slab cull as the fragment walk inflated by the tile radius), so each interaction fragment walks only its tile's triangle list instead of the light's whole stream. Measured motivation: the per-fragment record walk + per-triangle culls are ~69% of the soft-shadow frame cost and are near-identical across a tile. Conservative at every level (over-included triangles are re-culled per fragment; tile overflow and out-of-buffer lights fall back to the full walk), so the shadow is bit-exact vs the full walk. 0 = full per-fragment walk.", 0, 1 );
idCVar r_softShadowBenchExcludeWorld( "r_softShadowBenchExcludeWorld", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "softgate bench only: exclude the static world model from analytic soft-shadow collection, so the .cap's reconstructed captured casters (which already include the world faces) are the SOLE soft casters and are not double-counted. Set by the bench when com_softShadowGateBenchReplay is on; leave 0 in the game." );
idCVar r_softShadowWalkCounters( "r_softShadowWalkCounters", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "walk-attribution instrument: dispatch the soft-shadow TERM compute via its COUNTING permutation (SW_GPU_WALK_COUNTERS=1) so per-fragment cull/MT-survivor counts accumulate into a UAV read back by the gate bench. Separate pipeline - the shipped term shader is byte-identical. Bench/diagnostic only; small atomic cost, leave 0 in the game." );
idCVar r_softShadowCompute( "r_softShadowCompute", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "analytic soft shadows: DEFAULT ON (2026-08-17: wins or ties EVERY corpus scene at 1440p - heavy trio 14.1/17.1/16.2 ms vs 15.6/20.6/18.7 in-shader - after two fixes: (1) the coverage early-outs [valid-receiver + falloff x projection zero test via the light's world planes and its own falloff/projection textures] converge the compute pixel set to the fragment path's, killing the original 1.8x scissor-overcoverage loss; (2) the scissor->SV_Position Y-FLIP in AddLight/BinLight [scissorRect is GL bottom-up] - unflipped, every non-fullscreen light's term rect was mirrored [black lights] and its tile bins misplaced [silent full-walk fallback]. RADV launches the 32-thread groups as WAVE32 with VOPD dual-issue on the packed-fp16 sample loop [verified in ISA: s_and_saveexec_b32 + v_dual_*], which the wave64 pixel shader forecloses - that is the win. Bit-exactness held: full-corpus gate PASS both modes, per-light anaTerm hashes IDENTICAL 0 vs 1; the early-outs are disabled under debug shaders so instruments stay full-field. Costs the R32F term atlas [~235 MB at 1440p]; set 0 to trade the perf for the memory.) Evaluate the face-coverage integral in a per-light COMPUTE pass (wave32-friendly 32-thread groups, so RDNA3 can VOPD dual-issue where the wave64 pixel shader cannot) instead of inside the interaction fragment shader. Pipeline: softpos.{vs,ps} re-rasterises the depth-prepassed surfaces at depth-EQUAL into an RGBA32F EXACT world-position G-buffer (no depth reconstruction - no grazing instability); softterm.cs then runs the SAME shared coverage integral (tile-binned walk + full-walk fallback) per pixel of each soft light's scissor into an R32F term atlas (one screen-size slot per light, all lights dispatched in one batched compute phase); the interaction pixel shader just Loads its light's term texel (rpUser6). Designed to be BIT-EXACT vs the in-shader integral (same include, same float32 positions, R32F storage) - the anaTerm hash must match 0 vs 1. The term is full-rate regardless of raster VRS on the interaction pass. Lights past the atlas slot budget, translucent interactions and non-face modes keep the in-shader integral. Needs r_useSoftShadowVolumes + r_softShadowFaceCoverage.", 0, 1 );
idCVar r_softShadowAsyncCompute( "r_softShadowAsyncCompute", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "analytic soft shadows: MEASURED WASH, keep 0 (2026-08-17: 1440p heavy trio flat vs sync - the graphics work available to overlap [HiZ/ambient/SSAO/atlas, ~1-1.5 ms] is smaller than the submission overhead the split adds; gate PASS + hashes bit-exact in both modes, so the toggle stays as validated plumbing should the overlap window ever grow). Run the batched tile-bin + coverage-term dispatches on the ASYNC COMPUTE queue, overlapping the HiZ/ambient/SSAO/shadow-atlas raster between the softpos fill and the light loop. The graphics work recorded so far is submitted early, the compute queue waits on it, and the frame-end graphics submission waits on the compute instance - so the results are always complete before any interaction draw reads them (bit-exact by construction; the gate + hash A/B must stay identical). Self-disables when the device has no dedicated compute queue family. Toggles live.", 0, 1 );
idCVar r_softShadowVRS( "r_softShadowVRS", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "analytic soft shadows: variable-rate shading on the soft-wedge INTERACTION pass (the expensive per-fragment coverage integral). 0 = off (full 1x1 rate) - DEFAULT by play-test ruling 2026-08-17: the uniform 2x2 coarsening visibly degrades the shadow term ('disabling VRS improves the scene significantly') even though the gate counts zero defects with it; VRS was carrying ~2.4x frame time, which the perf work must now recover at full rate. CONSTANT-rate (uniform coarsening): 1 = 2x2, 2 = 4x4 (clamped to maxFragmentSize; 2x2 on RDNA). 3 = IMAGE-DRIVEN rate image, currently a uniform 2x2 fill. No-ops safely when the device lacks VRS. Levels 1/2 toggle live; level 3 attaches a shading-rate image to hdrFBO at framebuffer-build time, so switching to/from 3 needs a vid_restart (or set it at launch).", 0, 3 );
idCVar r_softShadowBandWedges( "r_softShadowBandWedges", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "soft shadow volumes: also rasterise the per-edge penumbra-wedge frusta into the band (outer-penumbra fringe beyond the point-light shadow volume). 0 = shadow volume only (the combined umbra+penumbra region); the outer-penumbra rim then uses the unshadowed variant. Experimental." );
idCVar r_softShadowAAM( "r_softShadowAAM", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "soft shadow volumes: faithful Assarsson-Akenine-Moeller structure. The disk-coverage wedge computes ABSOLUTE occlusion with no reference, so it under-covers the umbra (never solid) and winds spuriously off-axis (extraneous umbra). AAM instead ANCHORS on the hard shadow: the band prepass encodes umbra=2 (core z-fail volume + shell), penumbra ring=1 (shell only), lit=0. The wedge coverage then runs ONLY in the penumbra ring (stencil==1); the umbra (stencil>=2) is drawn by no pass and so is fully shadowed - a SOLID umbra by construction, not from the integral; the lit remainder (stencil==0) is provably lit. Fixes both under-occlusion (umbra solid) and off-axis over-occlusion (wedge never runs on lit fragments). Forces the band prepass + shell on. Known limit: overlapping penumbra rings can reach stencil 2 and read as umbra (a stencil-count conflation AAM shares); refine with separate stencil bits if it shows." );
idCVar r_softShadowBandMask( "r_softShadowBandMask", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "soft shadow volumes: hard-shadow stencil gate (Carmack's reverse). The prepass marks the engine's z-fail point-light shadow volume (the exact umbra) into stencil; the coverage shader then runs ONLY where stencil>0 and the cheap LIT variant (shadow=1) on the provably-lit remainder. This is a CORRECTNESS fix, not just perf: the disk-coverage integral cannot tell an illusory umbra (the light-relative silhouette loop spuriously encircles the disk from an off-axis receiver) from a real one, so it over-occludes lit points; the hard-shadow gate is the only discriminator (outside the umbra the point is provably lit). Measured on captured Erebus views: extraneous over-occlusion 25->0 / 21->0, mean|cov-truth| -27..-38%, umbra unchanged; a strict gate clips a few outer-penumbra samples (recovered by the inflated shell, r_softShadowBandWedges). 0 = off (coverage on the whole lit surface, over-occludes); 1 = on; 2 = DIAGNOSTIC (band map: red = coverage/stencil>0, green = lit/stencil==0).", 0, 2 );

idCVar r_softShadowEmergentUmbra( "r_softShadowEmergentUmbra", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "soft shadow volumes (AAM): make the umbra EMERGE from the coverage integral instead of stamping the point-light hard shadow black. The point-light silhouette is the ~50% occlusion contour = the MIDDLE of the penumbra, so stamping it umbra crushes the inner half to black (over-hardened, 'blur around a stencil shadow'; measured contact-hardening width ratio 0.50 vs 1.0 truth). With this on, the inflated shell is a SOLID cone over the whole penumbra AND umbra: the coverage runs across all of it and the umbra falls out where it SATURATES to 1. A THIRD interaction pass (stencil NOTEQUAL CORE_BASE, mask ~SHELL_BIT = the low-bits-deviated umbra region) runs the coverage with the centre-lit guard OFF (swCentreLit 0) so it can saturate; the ring pass keeps the guard on. Restores contact hardening (synthetic width ratio 0.50->1.00). KNOWN RESIDUAL (accepted): the coverage integral under-shadows real casters whose near/far-plane cross-section the silhouette edge stream cannot express (F6), so deep umbra can LEAK until that cross-section fix lands - evaluate in-game before defaulting on. 0 = shipped solid-umbra stamp; 1 = emergent umbra." );
idCVar r_softShadowStencilOnly( "r_softShadowStencilOnly", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "soft shadow volumes: CONFINE the total shadow to the stencil core (point-light hard shadow) - draw the penumbra ring LIT and drop the analytic coverage entirely. Measured against the raytraced oracle on the erebus corpus to zero out BOTH pathologies (false shadow 1.4-4.3% -> 0, missing umbra up to 58% -> 0) because those are coverage artifacts the hard stencil cannot produce; the cost is a hard penumbra edge (no gradient) for a later soft pass to fill. The umbra and lit regions are exact; only 0.15<truth<0.85 (the penumbra) is wrong. Use with r_softShadowAAM 1 (band on) and r_softShadowEmergentUmbra 0 (core stays umbra). 0 = coverage as normal; 1 = stencil hard shadow only." );
idCVar r_softShadowContinuous( "r_softShadowContinuous", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "soft shadow volumes: single continuous coverage pass over the whole lit surface with the centre-lit winding-subtraction guard forced ON, and NO hard-shadow stencil gate/core stamp. Trades accuracy (over-lightens, penumbra mean|err| ~0.37) for the least penumbra over-darkening (~0% over-hardened) and the most temporal stability (no per-fragment binary centreBlocked/parity decision). Requires the band pipeline off (r_softShadowAAM 0, r_softShadowBandMask 0). Measured winner on {min over-darkening, max temporal stability} across the erebus corpus; the point-light hard-umbra stamp is what caused both the over-darkening and the frame-to-frame snapping. 0 = off; 1 = on." );
idCVar r_exposure( "r_exposure", "0.5", CVAR_ARCHIVE | CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "HDR exposure or LDR brightness [-4.0 .. 4.0]", -4.0f, 4.0f );
idCVar r_emissiveScale( "r_emissiveScale", "2.5", CVAR_ARCHIVE | CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "brightness multiplier applied to additive (blend add) material stages so emissive FX exceed unit brightness for HDR glow/bloom; 1.0 = off", 1.0f, 16.0f );

// HDR display output (scRGB / extended-sRGB-linear FP16 swapchain). r_hdrOutput is
// read when the swapchain is created, so it needs a vid_restart to take effect.
idCVar r_hdrOutput( "r_hdrOutput", "0", CVAR_ARCHIVE | CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "present to an HDR display via an scRGB FP16 swapchain (needs an HDR monitor + compositor; vid_restart to apply)" );
idCVar r_hdrPaperWhiteNits( "r_hdrPaperWhiteNits", "200", CVAR_ARCHIVE | CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "HDR paper-white luminance in nits (SDR-white maps here)", 80.0f, 1000.0f );
idCVar r_hdrMaxNits( "r_hdrMaxNits", "1000", CVAR_ARCHIVE | CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "HDR display peak luminance in nits (highlights roll off toward this)", 200.0f, 10000.0f );
idCVar r_hdrToneMapOperator( "r_hdrToneMapOperator", "0", CVAR_ARCHIVE | CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "HDR display tone curve (runtime): 0 = linear + highlight shoulder, 1 = Reinhard, 2 = ACES, 3 = Hable/Uncharted2", 0, 3 );
idCVar r_hdrToneMapStrength( "r_hdrToneMapStrength", "1.0", CVAR_ARCHIVE | CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "blend the tone curve toward linear (0 = linear/most visibility, 1 = full operator/most contrast)", 0.0f, 1.0f );

idCVar r_useSSR( "r_useSSR", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL | CVAR_NEW, "" );
idCVar r_ssrJitter( "r_ssrJitter", "0", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "" );
idCVar r_ssrMaxDistance( "r_ssrMaxDistance", "100", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "In meters" );
idCVar r_ssrMaxSteps( "r_ssrMaxSteps", "100", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "" );
idCVar r_ssrStride( "r_ssrStride", "12", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "" );
idCVar r_ssrZThickness( "r_ssrZThickness", "2", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "" );

idCVar r_useTemporalAA( "r_useTemporalAA", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "only disable for debugging" );
idCVar r_taaJitter( "r_taaJitter", "1", CVAR_RENDERER | CVAR_INTEGER | CVAR_NEW, "0: None, 1: MSAA, 2: Halton, 3: R2 Sequence, 4: White Noise" );
idCVar r_taaEnableHistoryClamping( "r_taaEnableHistoryClamping", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "" );
idCVar r_taaClampingFactor( "r_taaClampingFactor", "1.0", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "" );
idCVar r_taaNewFrameWeight( "r_taaNewFrameWeight", "0.1", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "" );
idCVar r_taaMaxRadiance( "r_taaMaxRadiance", "10000", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "" );
idCVar r_taaMotionVectors( "r_taaMotionVectors", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_NEW, "" );

idCVar r_useCRTPostFX( "r_useCRTPostFX", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER | CVAR_NEW, "RetroArch CRT shader: 1 = Matthias CRT, 1 = New Pixie, 2 = Zfast", 0, 3 );
idCVar r_crtCurvature( "r_crtCurvature", "2", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "rounded borders" );
idCVar r_crtVignette( "r_crtVignette", "0.8", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "fading into the borders" );

idCVar r_retroDitherScale( "r_retroDitherScale", "0.3", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "" );

idCVar r_renderMode( "r_renderMode", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER | CVAR_NEW, "0 = Doom, 1 = CGA, 2 = CGA Highres, 3 = Commodore 64, 4 = Commodore 64 Highres, 5 = Amstrad CPC 6128, 6 = Amstrad CPC 6128 Highres, 7 = Sega Genesis, 8 = Sega Genesis Highres, 9 = Sony PSX", 0, 9 );

idCVar r_psxVertexJitter( "r_psxVertexJitter", "0.5", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "", 0.0f, 0.75f );
idCVar r_psxAffineTextures( "r_psxAffineTextures", "1", CVAR_RENDERER | CVAR_FLOAT | CVAR_NEW, "" );

idCVar r_useMaskedOcclusionCulling( "r_useMaskedOcclusionCulling", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_NOCHEAT | CVAR_NEW, "SIMD optimized software culling by Intel" );
// RB end

const char* fileExten[4] = { "tga", "png", "jpg", "exr" };
const char* envDirection[6] = { "_px", "_nx", "_py", "_ny", "_pz", "_nz" };
const char* skyDirection[6] = { "_forward", "_back", "_left", "_right", "_up", "_down" };

DeviceManager* deviceManager = NULL;


bool R_UsePixelatedLook()
{
	return ( r_renderMode.GetInteger() == RENDERMODE_PSX ) || image_pixelLook.GetBool();
}

float R_SSAAScale()
{
	// snap the archived float to the discrete set the menu exposes
	const float s = r_ssaaScale.GetFloat();
	if( s >= 2.0f )
	{
		return 2.0f;
	}
	if( s >= 1.5f )
	{
		return 1.5f;
	}
	return 1.0f;
}

bool R_UseSSAA()
{
	return R_SSAAScale() > 1.0f;
}

bool R_UseTemporalAA()
{
	if( !r_useTemporalAA.GetBool() )
	{
		return false;
	}

	if( r_renderMode.GetInteger() != RENDERMODE_DOOM )
	{
		return false;
	}

	switch( r_antiAliasing.GetInteger() )
	{
		case ANTI_ALIASING_TAA:
			return true;

#if ID_MSAA
		case ANTI_ALIASING_TAA_SMAA_1X:
			return true;
#endif

		default:
			return false;
	}
}

bool R_UseHiZ()
{
	// TODO check for driver problems here
#if defined(__linux__) || defined(__APPLE__)
	if( glConfig.vendor == VENDOR_INTEL && glConfig.gpuType == GPU_TYPE_OTHER )
	{
		// SRS - Disable HiZ to work-around Linux/macOS driver issues on Intel iGPUs
		return false;
	}
#endif
	return r_useHierarchicalDepthBuffer.GetBool();
}

uint R_GetMSAASamples()
{
#if ID_MSAA
	switch( r_antiAliasing.GetInteger() )
	{
		case ANTI_ALIASING_MSAA_2X:
			return 2;

		case ANTI_ALIASING_MSAA_4X:
			return 4;

		default:
			return 1;
	}
#else
	return 1;
#endif
}

/*
=============================
R_SetNewMode

r_fullScreen -2		borderless fullscreen on current monitor at desktop resolution
r_fullScreen -1		borderless window at exact desktop coordinates
r_fullScreen 0		bordered window at exact desktop coordinates
r_fullScreen 1		fullscreen on monitor 1 at r_vidMode
r_fullScreen 2		fullscreen on monitor 2 at r_vidMode
...

r_vidMode -1		use r_customWidth / r_customHeight, even if they don't appear on the mode list
r_vidMode 0			use first mode returned by EnumDisplaySettings()
r_vidMode 1			use second mode returned by EnumDisplaySettings()
...

r_displayRefresh 0	don't specify refresh
r_displayRefresh 70	specify 70 hz, etc
=============================
*/
void R_SetNewMode( const bool fullInit )
{
	// try up to three different configurations

	for( int i = 0 ; i < 3; i++ )
	{
		if( i == 0 /*&& vr_enable.GetInteger() != STEREO3D_QUAD_BUFFER*/ )
		{
			continue;		// don't even try for a stereo mode
		}

		glimpParms_t	parms;

		if( r_fullscreen.GetInteger() <= 0 )
		{
			// use explicit position / size for window
			parms.x = r_windowX.GetInteger();
			parms.y = r_windowY.GetInteger();
			parms.width = r_windowWidth.GetInteger();
			parms.height = r_windowHeight.GetInteger();
			// may still be -1 or -2 to force a borderless window
			parms.fullScreen = r_fullscreen.GetInteger();
			parms.displayHz = 0;		// ignored
		}
		else
		{
			// get the mode list for this monitor
			idList<vidMode_t> modeList;
			if( !R_GetModeListForDisplay( r_fullscreen.GetInteger() - 1, modeList ) )
			{
				idLib::Printf( "Going to safe mode because display not found.\n" );
				goto safeMode;
			}

			if( modeList.Num() < 1 )
			{
				idLib::Printf( "Going to safe mode because mode list failed.\n" );
				goto safeMode;
			}

			parms.x = 0;		// ignored
			parms.y = 0;		// ignored
			parms.fullScreen = r_fullscreen.GetInteger();

			// set the parameters we are trying
			if( r_vidMode.GetInteger() < 0 )
			{
				// try forcing a specific mode, even if it isn't on the list
				parms.width = r_customWidth.GetInteger();
				parms.height = r_customHeight.GetInteger();
				parms.displayHz = r_displayRefresh.GetInteger();
			}
			else
			{
				if( r_vidMode.GetInteger() >= modeList.Num() )
				{
					idLib::Printf( "r_vidMode reset from %i to 0.\n", r_vidMode.GetInteger() );
					r_vidMode.SetInteger( 0 );
				}

				parms.width = modeList[ r_vidMode.GetInteger() ].width;
				parms.height = modeList[ r_vidMode.GetInteger() ].height;
				parms.displayHz = modeList[ r_vidMode.GetInteger() ].displayHz;
			}
		}

		switch( r_antiAliasing.GetInteger() )
		{
#if ID_MSAA
			case ANTI_ALIASING_MSAA_2X:
				parms.multiSamples = 2;
				break;
			case ANTI_ALIASING_MSAA_4X:
				parms.multiSamples = 4;
				break;
#elif defined( _MSC_VER )			// SRS: #pragma warning is MSVC specific
#pragma warning( push )
#pragma warning( disable : 4065 )	// C4065: switch statement contains 'default' but no 'case'
#endif

			default:
				parms.multiSamples = 1;
				break;
		}
#if !ID_MSAA && defined( _MSC_VER )
#pragma warning( pop )
#endif

		if( fullInit )
		{
			// create the context as well as setting up the window

#if defined( VULKAN_USE_PLATFORM_SDL )
			if( VKimp_Init( parms ) )
#else
			if( GLimp_Init( parms ) )
#endif
			{
				ImGuiHook::Init( renderSystem->GetWidth(), renderSystem->GetHeight() );
				break;
			}
		}
		else
		{
			// just rebuild the window

#if defined( VULKAN_USE_PLATFORM_SDL )
			if( VKimp_SetScreenParms( parms ) )
#else
			if( GLimp_SetScreenParms( parms ) )
#endif
			{
				Framebuffer::ResizeFramebuffers();
				ImGuiHook::NotifyDisplaySizeChanged( renderSystem->GetWidth(), renderSystem->GetHeight() );
				break;
			}
		}

		if( i == 2 )
		{
			common->FatalError( "Unable to initialize renderer" );
		}

		if( i == 0 )
		{
			// same settings, no stereo
			continue;
		}

safeMode:
		// if we failed, set everything back to "safe mode"
		// and try again

		// SRS - get the first display with a non-zero mode list, or fail if not found
		int safeDisplay = 0;
		idList<vidMode_t> safeList;
		for( ; ; safeDisplay++ )
		{
			if( !R_GetModeListForDisplay( safeDisplay, safeList ) )
			{
				common->FatalError( "Unable to find a valid display for renderer" );
			}
			else if( safeList.Num() > 0 )
			{
				break;
			}
		}
		// SRS end

		r_vidMode.SetInteger( 0 );
		r_fullscreen.SetInteger( safeDisplay + 1 );
		r_displayRefresh.SetInteger( 0 );
		r_antiAliasing.SetInteger( 0 );
	}
}


/*
=====================
R_ReloadSurface_f

Reload the material displayed by r_showSurfaceInfo
=====================
*/
static void R_ReloadSurface_f( const idCmdArgs& args )
{
	modelTrace_t mt;
	idVec3 start, end;

	if( !tr.primaryView )
	{
		return;
	}

	// start far enough away that we don't hit the player model
	start = tr.primaryView->renderView.vieworg + tr.primaryView->renderView.viewaxis[0] * 16;
	end = start + tr.primaryView->renderView.viewaxis[0] * 1000.0f;
	if( !tr.primaryWorld->Trace( mt, start, end, 0.0f, false ) )
	{
		return;
	}

	common->Printf( "Reloading %s\n", mt.material->GetName() );

	// reload the decl
	mt.material->base->Reload();

	nvrhi::CommandListHandle commandList = deviceManager->GetDevice()->createCommandList();
	commandList->open();

	// reload any images used by the decl
	mt.material->ReloadImages( false, commandList );

	commandList->close();
	deviceManager->GetDevice()->executeCommandList( commandList );
}

/*
==============
R_ListModes_f
==============
*/
static void R_ListModes_f( const idCmdArgs& args )
{
	for( int displayNum = 0 ; ; displayNum++ )
	{
		idList<vidMode_t> modeList;
		if( !R_GetModeListForDisplay( displayNum, modeList ) )
		{
			break;
		}
		for( int i = 0; i < modeList.Num() ; i++ )
		{
			common->Printf( "Monitor %i, mode %3i: %4i x %4i @ %ihz\n", displayNum + 1, i, modeList[i].width, modeList[i].height, modeList[i].displayHz );
		}
	}
}

/*
=============
R_TestImage_f

Display the given image centered on the screen.
testimage <number>
testimage <filename>
=============
*/
void R_TestImage_f( const idCmdArgs& args )
{
	int imageNum;

	if( tr.testVideo )
	{
		delete tr.testVideo;
		tr.testVideo = NULL;
	}
	tr.testImage = NULL;

	if( args.Argc() != 2 )
	{
		return;
	}

	if( idStr::IsNumeric( args.Argv( 1 ) ) )
	{
		imageNum = atoi( args.Argv( 1 ) );
		if( imageNum >= 0 && imageNum < globalImages->images.Num() )
		{
			tr.testImage = globalImages->images[imageNum];
		}
	}
	else
	{
		tr.testImage = globalImages->ImageFromFile( args.Argv( 1 ), TF_DEFAULT, TR_REPEAT, TD_DEFAULT );
	}
}

/*
=============
R_TestVideo_f

Plays the cinematic file in a testImage
=============
*/
void R_TestVideo_f( const idCmdArgs& args )
{
	if( tr.testVideo )
	{
		delete tr.testVideo;
		tr.testVideo = NULL;
	}
	tr.testImage = NULL;

	if( args.Argc() < 2 )
	{
		return;
	}

	tr.testImage = globalImages->ImageFromFile( "_scratch", TF_DEFAULT, TR_REPEAT, TD_DEFAULT );
	tr.testVideo = idCinematic::Alloc();
	// SRS - make sure we have a valid bink, ffmpeg, or RoQ video file, otherwise delete testVideo and return
	// SRS - no need to call ImageForTime() here, playback is handled within idRenderBackend::DBG_TestImage()
	if( !tr.testVideo->InitFromFile( args.Argv( 1 ), true, NULL ) )
	{
		delete tr.testVideo;
		tr.testVideo = NULL;
		tr.testImage = NULL;
		return;
	}

	// try to play the matching wav file
	idStr	wavString = args.Argv( ( args.Argc() == 2 ) ? 1 : 2 );
	wavString.StripFileExtension();
	wavString = wavString + ".wav";
	common->SW()->PlayShaderDirectly( wavString.c_str() );
}

static int R_QsortSurfaceAreas( const void* a, const void* b )
{
	const idMaterial*	ea, *eb;
	int	ac, bc;

	ea = *( idMaterial** )a;
	if( !ea->EverReferenced() )
	{
		ac = 0;
	}
	else
	{
		ac = ea->GetSurfaceArea();
	}
	eb = *( idMaterial** )b;
	if( !eb->EverReferenced() )
	{
		bc = 0;
	}
	else
	{
		bc = eb->GetSurfaceArea();
	}

	if( ac < bc )
	{
		return -1;
	}
	if( ac > bc )
	{
		return 1;
	}

	return idStr::Icmp( ea->GetName(), eb->GetName() );
}


/*
===================
R_ReportSurfaceAreas_f

Prints a list of the materials sorted by surface area
===================
*/
#pragma warning( disable: 6385 ) // This is simply to get pass a false defect for /analyze -- if you can figure out a better way, please let Shawn know...
void R_ReportSurfaceAreas_f( const idCmdArgs& args )
{
	unsigned int		i;
	idMaterial**	list;

	const unsigned int count = declManager->GetNumDecls( DECL_MATERIAL );
	if( count == 0 )
	{
		return;
	}

	list = ( idMaterial** )_alloca( count * sizeof( *list ) );

	for( i = 0 ; i < count ; i++ )
	{
		list[i] = ( idMaterial* )declManager->DeclByIndex( DECL_MATERIAL, i, false );
	}

	qsort( list, count, sizeof( list[0] ), R_QsortSurfaceAreas );

	// skip over ones with 0 area
	for( i = 0 ; i < count ; i++ )
	{
		if( list[i]->GetSurfaceArea() > 0 )
		{
			break;
		}
	}

	for( ; i < count ; i++ )
	{
		// report size in "editor blocks"
		int	blocks = list[i]->GetSurfaceArea() / 4096.0;
		common->Printf( "%7i %s\n", blocks, list[i]->GetName() );
	}
}
#pragma warning( default: 6385 )


/*
==============================================================================

						SCREEN SHOTS

==============================================================================
*/

bool R_ReadPixelsRGB8( nvrhi::IDevice* device, CommonRenderPasses* pPasses, nvrhi::ITexture* texture, nvrhi::ResourceStates textureState, const char* fullname )
{
	nvrhi::TextureDesc desc = texture->getDesc();
	nvrhi::TextureHandle tempTexture;
	nvrhi::FramebufferHandle tempFramebuffer;

	nvrhi::CommandListHandle commandList = device->createCommandList();
	commandList->open();

	if( textureState != nvrhi::ResourceStates::Unknown )
	{
		commandList->beginTrackingTextureState( texture, nvrhi::TextureSubresourceSet( 0, 1, 0, 1 ), textureState );
	}

	switch( desc.format )
	{
		case nvrhi::Format::RGBA8_UNORM:
		case nvrhi::Format::SRGBA8_UNORM:
			tempTexture = texture;
			break;
		default:
			desc.format = nvrhi::Format::SRGBA8_UNORM;
			desc.isRenderTarget = true;
			desc.initialState = nvrhi::ResourceStates::RenderTarget;
			desc.keepInitialState = true;

			tempTexture = device->createTexture( desc );
			tempFramebuffer = device->createFramebuffer( nvrhi::FramebufferDesc().addColorAttachment( tempTexture ) );

			pPasses->BlitTexture( commandList, tempFramebuffer, texture );
	}

	nvrhi::StagingTextureHandle stagingTexture = device->createStagingTexture( desc, nvrhi::CpuAccessMode::Read );
	commandList->copyTexture( stagingTexture, nvrhi::TextureSlice(), tempTexture, nvrhi::TextureSlice() );

	if( textureState != nvrhi::ResourceStates::Unknown )
	{
		commandList->setTextureState( texture, nvrhi::TextureSubresourceSet( 0, 1, 0, 1 ), textureState );
		commandList->commitBarriers();
	}

	commandList->close();
	device->executeCommandList( commandList );

	size_t rowPitch = 0;
	void* pData = device->mapStagingTexture( stagingTexture, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch );

	if( !pData )
	{
		return false;
	}

	uint32_t* newData = nullptr;

	if( rowPitch != desc.width * 4 )
	{
		newData = new uint32_t[desc.width * desc.height];

		for( uint32_t row = 0; row < desc.height; row++ )
		{
			memcpy( newData + row * desc.width, static_cast<char*>( pData ) + row * rowPitch, desc.width * sizeof( uint32_t ) );
		}

		pData = newData;
	}

	byte* data = static_cast<byte*>( pData );

#if 0
	// fill with red for debugging
	for( int i = 0; i < ( desc.width * desc.height ); i++ )
	{
		data[ i * 4 + 0 ] = 255;
		data[ i * 4 + 1 ] = 0;
		data[ i * 4 + 2 ] = 0;
	}
#endif

	// fix alpha
	for( uint32_t i = 0; i < ( desc.width * desc.height ); i++ )
	{
		data[ i * 4 + 3 ] = 0xff;
	}

	// Save screen shots to fs_savepath (the writable user dir), never fs_basepath (the read-only install /
	// repo tree - dumps landing there is a surprise and pollutes the checkout). Same rule on every platform.
	R_WritePNG( fullname, static_cast<byte*>( pData ), 4, desc.width, desc.height, "fs_savepath" );

	if( newData )
	{
		delete[] newData;
		newData = nullptr;
	}

	device->unmapStagingTexture( stagingTexture );

	return true;
}

bool R_ReadPixelsRGB16F( nvrhi::IDevice* device, CommonRenderPasses* pPasses, nvrhi::ITexture* texture, nvrhi::ResourceStates textureState, byte** pic, int picWidth, int picHeight, bool filterCorruption )
{
	nvrhi::TextureDesc desc = texture->getDesc();
	nvrhi::TextureHandle tempTexture;
	nvrhi::FramebufferHandle tempFramebuffer;

#if 0
	if( desc.width != picWidth || desc.height != picHeight )
	{
		return false;
	}
#endif

	nvrhi::CommandListHandle commandList = device->createCommandList();
	commandList->open();

	if( textureState != nvrhi::ResourceStates::Unknown )
	{
		commandList->beginTrackingTextureState( texture, nvrhi::TextureSubresourceSet( 0, 1, 0, 1 ), textureState );
	}

	switch( desc.format )
	{
		case nvrhi::Format::RGBA16_FLOAT:
			tempTexture = texture;
			break;
		default:
			desc.format = nvrhi::Format::RGBA16_FLOAT;
			desc.isRenderTarget = true;
			desc.initialState = nvrhi::ResourceStates::RenderTarget;
			desc.keepInitialState = true;

			tempTexture = device->createTexture( desc );
			tempFramebuffer = device->createFramebuffer( nvrhi::FramebufferDesc().addColorAttachment( tempTexture ) );

			pPasses->BlitTexture( commandList, tempFramebuffer, texture );
	}

	nvrhi::StagingTextureHandle stagingTexture = device->createStagingTexture( desc, nvrhi::CpuAccessMode::Read );
	commandList->copyTexture( stagingTexture, nvrhi::TextureSlice(), tempTexture, nvrhi::TextureSlice() );

	if( textureState != nvrhi::ResourceStates::Unknown )
	{
		commandList->setTextureState( texture, nvrhi::TextureSubresourceSet( 0, 1, 0, 1 ), textureState );
		commandList->commitBarriers();
	}

	commandList->close();
	device->executeCommandList( commandList );

	size_t rowPitch = 0;
	void* pData = device->mapStagingTexture( stagingTexture, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch );

	if( !pData )
	{
		return false;
	}

	uint16_t* newData = nullptr;

	if( rowPitch != desc.width * 8 )
	{
		newData = new uint16_t[desc.width * desc.height * 2];

		for( uint32_t row = 0; row < desc.height; row++ )
		{
			memcpy( newData + row * desc.width, static_cast<char*>( pData ) + row * rowPitch, desc.width * sizeof( uint16_t ) * 4 );
		}

		pData = newData;
	}

	int pix = picWidth * picHeight;
	const int bufferSize = pix * 3 * 2;

	void* floatRGB16F = R_StaticAlloc( bufferSize );
	*pic = ( byte* ) floatRGB16F;

	// copy from RGBA16F to RGB16F
	uint16_t* data = static_cast<uint16_t*>( pData );
	uint16_t* outData = static_cast<uint16_t*>( floatRGB16F );

#if 0
	for( int i = 0; i < ( desc.width * desc.height ); i++ )
	{
		outData[ i * 3 + 0 ] = F32toF16( 1 );
		outData[ i * 3 + 1 ] = F32toF16( 0 );
		outData[ i * 3 + 2 ] = F32toF16( 0 );
	}
#endif

	for( uint32_t i = 0; i < ( desc.width * desc.height ); i++ )
	{
		outData[ i * 3 + 0 ] = data[ i * 4 + 0 ];
		outData[ i * 3 + 1 ] = data[ i * 4 + 1 ];
		outData[ i * 3 + 2 ] = data[ i * 4 + 2 ];
	}

	// RB: filter out garbage and reset it to black
	// this is a rare case but with a high visual impact
	// NOTE: this luminance ceiling is tuned for ENVPROBE captures (~0.5-4.0); a gameplay HDR
	// frame legitimately exceeds it on any bright emissive, so screenshot-style captures pass
	// filterCorruption = false to keep the honest pixel values (including NaNs - they are data).
	bool isCorrupted = false;

	const idVec3 LUMINANCE_LINEAR( 0.299f, 0.587f, 0.144f );
	idVec3 rgb;

	for( uint32_t i = 0; filterCorruption && i < ( desc.width * desc.height ); i++ )
	{
		rgb.x = F16toF32( outData[ i * 3 + 0 ] );
		rgb.y = F16toF32( outData[ i * 3 + 1 ] );
		rgb.z = F16toF32( outData[ i * 3 + 2 ] );

		if( IsNAN( rgb.x ) || IsNAN( rgb.y ) || IsNAN( rgb.z ) )
		{
			isCorrupted = true;
			break;
		}

		// captures within the Doom 3 main campaign usually have a luminance of ~ 0.5 - 4.0
		// the threshold is a bit higher and might need to be adapted for total conversion content
		float luminance = rgb * LUMINANCE_LINEAR;
		if( luminance > 30.0f )
		{
			isCorrupted = true;
			break;
		}
	}

	if( isCorrupted )
	{
		for( uint32_t i = 0; i < ( desc.width * desc.height ); i++ )
		{
			outData[ i * 3 + 0 ] = F32toF16( 0 );
			outData[ i * 3 + 1 ] = F32toF16( 0 );
			outData[ i * 3 + 2 ] = F32toF16( 0 );
		}
	}

	if( newData )
	{
		delete[] newData;
		newData = nullptr;
	}

	device->unmapStagingTexture( stagingTexture );

	return ( !isCorrupted );
}

/*
==================
R_ReadPixelsR32F

Reads one channel of `texture` back to CPU at full 32-bit float precision. The source is blitted into an
RGBA32_FLOAT render target (the blit SAMPLES the source, so a DEPTH texture lands its depth in R), staged,
and the R channel returned as a picWidth*picHeight float array (R_StaticAlloc; caller frees). Used by the
soft-shadow scene capture to store the depth buffer without the half-float precision loss of R_ReadPixelsRGB16F.
==================
*/
bool R_ReadPixelsR32F( nvrhi::IDevice* device, CommonRenderPasses* pPasses, nvrhi::ITexture* texture, nvrhi::ResourceStates textureState, float** pic, int picWidth, int picHeight )
{
	nvrhi::TextureDesc desc = texture->getDesc();

	nvrhi::CommandListHandle commandList = device->createCommandList();
	commandList->open();

	if( textureState != nvrhi::ResourceStates::Unknown )
	{
		commandList->beginTrackingTextureState( texture, nvrhi::TextureSubresourceSet( 0, 1, 0, 1 ), textureState );
	}

	// always blit into an RGBA32F RT: the source is a depth (or otherwise non-RGBA32F) texture, and the blit
	// samples it, so full-precision depth ends up in R.
	desc.format = nvrhi::Format::RGBA32_FLOAT;
	desc.isRenderTarget = true;
	desc.isTypeless = false;
	desc.initialState = nvrhi::ResourceStates::RenderTarget;
	desc.keepInitialState = true;

	nvrhi::TextureHandle tempTexture = device->createTexture( desc );
	nvrhi::FramebufferHandle tempFramebuffer = device->createFramebuffer( nvrhi::FramebufferDesc().addColorAttachment( tempTexture ) );
	pPasses->BlitTexture( commandList, tempFramebuffer, texture );

	nvrhi::StagingTextureHandle stagingTexture = device->createStagingTexture( desc, nvrhi::CpuAccessMode::Read );
	commandList->copyTexture( stagingTexture, nvrhi::TextureSlice(), tempTexture, nvrhi::TextureSlice() );

	if( textureState != nvrhi::ResourceStates::Unknown )
	{
		commandList->setTextureState( texture, nvrhi::TextureSubresourceSet( 0, 1, 0, 1 ), textureState );
		commandList->commitBarriers();
	}

	commandList->close();
	device->executeCommandList( commandList );

	size_t rowPitch = 0;
	void* pData = device->mapStagingTexture( stagingTexture, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch );
	if( !pData )
	{
		return false;
	}

	float* out = ( float* )R_StaticAlloc( ( size_t )picWidth * picHeight * sizeof( float ) );
	*pic = out;
	const char* base = static_cast<const char*>( pData );
	const int rows = Min( ( int )desc.height, picHeight );
	const int cols = Min( ( int )desc.width, picWidth );
	for( int y = 0; y < rows; y++ )
	{
		const float* row = reinterpret_cast<const float*>( base + ( size_t )y * rowPitch );
		for( int x = 0; x < cols; x++ )
		{
			out[y * picWidth + x] = row[x * 4 + 0];		// R channel
		}
	}

	device->unmapStagingTexture( stagingTexture );
	return true;
}

// like R_ReadPixelsR32F but keeps all four channels (used by the soft-shadow gate to read the exact
// softpos receiver world position - the point the shipped shader actually shaded)
bool R_ReadPixelsRGBA32F( nvrhi::IDevice* device, CommonRenderPasses* pPasses, nvrhi::ITexture* texture, nvrhi::ResourceStates textureState, float** pic, int picWidth, int picHeight )
{
	nvrhi::TextureDesc desc = texture->getDesc();

	nvrhi::CommandListHandle commandList = device->createCommandList();
	commandList->open();

	if( textureState != nvrhi::ResourceStates::Unknown )
	{
		commandList->beginTrackingTextureState( texture, nvrhi::TextureSubresourceSet( 0, 1, 0, 1 ), textureState );
	}

	desc.format = nvrhi::Format::RGBA32_FLOAT;
	desc.isRenderTarget = true;
	desc.isTypeless = false;
	desc.initialState = nvrhi::ResourceStates::RenderTarget;
	desc.keepInitialState = true;

	nvrhi::TextureHandle tempTexture = device->createTexture( desc );
	nvrhi::FramebufferHandle tempFramebuffer = device->createFramebuffer( nvrhi::FramebufferDesc().addColorAttachment( tempTexture ) );
	pPasses->BlitTexture( commandList, tempFramebuffer, texture );

	nvrhi::StagingTextureHandle stagingTexture = device->createStagingTexture( desc, nvrhi::CpuAccessMode::Read );
	commandList->copyTexture( stagingTexture, nvrhi::TextureSlice(), tempTexture, nvrhi::TextureSlice() );

	if( textureState != nvrhi::ResourceStates::Unknown )
	{
		commandList->setTextureState( texture, nvrhi::TextureSubresourceSet( 0, 1, 0, 1 ), textureState );
		commandList->commitBarriers();
	}

	commandList->close();
	device->executeCommandList( commandList );

	size_t rowPitch = 0;
	void* pData = device->mapStagingTexture( stagingTexture, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch );
	if( !pData )
	{
		return false;
	}

	float* out = ( float* )R_StaticAlloc( ( size_t )picWidth * picHeight * 4 * sizeof( float ) );
	*pic = out;
	const char* base = static_cast<const char*>( pData );
	const int rows = Min( ( int )desc.height, picHeight );
	const int cols = Min( ( int )desc.width, picWidth );
	for( int y = 0; y < rows; y++ )
	{
		const float* row = reinterpret_cast<const float*>( base + ( size_t )y * rowPitch );
		for( int x = 0; x < cols; x++ )
		{
			out[( y * picWidth + x ) * 4 + 0] = row[x * 4 + 0];
			out[( y * picWidth + x ) * 4 + 1] = row[x * 4 + 1];
			out[( y * picWidth + x ) * 4 + 2] = row[x * 4 + 2];
			out[( y * picWidth + x ) * 4 + 3] = row[x * 4 + 3];
		}
	}

	device->unmapStagingTexture( stagingTexture );
	return true;
}

/*
==================
R_CaptureHDRScreenshot

Writes the current FP16 scene buffer (pre-tonemap linear HDR) to screenshots/<baseName>.exr.
This is the capture half of the com_autoCapture harness (common_frame.cpp): unlike the SDR
"screenshot" command this preserves the real radiance values, so shadow/light output can be
verified numerically at an exact saved viewpoint without a display or an SDR tonemap in the way.
==================
*/
void R_CaptureHDRScreenshot( const char* baseName )
{
	idImage* img = globalImages->currentRenderHDRImage;
	if( img == NULL || img->GetTextureHandle() == nullptr )
	{
		common->Warning( "R_CaptureHDRScreenshot: no HDR render image" );
		return;
	}

	const int w = img->GetUploadWidth();
	const int h = img->GetUploadHeight();

	byte* rgb16f = NULL;
	if( !R_ReadPixelsRGB16F( deviceManager->GetDevice(), &backEnd.GetCommonPasses(), img->GetTextureHandle(),
							 nvrhi::ResourceStates::ShaderResource, &rgb16f, w, h, false ) || rgb16f == NULL )
	{
		common->Warning( "R_CaptureHDRScreenshot: HDR readback failed" );
		if( rgb16f != NULL )
		{
			R_StaticFree( rgb16f );
		}
		return;
	}

	idStr fileName;
	fileName.Format( "screenshots/%s.exr", baseName );
	R_WriteEXR( fileName.c_str(), rgb16f, 3, w, h, "fs_savepath" );
	R_StaticFree( rgb16f );

	common->Printf( "Wrote HDR capture %s (%ix%i)\n", fileName.c_str(), w, h );
}

/*
==================
TakeScreenshot

Move to tr_imagefiles.c...

If ref == NULL, common->UpdateScreen will be used
==================
*/
void idRenderSystemLocal::TakeScreenshot( int widthIgnored, int heightIgnored, const char* fileName, renderView_t* ref )
{
	takingScreenshot = true;

	// make sure the game / draw thread has completed
	commonLocal.WaitGameThread();

	// discard anything currently on the list
	tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );

	// SRS - Update finishSyncTime so frame-over-frame timers display correctly for screenshots
	commonLocal.frameTiming.finishSyncTime = Sys_Microseconds();

	if( ref )
	{
		// ref is only used by envShot, Event_camShot, etc to grab screenshots of things in the world,
		// so this omits the hud and other effects
		tr.primaryWorld->RenderScene( ref );
	}
	else
	{
		// build all the draw commands without running a new game tic
		commonLocal.Draw();
	}
	// this should exit right after vsync, with the GPU idle and ready to draw
	const emptyCommand_t* cmd = tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );

	// get the GPU busy with new commands
	tr.RenderCommandBuffers( cmd );

	// discard anything currently on the list (this triggers SwapBuffers)
	tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );

	R_ReadPixelsRGB8( deviceManager->GetDevice(), &backEnd.GetCommonPasses(), globalImages->ldrImage->GetTextureHandle() , nvrhi::ResourceStates::RenderTarget, fileName );

	// discard anything currently on the list
	tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );

	takingScreenshot = false;
}

// RB: TODO FINISH or REMOVE
byte* idRenderSystemLocal::CaptureRenderToBuffer( int width, int height, renderView_t* ref )
{
	byte*		buffer;

	takingScreenshot = true;

	int pix = width * height;
	//const int bufferSize = pix * 3 * 2;

	// HDR only for now
	//if( exten == EXR )
	{
		buffer = ( byte* )R_StaticAlloc( pix * 3 * 2 );
	}
	//else if( exten == PNG )
	//{
	//	buffer = ( byte* )R_StaticAlloc( pix * 3 );
	//}

	//R_ReadTiledPixels( width, height, buffer, ref );

	takingScreenshot = false;

	return buffer;
}

/*
==================
R_ScreenshotFilename

Returns a filename with digits appended
if we have saved a previous screenshot, don't scan
from the beginning, because recording demo avis can involve
thousands of shots
==================
*/
void R_ScreenshotFilename( int& lastNumber, const char* base, idStr& fileName )
{
	bool restrict = cvarSystem->GetCVarBool( "fs_restrict" );
	cvarSystem->SetCVarBool( "fs_restrict", false );

	lastNumber++;
	if( lastNumber > 99999 )
	{
		lastNumber = 99999;
	}
	for( ; lastNumber < 99999 ; lastNumber++ )
	{

		// RB: added date to screenshot name
#if 0
		int	frac = lastNumber;
		int	a, b, c, d, e;

		a = frac / 10000;
		frac -= a * 10000;
		b = frac / 1000;
		frac -= b * 1000;
		c = frac / 100;
		frac -= c * 100;
		d = frac / 10;
		frac -= d * 10;
		e = frac;

		sprintf( fileName, "%s%i%i%i%i%i.png", base, a, b, c, d, e );
#else
		time_t aclock;
		time( &aclock );
		struct tm* t = localtime( &aclock );

		sprintf( fileName, "%s%s-%04d%02d%02d-%02d%02d%02d-%03d.png", base, "rbdoom-3-bfg",
				 1900 + t->tm_year, 1 + t->tm_mon, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec, lastNumber );
#endif
		// RB end
		if( lastNumber == 99999 )
		{
			break;
		}
		int len = fileSystem->ReadFile( fileName, NULL, NULL );
		if( len <= 0 )
		{
			break;
		}
		// check again...
	}
	cvarSystem->SetCVarBool( "fs_restrict", restrict );
}

/*
==================
R_BlendedScreenShot

screenshot
screenshot [filename]
screenshot [width] [height]
==================
*/
void R_ScreenShot_f( const idCmdArgs& args )
{
	static int lastNumber = 0;
	idStr checkname;

	int width = renderSystem->GetWidth();
	int height = renderSystem->GetHeight();
	int	blends = 0;

	switch( args.Argc() )
	{
		case 1:
			width = renderSystem->GetWidth();
			height = renderSystem->GetHeight();
			blends = 1;
			R_ScreenshotFilename( lastNumber, "screenshots/", checkname );
			break;

		case 2:
			width = renderSystem->GetWidth();
			height = renderSystem->GetHeight();
			blends = 1;
			checkname = args.Argv( 1 );
			break;

		case 3:
			width = atoi( args.Argv( 1 ) );
			height = atoi( args.Argv( 2 ) );
			blends = 1;
			R_ScreenshotFilename( lastNumber, "screenshots/", checkname );
			break;

		default:
			common->Printf( "usage: screenshot\n       screenshot <filename>\n       screenshot <width> <height>" );
			return;
	}

	// put the console away
	console->Close();

	tr.TakeScreenshot( width, height, checkname, NULL );

	common->Printf( "Wrote %s\n", checkname.c_str() );
}



/*
==================
R_EnvShot_f

envshot <basename>

Saves out env/<basename>_ft.tga, etc

RB: This is outdated and probably a relict from Rage. It could be updated to dump panorama images for tools like Blender or Substance Painter
==================
*/
void R_EnvShot_f( const idCmdArgs& args )
{
	idStr		fullname;
	const char*	baseName;
	int			i;
	idMat3		axis[6], oldAxis;
	renderView_t	ref;
	viewDef_t	primary;
	int			blends;
	const char*  extension;
	int			size;
	int         res_w, res_h, old_fov_x, old_fov_y;

	res_w = renderSystem->GetWidth();
	res_h = renderSystem->GetHeight();

	if( args.Argc() != 2 && args.Argc() != 3 && args.Argc() != 4 )
	{
		common->Printf( "USAGE: envshot <basename> [size] [blends]\n" );
		return;
	}
	baseName = args.Argv( 1 );

	blends = 1;
	if( args.Argc() == 4 )
	{
		size = atoi( args.Argv( 2 ) );
		blends = atoi( args.Argv( 3 ) );
	}
	else if( args.Argc() == 3 )
	{
		size = atoi( args.Argv( 2 ) );
		blends = 1;
	}
	else
	{
		size = 256;
		blends = 1;
	}

	if( !tr.primaryView )
	{
		common->Printf( "No primary view.\n" );
		return;
	}

	primary = *tr.primaryView;

	memset( &axis, 0, sizeof( axis ) );

	// +X
	axis[0][0][0] = 1;
	axis[0][1][2] = 1;
	axis[0][2][1] = 1;

	// -X
	axis[1][0][0] = -1;
	axis[1][1][2] = -1;
	axis[1][2][1] = 1;

	// +Y
	axis[2][0][1] = 1;
	axis[2][1][0] = -1;
	axis[2][2][2] = -1;

	// -Y
	axis[3][0][1] = -1;
	axis[3][1][0] = -1;
	axis[3][2][2] = 1;

	// +Z
	axis[4][0][2] = 1;
	axis[4][1][0] = -1;
	axis[4][2][1] = 1;

	// -Z
	axis[5][0][2] = -1;
	axis[5][1][0] = 1;
	axis[5][2][1] = 1;

	// let's get the game window to a "size" resolution
	if( ( res_w != size ) || ( res_h != size ) )
	{
		cvarSystem->SetCVarInteger( "r_windowWidth", size );
		cvarSystem->SetCVarInteger( "r_windowHeight", size );
		R_SetNewMode( false ); // the same as "vid_restart"
	} // FIXME that's a hack!!

	// so we return to that axis and fov after the fact.
	oldAxis = primary.renderView.viewaxis;
	old_fov_x = primary.renderView.fov_x;
	old_fov_y = primary.renderView.fov_y;

	for( i = 0 ; i < 6 ; i++ )
	{
		ref = primary.renderView;

		extension = envDirection[ i ];

		ref.fov_x = ref.fov_y = 90;
		ref.viewaxis = axis[i];
		fullname.Format( "env/%s%s", baseName, extension );

		tr.TakeScreenshot( size, size, fullname, &ref );
	}

	// restore the original resolution, axis and fov
	ref.viewaxis = oldAxis;
	ref.fov_x = old_fov_x;
	ref.fov_y = old_fov_y;
	cvarSystem->SetCVarInteger( "r_windowWidth", res_w );
	cvarSystem->SetCVarInteger( "r_windowHeight", res_h );
	R_SetNewMode( false ); // the same as "vid_restart"

	common->Printf( "Wrote a env set with the name %s\n", baseName );
}

//============================================================================

void R_TransformCubemap( const char* orgDirection[6], const char* orgDir, const char* destDirection[6], const char* destDir, const char* baseName )
{
	idStr fullname;
	int			i;
	bool        errorInOriginalImages = false;
	byte*		buffers[6];
	int			width = 0, height = 0;

	for( i = 0 ; i < 6 ; i++ )
	{
		// read every image images
		fullname.Format( "%s/%s%s.tga", orgDir, baseName, orgDirection[i] );
		common->Printf( "loading %s\n", fullname.c_str() );
		const bool captureToImage = false;
		common->UpdateScreen( captureToImage );
		R_LoadImage( fullname, &buffers[i], &width, &height, NULL, true, NULL );

		//check if the buffer is troublesome
		if( !buffers[i] )
		{
			common->Printf( "failed.\n" );
			errorInOriginalImages = true;
		}
		else if( width != height )
		{
			common->Printf( "wrong size pal!\n\n\nget your shit together and set the size according to your images!\n\n\ninept programmers are inept!\n" );
			errorInOriginalImages = true; // yeah, but don't just choke on a joke!
		}
		else
		{
			errorInOriginalImages = false;
		}

		if( errorInOriginalImages )
		{
			errorInOriginalImages = false;
			for( i-- ; i >= 0 ; i-- )
			{
				Mem_Free( buffers[i] ); // clean up every buffer from this stage down
			}

			return;
		}

		// apply rotations and flips
		R_ApplyCubeMapTransforms( i, buffers[i], width );

		//save the images with the appropiate skybox naming convention
		fullname.Format( "%s/%s/%s%s.tga", destDir, baseName, baseName, destDirection[i] );
		common->Printf( "writing %s\n", fullname.c_str() );
		common->UpdateScreen( false );
		R_WriteTGA( fullname, buffers[i], width, width, false, "fs_basepath" );
	}

	for( i = 0 ; i < 6 ; i++ )
	{
		if( buffers[i] )
		{
			Mem_Free( buffers[i] );
		}
	}
}

/*
==================
R_TransformEnvToSkybox_f

R_TransformEnvToSkybox_f <basename>

transforms env textures (of the type px, py, pz, nx, ny, nz)
to skybox textures ( forward, back, left, right, up, down)
==================
*/
void R_TransformEnvToSkybox_f( const idCmdArgs& args )
{
	if( args.Argc() != 2 )
	{
		common->Printf( "USAGE: envToSky <basename>\n" );
		return;
	}

	R_TransformCubemap( envDirection, "env", skyDirection, "skybox", args.Argv( 1 ) );
}

/*
==================
R_TransformSkyboxToEnv_f

R_TransformSkyboxToEnv_f <basename>

transforms skybox textures ( forward, back, left, right, up, down)
to env textures (of the type px, py, pz, nx, ny, nz)
==================
*/

void R_TransformSkyboxToEnv_f( const idCmdArgs& args )
{

	if( args.Argc() != 2 )
	{
		common->Printf( "USAGE: skyToEnv <basename>\n" );
		return;
	}

	R_TransformCubemap( skyDirection, "skybox", envDirection, "env", args.Argv( 1 ) );
}

//============================================================================


/*
===============
R_SetColorMappings
===============
*/
void R_SetColorMappings()
{
	float b = r_brightness.GetFloat();
	float invg = 1.0f / r_gamma.GetFloat();

	float j = 0.0f;
	for( int i = 0; i < 256; i++, j += b )
	{
		int inf = idMath::Ftoi( 0xffff * pow( j / 255.0f, invg ) + 0.5f );
		tr.gammaTable[i] = idMath::ClampInt( 0, 0xFFFF, inf );
	}
// SRS - Generalized Vulkan SDL platform
#if defined( VULKAN_USE_PLATFORM_SDL )
	VKimp_SetGamma( tr.gammaTable, tr.gammaTable, tr.gammaTable );
#else
	GLimp_SetGamma( tr.gammaTable, tr.gammaTable, tr.gammaTable );
#endif
}

/*
================
GfxInfo_f
================
*/
void GfxInfo_f( const idCmdArgs& args )
{
	common->Printf( "CPU: %s\n", Sys_GetProcessorString() );

	const char* fsstrings[] =
	{
		"windowed",
		"fullscreen"
	};

	common->Printf( "Graphics API: %s\n", deviceManager->GetDevice()->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12 ? "DirectX 12 " : "Vulkan" );
	common->Printf( "Render Device: %s\n", deviceManager->GetRendererString() );

	// print all the display adapters, monitors, and video modes
	//void DumpAllDisplayDevices();
	//DumpAllDisplayDevices();

	//common->Printf( "\nPIXELFORMAT: color(%d-bits) Z(%d-bit) stencil(%d-bits)\n", glConfig.colorBits, glConfig.depthBits, glConfig.stencilBits );
	common->Printf( "MODE: %d, %d x %d %s hz:", r_vidMode.GetInteger(), renderSystem->GetWidth(), renderSystem->GetHeight(), fsstrings[r_fullscreen.GetBool()] );
	if( glConfig.displayFrequency )
	{
		common->Printf( "%d\n", glConfig.displayFrequency );
	}
	else
	{
		common->Printf( "N/A\n" );
	}

	common->Printf( "-------\n" );

	if( r_swapInterval.GetInteger() )
	{
		common->Printf( "Forcing swapInterval %i\n", r_swapInterval.GetInteger() );
	}
	else
	{
		common->Printf( "swapInterval not forced\n" );
	}

	//idLib::Printf( "%i multisamples\n", glConfig.multisamples );

	common->Printf( "%5.1f cm screen width (%4.1f\" diagonal)\n",
					glConfig.physicalScreenWidthInCentimeters, glConfig.physicalScreenWidthInCentimeters / 2.54f
					* sqrt( ( float )( 16 * 16 + 9 * 9 ) ) / 16.0f );
	extern idCVar r_forceScreenWidthCentimeters;
	if( r_forceScreenWidthCentimeters.GetFloat() )
	{
		common->Printf( "screen size manually forced to %5.1f cm width (%4.1f\" diagonal)\n",
						renderSystem->GetPhysicalScreenWidthInCentimeters(), renderSystem->GetPhysicalScreenWidthInCentimeters() / 2.54f
						* sqrt( ( float )( 16 * 16 + 9 * 9 ) ) / 16.0f );
	}
}

/*
=================
R_VidRestart_f
=================
*/
void R_VidRestart_f( const idCmdArgs& args )
{
	// if OpenGL isn't started, do nothing
	if( !tr.IsInitialized() )
	{
		return;
	}

	// set the mode without re-initializing the context
	R_SetNewMode( false );
}

/*
=================
R_InitMaterials
=================
*/
void R_InitMaterials()
{
	tr.defaultMaterial = declManager->FindMaterial( "_default", false );
	if( !tr.defaultMaterial )
	{
		common->FatalError( "_default material not found" );
	}
	tr.defaultPointLight = declManager->FindMaterial( "lights/defaultPointLight" );
	tr.defaultProjectedLight = declManager->FindMaterial( "lights/defaultProjectedLight" );
	tr.whiteMaterial = declManager->FindMaterial( "_white", false );
	tr.charSetMaterial = declManager->FindMaterial( "textures/bigchars" );

	// RB: create implicit material
	tr.imgGuiMaterial = declManager->FindMaterial( "_imguiFont", true );

	ImGuiIO& io = ImGui::GetIO();
	io.Fonts->TexID = ( void* )( intptr_t )tr.imgGuiMaterial;
}


/*
=================
R_SizeUp_f

Keybinding command
=================
*/
static void R_SizeUp_f( const idCmdArgs& args )
{
	if( r_screenFraction.GetInteger() + 10 > 100 )
	{
		r_screenFraction.SetInteger( 100 );
	}
	else
	{
		r_screenFraction.SetInteger( r_screenFraction.GetInteger() + 10 );
	}
}


/*
=================
R_SizeDown_f

Keybinding command
=================
*/
static void R_SizeDown_f( const idCmdArgs& args )
{
	if( r_screenFraction.GetInteger() - 10 < 10 )
	{
		r_screenFraction.SetInteger( 10 );
	}
	else
	{
		r_screenFraction.SetInteger( r_screenFraction.GetInteger() - 10 );
	}
}


/*
===============
TouchGui_f

  this is called from the main thread
===============
*/
void R_TouchGui_f( const idCmdArgs& args )
{
	const char*	gui = args.Argv( 1 );

	if( !gui[0] )
	{
		common->Printf( "USAGE: touchGui <guiName>\n" );
		return;
	}

	common->Printf( "touchGui %s\n", gui );
	const bool captureToImage = false;
	common->UpdateScreen( captureToImage );
	uiManager->Touch( gui );
}



/*
=================
R_TestVRS_f

Report hardware variable-rate-shading availability + the rate-image tile size. VRS is the vehicle
for the width-driven analytic soft-shadow sub-sampling (shade the inline wedge at a coarse rate in the
smooth wide penumbra, full rate at contact/kinks). The rate-image is sized ceil(screen / tileSize), so
the tile size printed here is what the width pre-pass and the rate image must use.
=================
*/
void R_TestVRS_f( const idCmdArgs& args )
{
	nvrhi::IDevice* dev = deviceManager != NULL ? deviceManager->GetDevice() : NULL;
	if( dev == NULL )
	{
		common->Printf( "VRS: no device.\n" );
		return;
	}
	if( !dev->queryFeatureSupport( nvrhi::Feature::VariableRateShading ) )
	{
		common->Printf( "VRS: image-based fragment shading rate NOT supported on this device.\n" );
		return;
	}
	nvrhi::VariableRateShadingFeatureInfo info = {};
	dev->queryFeatureSupport( nvrhi::Feature::VariableRateShading, &info, sizeof( info ) );
	common->Printf( "VRS: image-based fragment shading rate AVAILABLE; rate-image tile size = %u px.\n", info.shadingRateImageTileSize );
}

/*
=================
R_DumpHDR_f

Dump the current final (LDR) frame to <name>.png in fs_savepath, respecting whatever config was applied
at launch. captureShadowRefs cycles cvars mid-process and re-renders with R_RenderOneFrame, which does NOT
re-run the frontend (soft-edge collection / RT TLAS), so every column came out identical and could not show
the RT-soft vs analytic-hard difference. The reliable A/B is a SEPARATE launch per config (config applied at
startup, frontend builds correctly over the wait frames) then this one dump. Renders one backend frame from
the current command buffer, like R_ScreenShot_f.
=================
*/
void R_DumpHDR_f( const idCmdArgs& args )
{
	idStr fn = ( args.Argc() > 1 ) ? args.Argv( 1 ) : "dumphdr";
	fn += ".png";
	const emptyCommand_t* cmd = tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );
	tr.RenderCommandBuffers( cmd );
	tr.SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );
	R_ReadPixelsRGB8( deviceManager->GetDevice(), &backEnd.GetCommonPasses(), globalImages->ldrImage->GetTextureHandle(), nvrhi::ResourceStates::RenderTarget, fn.c_str() );
	common->Printf( "dumpHDR: wrote %s\n", fn.c_str() );
}

/*
=================
R_InitCommands
=================
*/
void R_InitCommands()
{
	cmdSystem->AddCommand( "sizeUp", R_SizeUp_f, CMD_FL_RENDERER, "makes the rendered view larger" );
	cmdSystem->AddCommand( "sizeDown", R_SizeDown_f, CMD_FL_RENDERER, "makes the rendered view smaller" );
	cmdSystem->AddCommand( "reloadGuis", R_ReloadGuis_f, CMD_FL_RENDERER, "reloads guis" );
	cmdSystem->AddCommand( "listGuis", R_ListGuis_f, CMD_FL_RENDERER, "lists guis" );
	cmdSystem->AddCommand( "touchGui", R_TouchGui_f, CMD_FL_RENDERER, "touches a gui" );
	cmdSystem->AddCommand( "screenshot", R_ScreenShot_f, CMD_FL_RENDERER, "takes a screenshot" );
	cmdSystem->AddCommand( "testVRS", R_TestVRS_f, CMD_FL_RENDERER, "report hardware variable-rate-shading availability + rate-image tile size" );
	cmdSystem->AddCommand( "dumpHDR", R_DumpHDR_f, CMD_FL_RENDERER, "dump the current final frame to <name>.png (config-respecting; for headless A/B via separate launches)" );
	cmdSystem->AddCommand( "capture", R_CaptureSoftShadow_f, CMD_FL_RENDERER, "arms a one-shot scene capture (.cap: full savegame + camera + soft-shadow effect blocks) + a .png preview" );
	cmdSystem->AddCommand( "captureShadowRefs", R_CaptureShadowRefs_f, CMD_FL_RENDERER, "self-contained: freezes time, cycles RT-ref/analytic-bandoff/analytic-bandon, dumps frame+term PNG columns, restores cvars" );
	cmdSystem->AddCommand( "testSoftShadowLocator", R_TestSoftShadowLocator_f, CMD_FL_RENDERER, "automated self-check: RT oracle vs soft+PCSS-locator hybrid from one frozen view; prints PASS/FAIL false-shadow rate" );
	cmdSystem->AddCommand( "softShadowGoto", R_SoftShadowGoto_f, CMD_FL_RENDERER, "skip the intro cinematic and pin the view at a .cap camera over the next frames (run before testSoftShadowLocator)" );
	cmdSystem->AddCommand( "softShadowShots", R_SoftShadowShots_f, CMD_FL_RENDERER, "headless: render each given .cap to shot_<name>.png then quit (no waits/bash; drives the frame loop internally)" );
	cmdSystem->AddCommand( "checkShadowConfig", R_CheckShadowConflicts_f, CMD_FL_RENDERER, "warn on contradictory shadow-technique cvar combos (e.g. scanline silently no-op'd by the surf cache, RT vs soft)" );
	cmdSystem->AddCommand( "softShadowRepro", R_SoftShadowRepro_f, CMD_FL_RENDERER, "headless: restore each .cap's embedded save (exact game state), freeze, A/B scanline vs sampled in-engine, then quit" );
	cmdSystem->AddCommand( "softShadowRecapture", R_SoftShadowRecapture_f, CMD_FL_RENDERER, "headless: restore each .cap's embedded save + camera, overwrite the .cap in place with the current writer (v7 live-light tail), then quit" );
	cmdSystem->AddCommand( "softShadowReproShot", R_SoftShadowReproShot_f, CMD_FL_RENDERER, "internal: the synchronous freeze+A/B+diff shot for one already-positioned capture (used by softShadowRepro)" );
	cmdSystem->AddCommand( "softShadowSpawnCasters", R_SoftShadowSpawnCasters_f, CMD_FL_RENDERER, "reproduce a capture's dynamic casters (the scripted rock/crate) that loadGame does not spawn" );
	cmdSystem->AddCommand( "envshot", R_EnvShot_f, CMD_FL_RENDERER, "takes an environment shot" );
	cmdSystem->AddCommand( "envToSky", R_TransformEnvToSkybox_f, CMD_FL_RENDERER | CMD_FL_CHEAT, "transforms environment textures to sky box textures" );
	cmdSystem->AddCommand( "skyToEnv", R_TransformSkyboxToEnv_f, CMD_FL_RENDERER | CMD_FL_CHEAT, "transforms sky box textures to environment textures" );
	cmdSystem->AddCommand( "gfxInfo", GfxInfo_f, CMD_FL_RENDERER, "show graphics info" );
	cmdSystem->AddCommand( "modulateLights", R_ModulateLights_f, CMD_FL_RENDERER | CMD_FL_CHEAT, "modifies shader parms on all lights" );
	cmdSystem->AddCommand( "testImage", R_TestImage_f, CMD_FL_RENDERER | CMD_FL_CHEAT, "displays the given image centered on screen", idCmdSystem::ArgCompletion_ImageName );
	cmdSystem->AddCommand( "testVideo", R_TestVideo_f, CMD_FL_RENDERER | CMD_FL_CHEAT, "displays the given cinematic", idCmdSystem::ArgCompletion_VideoName );
	cmdSystem->AddCommand( "reportSurfaceAreas", R_ReportSurfaceAreas_f, CMD_FL_RENDERER, "lists all used materials sorted by surface area" );
	cmdSystem->AddCommand( "showInteractionMemory", R_ShowInteractionMemory_f, CMD_FL_RENDERER, "shows memory used by interactions" );
	cmdSystem->AddCommand( "vid_restart", R_VidRestart_f, CMD_FL_RENDERER, "restarts renderSystem" );
	cmdSystem->AddCommand( "listRenderEntityDefs", R_ListRenderEntityDefs_f, CMD_FL_RENDERER, "lists the entity defs" );
	cmdSystem->AddCommand( "listRenderLightDefs", R_ListRenderLightDefs_f, CMD_FL_RENDERER, "lists the light defs" );
	cmdSystem->AddCommand( "listModes", R_ListModes_f, CMD_FL_RENDERER, "lists all video modes" );
	cmdSystem->AddCommand( "reloadSurface", R_ReloadSurface_f, CMD_FL_RENDERER, "reloads the decl and images for selected surface" );
}

/*
===============
idRenderSystemLocal::Clear
===============
*/
void idRenderSystemLocal::Clear()
{
	registered = false;
	frameCount = 0;
	viewCount = 0;
	frameShaderTime = 0.0f;
	ambientLightVector.Zero();
	worlds.Clear();
	primaryWorld = NULL;
	memset( &primaryRenderView, 0, sizeof( primaryRenderView ) );
	primaryView = NULL;
	defaultMaterial = NULL;
	testImage = NULL;
	ambientCubeImage = NULL;
	viewDef = NULL;
	memset( &pc, 0, sizeof( pc ) );
	memset( &identitySpace, 0, sizeof( identitySpace ) );
	memset( renderCrops, 0, sizeof( renderCrops ) );
	currentRenderCrop = 0;
	currentColorNativeBytesOrder = 0xFFFFFFFF;
	currentGLState = 0;
	guiRecursionLevel = 0;
	guiModel = NULL;
	memset( gammaTable, 0, sizeof( gammaTable ) );
	memset( &cubeAxis, 0, sizeof( cubeAxis ) ); // RB
	takingScreenshot = false;
	takingEnvprobe = false;

	if( unitSquareTriangles != NULL )
	{
		Mem_Free( unitSquareTriangles->verts );
		Mem_Free( unitSquareTriangles->indexes );
		Mem_Free( unitSquareTriangles );
		unitSquareTriangles = NULL;
	}

	if( zeroOneCubeTriangles != NULL )
	{
		Mem_Free( zeroOneCubeTriangles->verts );
		Mem_Free( zeroOneCubeTriangles->indexes );
		Mem_Free( zeroOneCubeTriangles );
		zeroOneCubeTriangles = NULL;
	}

	if( zeroOneSphereTriangles != NULL )
	{
		Mem_Free( zeroOneSphereTriangles->verts );
		Mem_Free( zeroOneSphereTriangles->indexes );
		Mem_Free( zeroOneSphereTriangles );
		zeroOneSphereTriangles = NULL;
	}

	if( testImageTriangles != NULL )
	{
		Mem_Free( testImageTriangles->verts );
		Mem_Free( testImageTriangles->indexes );
		Mem_Free( testImageTriangles );
		testImageTriangles = NULL;
	}

	frontEndJobList = NULL;

	// RB
	envprobeJobList = NULL;
	envprobeJobs.Clear();
	lightGridJobs.Clear();

#if defined(USE_INTRINSICS_SSE)
	// destroy occlusion culling object and free hierarchical z-buffer
	if( maskedOcclusionCulling != NULL )
	{
#if MOC_MULTITHREADED
		delete maskedOcclusionThreaded;
		maskedOcclusionThreaded = NULL;
#endif
		MaskedOcclusionCulling::Destroy( maskedOcclusionCulling );

		maskedOcclusionCulling = NULL;
	}
#endif
}

/*
=============
R_MakeFullScreenTris
=============
*/
static srfTriangles_t* R_MakeFullScreenTris()
{
	// copy verts and indexes
	srfTriangles_t* tri = ( srfTriangles_t* )Mem_ClearedAlloc( sizeof( *tri ), TAG_RENDER_TOOLS );

	tri->numIndexes = 6;
	tri->numVerts = 4;

	int indexSize = tri->numIndexes * sizeof( tri->indexes[0] );
	int allocatedIndexBytes = ALIGN( indexSize, 16 );
	tri->indexes = ( triIndex_t* )Mem_Alloc( allocatedIndexBytes, TAG_RENDER_TOOLS );

	int vertexSize = tri->numVerts * sizeof( tri->verts[0] );
	int allocatedVertexBytes =  ALIGN( vertexSize, 16 );
	tri->verts = ( idDrawVert* )Mem_ClearedAlloc( allocatedVertexBytes, TAG_RENDER_TOOLS );

	idDrawVert* verts = tri->verts;

	triIndex_t tempIndexes[6] = { 3, 0, 2, 2, 0, 1 };
	memcpy( tri->indexes, tempIndexes, indexSize );

	verts[0].xyz[0] = -1.0f;
	verts[0].xyz[1] = 1.0f;
	verts[0].SetTexCoord( 0.0f, 1.0f );

	verts[1].xyz[0] = 1.0f;
	verts[1].xyz[1] = 1.0f;
	verts[1].SetTexCoord( 1.0f, 1.0f );

	verts[2].xyz[0] = 1.0f;
	verts[2].xyz[1] = -1.0f;
	verts[2].SetTexCoord( 1.0f, 0.0f );

	verts[3].xyz[0] = -1.0f;
	verts[3].xyz[1] = -1.0f;
	verts[3].SetTexCoord( 0.0f, 0.0f );

	for( int i = 0 ; i < 4 ; i++ )
	{
		verts[i].SetColor( 0xffffffff );
	}


	return tri;
}

/*
=============
R_MakeZeroOneCubeTris
=============
*/
static srfTriangles_t* R_MakeZeroOneCubeTris()
{
	srfTriangles_t* tri = ( srfTriangles_t* )Mem_ClearedAlloc( sizeof( *tri ), TAG_RENDER_TOOLS );

	tri->numVerts = 8;
	tri->numIndexes = 36;

	const int indexSize = tri->numIndexes * sizeof( tri->indexes[0] );
	const int allocatedIndexBytes = ALIGN( indexSize, 16 );
	tri->indexes = ( triIndex_t* )Mem_Alloc( allocatedIndexBytes, TAG_RENDER_TOOLS );

	const int vertexSize = tri->numVerts * sizeof( tri->verts[0] );
	const int allocatedVertexBytes =  ALIGN( vertexSize, 16 );
	tri->verts = ( idDrawVert* )Mem_ClearedAlloc( allocatedVertexBytes, TAG_RENDER_TOOLS );

	idDrawVert* verts = tri->verts;

	const float low = 0.0f;
	const float high = 1.0f;

	idVec3 center( 0.0f );
	idVec3 mx( low, 0.0f, 0.0f );
	idVec3 px( high, 0.0f, 0.0f );
	idVec3 my( 0.0f,  low, 0.0f );
	idVec3 py( 0.0f, high, 0.0f );
	idVec3 mz( 0.0f, 0.0f,  low );
	idVec3 pz( 0.0f, 0.0f, high );

	verts[0].xyz = center + mx + my + mz;
	verts[1].xyz = center + px + my + mz;
	verts[2].xyz = center + px + py + mz;
	verts[3].xyz = center + mx + py + mz;
	verts[4].xyz = center + mx + my + pz;
	verts[5].xyz = center + px + my + pz;
	verts[6].xyz = center + px + py + pz;
	verts[7].xyz = center + mx + py + pz;

	// bottom
	tri->indexes[ 0 * 3 + 0] = 2;
	tri->indexes[ 0 * 3 + 1] = 3;
	tri->indexes[ 0 * 3 + 2] = 0;
	tri->indexes[ 1 * 3 + 0] = 1;
	tri->indexes[ 1 * 3 + 1] = 2;
	tri->indexes[ 1 * 3 + 2] = 0;
	// back
	tri->indexes[ 2 * 3 + 0] = 5;
	tri->indexes[ 2 * 3 + 1] = 1;
	tri->indexes[ 2 * 3 + 2] = 0;
	tri->indexes[ 3 * 3 + 0] = 4;
	tri->indexes[ 3 * 3 + 1] = 5;
	tri->indexes[ 3 * 3 + 2] = 0;
	// left
	tri->indexes[ 4 * 3 + 0] = 7;
	tri->indexes[ 4 * 3 + 1] = 4;
	tri->indexes[ 4 * 3 + 2] = 0;
	tri->indexes[ 5 * 3 + 0] = 3;
	tri->indexes[ 5 * 3 + 1] = 7;
	tri->indexes[ 5 * 3 + 2] = 0;
	// right
	tri->indexes[ 6 * 3 + 0] = 1;
	tri->indexes[ 6 * 3 + 1] = 5;
	tri->indexes[ 6 * 3 + 2] = 6;
	tri->indexes[ 7 * 3 + 0] = 2;
	tri->indexes[ 7 * 3 + 1] = 1;
	tri->indexes[ 7 * 3 + 2] = 6;
	// front
	tri->indexes[ 8 * 3 + 0] = 3;
	tri->indexes[ 8 * 3 + 1] = 2;
	tri->indexes[ 8 * 3 + 2] = 6;
	tri->indexes[ 9 * 3 + 0] = 7;
	tri->indexes[ 9 * 3 + 1] = 3;
	tri->indexes[ 9 * 3 + 2] = 6;
	// top
	tri->indexes[10 * 3 + 0] = 4;
	tri->indexes[10 * 3 + 1] = 7;
	tri->indexes[10 * 3 + 2] = 6;
	tri->indexes[11 * 3 + 0] = 5;
	tri->indexes[11 * 3 + 1] = 4;
	tri->indexes[11 * 3 + 2] = 6;

	for( int i = 0 ; i < 4 ; i++ )
	{
		verts[i].SetColor( 0xffffffff );
	}

	return tri;
}

// RB begin
#if defined(USE_INTRINSICS_SSE)
static void R_MakeZeroOneCubeTrisForMaskedOcclusionCulling()
{
	const float low = 0.0f;
	const float high = 1.0f;

	idVec3 center( 0.0f );
	idVec3 mx( low, 0.0f, 0.0f );
	idVec3 px( high, 0.0f, 0.0f );
	idVec3 my( 0.0f,  low, 0.0f );
	idVec3 py( 0.0f, high, 0.0f );
	idVec3 mz( 0.0f, 0.0f,  low );
	idVec3 pz( 0.0f, 0.0f, high );

	idVec4* verts = tr.maskedZeroOneCubeVerts;

	verts[0].ToVec3() = center + mx + my + mz;
	verts[1].ToVec3() = center + px + my + mz;
	verts[2].ToVec3() = center + px + py + mz;
	verts[3].ToVec3() = center + mx + py + mz;
	verts[4].ToVec3() = center + mx + my + pz;
	verts[5].ToVec3() = center + px + my + pz;
	verts[6].ToVec3() = center + px + py + pz;
	verts[7].ToVec3() = center + mx + py + pz;

	verts[0].w = 1;
	verts[1].w = 1;
	verts[2].w = 1;
	verts[3].w = 1;
	verts[4].w = 1;
	verts[5].w = 1;
	verts[6].w = 1;
	verts[7].w = 1;

	unsigned int* indexes = tr.maskedZeroOneCubeIndexes;

	// bottom
	indexes[ 0 * 3 + 0] = 2;
	indexes[ 0 * 3 + 1] = 3;
	indexes[ 0 * 3 + 2] = 0;
	indexes[ 1 * 3 + 0] = 1;
	indexes[ 1 * 3 + 1] = 2;
	indexes[ 1 * 3 + 2] = 0;
	// back
	indexes[ 2 * 3 + 0] = 5;
	indexes[ 2 * 3 + 1] = 1;
	indexes[ 2 * 3 + 2] = 0;
	indexes[ 3 * 3 + 0] = 4;
	indexes[ 3 * 3 + 1] = 5;
	indexes[ 3 * 3 + 2] = 0;
	// left
	indexes[ 4 * 3 + 0] = 7;
	indexes[ 4 * 3 + 1] = 4;
	indexes[ 4 * 3 + 2] = 0;
	indexes[ 5 * 3 + 0] = 3;
	indexes[ 5 * 3 + 1] = 7;
	indexes[ 5 * 3 + 2] = 0;
	// right
	indexes[ 6 * 3 + 0] = 1;
	indexes[ 6 * 3 + 1] = 5;
	indexes[ 6 * 3 + 2] = 6;
	indexes[ 7 * 3 + 0] = 2;
	indexes[ 7 * 3 + 1] = 1;
	indexes[ 7 * 3 + 2] = 6;
	// front
	indexes[ 8 * 3 + 0] = 3;
	indexes[ 8 * 3 + 1] = 2;
	indexes[ 8 * 3 + 2] = 6;
	indexes[ 9 * 3 + 0] = 7;
	indexes[ 9 * 3 + 1] = 3;
	indexes[ 9 * 3 + 2] = 6;
	// top
	indexes[10 * 3 + 0] = 4;
	indexes[10 * 3 + 1] = 7;
	indexes[10 * 3 + 2] = 6;
	indexes[11 * 3 + 0] = 5;
	indexes[11 * 3 + 1] = 4;
	indexes[11 * 3 + 2] = 6;
}

static void R_MakeUnitCubeTrisForMaskedOcclusionCulling()
{
	const float low = -1.0f;
	const float high = 1.0f;

	idVec3 center( 0.0f );
	idVec3 mx( low, 0.0f, 0.0f );
	idVec3 px( high, 0.0f, 0.0f );
	idVec3 my( 0.0f,  low, 0.0f );
	idVec3 py( 0.0f, high, 0.0f );
	idVec3 mz( 0.0f, 0.0f,  low );
	idVec3 pz( 0.0f, 0.0f, high );

	idVec4* verts = tr.maskedUnitCubeVerts;

	verts[0].ToVec3() = center + mx + my + mz;
	verts[1].ToVec3() = center + px + my + mz;
	verts[2].ToVec3() = center + px + py + mz;
	verts[3].ToVec3() = center + mx + py + mz;
	verts[4].ToVec3() = center + mx + my + pz;
	verts[5].ToVec3() = center + px + my + pz;
	verts[6].ToVec3() = center + px + py + pz;
	verts[7].ToVec3() = center + mx + py + pz;

	verts[0].w = 1;
	verts[1].w = 1;
	verts[2].w = 1;
	verts[3].w = 1;
	verts[4].w = 1;
	verts[5].w = 1;
	verts[6].w = 1;
	verts[7].w = 1;
}
#endif

static srfTriangles_t* R_MakeZeroOneSphereTris()
{
	srfTriangles_t* tri = ( srfTriangles_t* )Mem_ClearedAlloc( sizeof( *tri ), TAG_RENDER_TOOLS );

	const float radius = 1.0f;
	const int rings = 20.0f;
	const int sectors = 20.0f;

	tri->numVerts = ( rings * sectors );
	tri->numIndexes = ( ( rings - 1 ) * sectors ) * 6;

	const int indexSize = tri->numIndexes * sizeof( tri->indexes[0] );
	const int allocatedIndexBytes = ALIGN( indexSize, 16 );
	tri->indexes = ( triIndex_t* )Mem_Alloc( allocatedIndexBytes, TAG_RENDER_TOOLS );

	const int vertexSize = tri->numVerts * sizeof( tri->verts[0] );
	const int allocatedVertexBytes =  ALIGN( vertexSize, 16 );
	tri->verts = ( idDrawVert* )Mem_ClearedAlloc( allocatedVertexBytes, TAG_RENDER_TOOLS );

	idDrawVert* verts = tri->verts;

	float const R = 1.0f / ( float )( rings - 1 );
	float const S = 1.0f / ( float )( sectors - 1 );

	int numTris = 0;
	int numVerts = 0;
	for( int r = 0; r < rings; ++r )
	{
		for( int s = 0; s < sectors; ++s )
		{
			const float y = sin( -idMath::HALF_PI +  idMath::PI * r * R );
			const float x = cos( 2 * idMath::PI * s * S ) * sin( idMath::PI * r * R );
			const float z = sin( 2 * idMath::PI * s * S ) * sin( idMath::PI * r * R );

			verts[ numVerts ].SetTexCoord( s * S, r * R );
			verts[ numVerts ].xyz = idVec3( x, y, z ) * radius;
			verts[ numVerts ].SetNormal( x, y, z );
			verts[ numVerts ].SetColor( 0xffffffff );
			numVerts++;

			if( r < ( rings - 1 ) )
			{
				int curRow = r * sectors;
				int nextRow = ( r + 1 ) * sectors;
				int nextS = ( s + 1 ) % sectors;

				tri->indexes[( numTris * 3 ) + 2] = ( curRow + s );
				tri->indexes[( numTris * 3 ) + 1] = ( nextRow + s );
				tri->indexes[( numTris * 3 ) + 0] = ( nextRow + nextS );

				numTris += 1;

				tri->indexes[( numTris * 3 ) + 2] = ( curRow + s );
				tri->indexes[( numTris * 3 ) + 1] = ( nextRow + nextS );
				tri->indexes[( numTris * 3 ) + 0] = ( curRow + nextS );

				numTris += 1;
			}
		}
	}

	return tri;
}
// RB end

/*
================
R_MakeTestImageTriangles

Initializes the Test Image Triangles
================
*/
srfTriangles_t* R_MakeTestImageTriangles()
{
	srfTriangles_t* tri = ( srfTriangles_t* )Mem_ClearedAlloc( sizeof( *tri ), TAG_RENDER_TOOLS );

	tri->numIndexes = 6;
	tri->numVerts = 4;

	int indexSize = tri->numIndexes * sizeof( tri->indexes[0] );
	int allocatedIndexBytes = ALIGN( indexSize, 16 );
	tri->indexes = ( triIndex_t* )Mem_Alloc( allocatedIndexBytes, TAG_RENDER_TOOLS );

	int vertexSize = tri->numVerts * sizeof( tri->verts[0] );
	int allocatedVertexBytes =  ALIGN( vertexSize, 16 );
	tri->verts = ( idDrawVert* )Mem_ClearedAlloc( allocatedVertexBytes, TAG_RENDER_TOOLS );

	ALIGNTYPE16 triIndex_t tempIndexes[6] = { 3, 0, 2, 2, 0, 1 };
	memcpy( tri->indexes, tempIndexes, indexSize );

	idDrawVert* tempVerts = tri->verts;
	tempVerts[0].xyz[0] = 0.0f;
	tempVerts[0].xyz[1] = 0.0f;
	tempVerts[0].xyz[2] = 0;
	tempVerts[0].SetTexCoord( 0.0, 0.0f );

	tempVerts[1].xyz[0] = 1.0f;
	tempVerts[1].xyz[1] = 0.0f;
	tempVerts[1].xyz[2] = 0;
	tempVerts[1].SetTexCoord( 1.0f, 0.0f );

	tempVerts[2].xyz[0] = 1.0f;
	tempVerts[2].xyz[1] = 1.0f;
	tempVerts[2].xyz[2] = 0;
	tempVerts[2].SetTexCoord( 1.0f, 1.0f );

	tempVerts[3].xyz[0] = 0.0f;
	tempVerts[3].xyz[1] = 1.0f;
	tempVerts[3].xyz[2] = 0;
	tempVerts[3].SetTexCoord( 0.0f, 1.0f );

	for( int i = 0; i < 4; i++ )
	{
		tempVerts[i].SetColor( 0xFFFFFFFF );
	}
	return tri;
}

/*
===============
idRenderSystemLocal::Init
===============
*/
void idRenderSystemLocal::Init()
{
	common->Printf( "------- Initializing renderSystem --------\n" );

	// clear all our internal state
	viewCount = 1;		// so cleared structures never match viewCount
	// we used to memset tr, but now that it is a class, we can't, so
	// there may be other state we need to reset

	ambientLightVector[0] = 0.5f;
	ambientLightVector[1] = 0.5f - 0.385f;
	ambientLightVector[2] = 0.8925f;
	ambientLightVector[3] = 1.0f;

	R_InitCommands();

	// allocate the frame data, which may be more if smp is enabled
	R_InitFrameData();

	guiModel = new( TAG_RENDER ) idGuiModel;
	guiModel->Clear();
	tr_guiModel = guiModel;	// for DeviceContext fast path

	globalImages->Init();

	// RB begin
	Framebuffer::Init();
	// RB end

	idCinematic::InitCinematic();

	// build brightness translation tables
	R_SetColorMappings();

	R_InitMaterials();

	renderModelManager->Init();

	// set the identity space
	identitySpace.modelMatrix[0 * 4 + 0] = 1.0f;
	identitySpace.modelMatrix[1 * 4 + 1] = 1.0f;
	identitySpace.modelMatrix[2 * 4 + 2] = 1.0f;

	// set cubemap axis for cubemap sampling tools

	// +X
	cubeAxis[0][0][0] = 1;
	cubeAxis[0][1][2] = 1;
	cubeAxis[0][2][1] = 1;

	// -X
	cubeAxis[1][0][0] = -1;
	cubeAxis[1][1][2] = -1;
	cubeAxis[1][2][1] = 1;

	// +Y
	cubeAxis[2][0][1] = 1;
	cubeAxis[2][1][0] = -1;
	cubeAxis[2][2][2] = -1;

	// -Y
	cubeAxis[3][0][1] = -1;
	cubeAxis[3][1][0] = -1;
	cubeAxis[3][2][2] = 1;

	// +Z
	cubeAxis[4][0][2] = 1;
	cubeAxis[4][1][0] = -1;
	cubeAxis[4][2][1] = 1;

	// -Z
	cubeAxis[5][0][2] = -1;
	cubeAxis[5][1][0] = 1;
	cubeAxis[5][2][1] = 1;

	// make sure the tr.unitSquareTriangles data is current in the vertex / index cache
	if( unitSquareTriangles == NULL )
	{
		unitSquareTriangles = R_MakeFullScreenTris();
	}

	// make sure the tr.zeroOneCubeTriangles data is current in the vertex / index cache
	if( zeroOneCubeTriangles == NULL )
	{
		zeroOneCubeTriangles = R_MakeZeroOneCubeTris();
		R_DeriveTangents( zeroOneCubeTriangles ); // RB: we need normals for debugging reflections
	}

	// RB make sure the tr.zeroOneSphereTriangles data is current in the vertex / index cache
	if( zeroOneSphereTriangles == NULL )
	{
		zeroOneSphereTriangles = R_MakeZeroOneSphereTris();
		//R_DeriveTangents( zeroOneSphereTriangles );
	}

	// make sure the tr.testImageTriangles data is current in the vertex / index cache
	if( testImageTriangles == NULL )
	{
		testImageTriangles = R_MakeTestImageTriangles();
	}

	frontEndJobList = parallelJobManager->AllocJobList( JOBLIST_RENDERER_FRONTEND, JOBLIST_PRIORITY_MEDIUM, 2048, 0, NULL );
	envprobeJobList = parallelJobManager->AllocJobList( JOBLIST_UTILITY, JOBLIST_PRIORITY_MEDIUM, 2048, 0, NULL ); // RB

	if( deviceManager->GetGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN )
	{
		// avoid GL_BlockingSwapBuffers
		omitSwapBuffers = true;
	}

#if defined(USE_INTRINSICS_SSE)
	// Flush denorms to zero to avoid performance issues with small values
	_mm_setcsr( _mm_getcsr() | 0x8040 );

	maskedOcclusionCulling = MaskedOcclusionCulling::Create();

#if MOC_MULTITHREADED
	maskedOcclusionThreaded = new CullingThreadpool( 2, 10, 6, 128 );
	maskedOcclusionThreaded->SetBuffer( maskedOcclusionCulling );
	maskedOcclusionThreaded->WakeThreads();
#endif

	R_MakeZeroOneCubeTrisForMaskedOcclusionCulling();
	R_MakeUnitCubeTrisForMaskedOcclusionCulling();
#endif

	// make sure the command buffers are ready to accept the first screen update
	SwapCommandBuffers( NULL, NULL, NULL, NULL, NULL, NULL );

	common->Printf( "renderSystem initialized.\n" );
	common->Printf( "--------------------------------------\n" );
}

/*
===============
idRenderSystemLocal::Shutdown
===============
*/
void idRenderSystemLocal::Shutdown()
{
	common->Printf( "idRenderSystem::Shutdown()\n" );

	fonts.DeleteContents();

	if( IsInitialized() )
	{
		globalImages->PurgeAllImages();
	}

	renderModelManager->Shutdown();

	// SRS - if testVideo is currently playing, make sure cinematic is deleted before ShutdownCinematic()
	if( tr.testVideo )
	{
		delete tr.testVideo;
		tr.testVideo = NULL;
	}

	idCinematic::ShutdownCinematic();

	globalImages->Shutdown();

	// RB begin
	Framebuffer::Shutdown();
	// RB end

	// free frame memory
	R_ShutdownFrameData();

	UnbindBufferObjects();

	// SRS - wait for device idle before freeing any resources the GPU may be using, otherwise get errors on shutdown
	deviceManager->GetDevice()->waitForIdle();

	// free the vertex cache, which should have nothing allocated now
	vertexCache.Shutdown();

	RB_ShutdownDebugTools();

	delete guiModel;

	parallelJobManager->FreeJobList( envprobeJobList );
	parallelJobManager->FreeJobList( frontEndJobList );

	Clear();

	commandList.Reset();

	ShutdownOpenGL();

	bInitialized = false;
}

/*
========================
idRenderSystemLocal::ResetGuiModels
========================
*/
void idRenderSystemLocal::ResetGuiModels()
{
	delete guiModel;
	guiModel = new( TAG_RENDER ) idGuiModel;
	guiModel->Clear();
	guiModel->BeginFrame();
	tr_guiModel = guiModel;	// for DeviceContext fast path
}

/*
========================
idRenderSystemLocal::BeginLevelLoad
========================
*/
void idRenderSystemLocal::BeginLevelLoad()
{
	// clear binding sets for previous level images and light data #676
	backEnd.ClearCaches();

	globalImages->BeginLevelLoad();
	renderModelManager->BeginLevelLoad();

	// Re-Initialize the Default Materials if needed.
	R_InitMaterials();
}

/*
========================
idRenderSystemLocal::LoadLevelImages
========================
*/
void idRenderSystemLocal::LoadLevelImages()
{
	globalImages->LoadLevelImages( false );

	deviceManager->GetDevice()->waitForIdle();
	deviceManager->GetDevice()->runGarbageCollection();
}

/*
========================
idRenderSystemLocal::Preload
========================
*/
void idRenderSystemLocal::Preload( const idPreloadManifest& manifest, const char* mapName )
{
	globalImages->Preload( manifest, true );
	uiManager->Preload( mapName );
	renderModelManager->Preload( manifest );
}

/*
========================
idRenderSystemLocal::EndLevelLoad
========================
*/
void idRenderSystemLocal::EndLevelLoad()
{
	renderModelManager->EndLevelLoad();
	globalImages->EndLevelLoad();
}

/*
========================
idRenderSystemLocal::BeginAutomaticBackgroundSwaps
========================
*/
void idRenderSystemLocal::BeginAutomaticBackgroundSwaps( autoRenderIconType_t icon )
{
}

/*
========================
idRenderSystemLocal::EndAutomaticBackgroundSwaps
========================
*/
void idRenderSystemLocal::EndAutomaticBackgroundSwaps()
{
}

/*
========================
idRenderSystemLocal::AreAutomaticBackgroundSwapsRunning
========================
*/
bool idRenderSystemLocal::AreAutomaticBackgroundSwapsRunning( autoRenderIconType_t* icon ) const
{
	return false;
}

/*
============
idRenderSystemLocal::RegisterFont
============
*/
idFont* idRenderSystemLocal::RegisterFont( const char* fontName )
{

	idStrStatic< MAX_OSPATH > baseFontName = fontName;
	baseFontName.Replace( "fonts/", "" );
	for( int i = 0; i < fonts.Num(); i++ )
	{
		if( idStr::Icmp( fonts[i]->GetName(), baseFontName ) == 0 )
		{
			fonts[i]->Touch();
			return fonts[i];
		}
	}
	idFont* newFont = new( TAG_FONT ) idFont( baseFontName );
	fonts.Append( newFont );
	return newFont;
}

/*
========================
idRenderSystemLocal::ResetFonts
========================
*/
void idRenderSystemLocal::ResetFonts()
{
	fonts.DeleteContents( true );
}
/*
========================
idRenderSystemLocal::InitOpenGL
========================
*/
void idRenderSystemLocal::InitBackend()
{
	// if OpenGL isn't started, start it now
	if( !IsInitialized() )
	{
		backEnd.Init();

		if( !commandList )
		{
			commandList = deviceManager->GetDevice()->createCommandList();
		}

		commandList->open();

		// Reloading images here causes the rendertargets to get deleted
		globalImages->ReloadImages( true, commandList );

		commandList->close();
		deviceManager->GetDevice()->executeCommandList( commandList );
	}
}

/*
========================
idRenderSystemLocal::ShutdownOpenGL
========================
*/
void idRenderSystemLocal::ShutdownOpenGL()
{
	// free the context and close the window
	R_ShutdownFrameData();

	backEnd.Shutdown();
}

/*
========================
idRenderSystemLocal::IsOpenGLRunning
========================
*/
bool idRenderSystemLocal::IsOpenGLRunning() const
{
	return IsInitialized();
}

/*
========================
idRenderSystemLocal::IsFullScreen
========================
*/
bool idRenderSystemLocal::IsFullScreen() const
{
	return glConfig.isFullscreen != 0;
}

/*
========================
idRenderSystemLocal::GetWidth
========================
*/
int idRenderSystemLocal::GetWidth() const
{
	// do something similar in case the VR API requires it
	/*
	if( glConfig.stereo3Dmode == STEREO3D_SIDE_BY_SIDE || glConfig.stereo3Dmode == STEREO3D_SIDE_BY_SIDE_COMPRESSED )
	{
		return glConfig.nativeScreenWidth >> 1;
	}
	*/

	return glConfig.nativeScreenWidth;
}

/*
========================
idRenderSystemLocal::GetHeight
========================
*/
int idRenderSystemLocal::GetHeight() const
{
	// do something similar in case the VR API requires it
	/*
	if( glConfig.stereo3Dmode == STEREO3D_HDMI_720 )
	{
		return 720;
	}
	extern idCVar stereoRender_warp;
	if( glConfig.stereo3Dmode == STEREO3D_SIDE_BY_SIDE && stereoRender_warp.GetBool() )
	{
		// for the Rift, render a square aspect view that will be symetric for the optics
		return glConfig.nativeScreenWidth >> 1;
	}
	*/

	return glConfig.nativeScreenHeight;
}


// RB: return swap chain width
int idRenderSystemLocal::GetNativeWidth() const
{
	return glConfig.nativeScreenWidth;
}

// RB: return swap chain height
int idRenderSystemLocal::GetNativeHeight() const
{
	return glConfig.nativeScreenHeight;
}
// RB end

// SSAA: the 3D scene render resolution = window resolution * scale. Used only by the scene
// render targets and the scene/post viewports; the UI, present and screenshot stay at
// GetWidth()/GetNativeWidth() so menus, HUD and cursor mapping remain native.
int idRenderSystemLocal::GetRenderWidth() const
{
	return idMath::Ftoi( GetWidth() * R_SSAAScale() );
}

int idRenderSystemLocal::GetRenderHeight() const
{
	return idMath::Ftoi( GetHeight() * R_SSAAScale() );
}

/*
========================
idRenderSystemLocal::GetVirtualWidth
========================
*/
int idRenderSystemLocal::GetVirtualWidth() const
{
// jmarshall - never strech
	//if( r_useVirtualScreenResolution.GetBool() )
	//{
	//	return SCREEN_WIDTH;
	//}
// jmarshall end
	return glConfig.nativeScreenWidth / 2;
}

/*
========================
idRenderSystemLocal::GetVirtualHeight
========================
*/
int idRenderSystemLocal::GetVirtualHeight() const
{
// jmarshall - never strech
	//if( r_useVirtualScreenResolution.GetBool() )
	//{
	//	return SCREEN_HEIGHT;
	//}
// jmarshall end
	return glConfig.nativeScreenHeight / 2;
}

/*
========================
idRenderSystemLocal::GetPixelAspect
========================
*/
float idRenderSystemLocal::GetPixelAspect() const
{
	/*
	switch( glConfig.stereo3Dmode )
	{
		case STEREO3D_SIDE_BY_SIDE_COMPRESSED:
			return glConfig.pixelAspect * 2.0f;
		case STEREO3D_TOP_AND_BOTTOM_COMPRESSED:
		case STEREO3D_INTERLACED:
			return glConfig.pixelAspect * 0.5f;
		default:
			return glConfig.pixelAspect;
	}
	*/

	return glConfig.pixelAspect;
}

/*
========================
idRenderSystemLocal::GetPhysicalScreenWidthInCentimeters

This is used to calculate stereoscopic screen offset for a given interocular distance.
========================
*/
idCVar	r_forceScreenWidthCentimeters( "r_forceScreenWidthCentimeters", "0", CVAR_RENDERER | CVAR_ARCHIVE, "Override screen width returned by hardware" );
float idRenderSystemLocal::GetPhysicalScreenWidthInCentimeters() const
{
	if( r_forceScreenWidthCentimeters.GetFloat() > 0 )
	{
		return r_forceScreenWidthCentimeters.GetFloat();
	}
	return glConfig.physicalScreenWidthInCentimeters;
}
