/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 2026 Aleksandr Petrosyan

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG
Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation, either version 3 of the License,
or (at your option) any later version.

Doom 3 BFG Edition Source Code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
Public License for more details.

You should have received a copy of the GNU General Public License along with
Doom 3 BFG Edition Source Code.  If not, see <http://www.gnu.org/licenses/>.

===========================================================================
*/

#pragma once

// C++ side of the RT reflections constant buffer. The matching HLSL cbuffer is
// declared by hand in reflection_trace.cs.hlsl (same convention as DdgiConstants
// / probe_trace.cs.hlsl - the RT shaders are self-contained and do not include
// global_inc.hlsl). Keep fields 16-byte aligned for the cbuffer layout.

struct ReflectionConstants
{
	// clip -> world rows (reconstruct the world position at a screen pixel from
	// its depth). ndc = ( uv.x*2-1, 1-uv.y*2, depth ); world = (row_i . (ndc,1)) / w.
	idVec4		unprojToWorld0;
	idVec4		unprojToWorld1;
	idVec4		unprojToWorld2;
	idVec4		unprojToWorld3;

	// (was world->clip reprojection rows, removed with the world-space re-shade)
	// worldToClip0.x now carries the glossy VNDF max sample count; the rest is unused.
	idVec4		worldToClip0;
	idVec4		worldToClip1;
	idVec4		worldToClip2;
	idVec4		worldToClip3;

	idVec4		eyePos;			// xyz = camera world origin, w unused

	idVec4		params0;		// x = max ray distance, y = normal bias,
	//							// z = reflection intensity, w = disocclusion eps
	idVec4		params1;		// x = roughness gate low (full reflect <=),
	//							// y = roughness gate high (no reflect >=),
	//							// z = env-fallback mip scale, w = shade shadow-ray bias
	idVec2i		screenSize;		// x = width, y = height (pixels)
	idVec2i		debugFlags;		// x = debug mode (0 normal, 1 show reflection,
	//							// 2 visualise trace), y = numLights (re-shade lights)
};
