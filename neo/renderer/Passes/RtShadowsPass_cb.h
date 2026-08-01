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

// C++ side of the ray-traced shadow constant buffer. The matching HLSL cbuffer is
// declared by hand in rt_shadows.cs.hlsl (same convention as ReflectionConstants /
// reflection_trace.cs.hlsl - the RT shaders are self-contained and do not include
// global_inc.hlsl). Keep fields 16-byte aligned for the cbuffer layout.
//
// The pass runs once per shadow-casting light: it reconstructs each screen pixel's
// world position from depth, traces one (or a few) visibility rays toward the light
// against the world TLAS, and writes the visibility fraction into a screen-space
// R8 mask that the forward interaction shader multiplies into the light term.

struct RtShadowConstants
{
	// clip -> world rows (reconstruct the world position at a screen pixel from its
	// depth). ndc = ( uv.x*2-1, 1-uv.y*2, depth ); world = (row_i . (ndc,1)) / w.
	idVec4		unprojToWorld0;
	idVec4		unprojToWorld1;
	idVec4		unprojToWorld2;
	idVec4		unprojToWorld3;

	idVec4		lightOrigin;	// xyz = world-space light origin, w = light radius (soft shadows)
	idVec4		params;			// x = normal bias, y = umbra floor (min shadow term),
	//							// z = ray count, w = frame index (soft-shadow jitter)
	idVec2i		screenSize;		// x = width, y = height (pixels, render resolution)
	idVec2i		pad;			// x = backface cull, y = debug force/hit-dist mode
	idVec2i		scissorMin;		// top-left pixel of this light's dispatch rect (screen space)
	idVec2i		pad2;

	idVec4		cameraOrigin;	// xyz = world-space eye (for the primary-ray TLAS coverage probe, mode 4)
};
