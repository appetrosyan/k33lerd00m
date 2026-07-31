/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 2026 Robert Beckebans (RBDOOM-3-BFG contributors)

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

// C++ side of the DDGI constant buffer. The matching HLSL cbuffer is declared
// by hand in the ddgi compute shaders (same convention as SsaoConstants /
// ssao_compute.cs.hlsl). Keep fields 16-byte aligned for the cbuffer layout.

struct DdgiConstants
{
	// probe volume placement (world space)
	idVec4		probeGridOrigin;		// xyz = origin
	idVec4		probeGridSpacing;		// xyz = spacing per axis
	idVec2i		probeGridCountsXY;		// probe count on X, Y
	idVec2i		probeGridCountZ_total;	// x = count on Z, y = total probes

	// octahedral atlas layout (mirrors LIGHTGRID_IRRADIANCE_SIZE et al.)
	idVec2i		irradianceProbe;		// x = texels/side incl. border, y = border
	idVec2i		distanceProbe;			// x = texels/side incl. border, y = border

	// update parameters
	int			raysPerProbe;
	float		hysteresis;				// temporal blend weight for history
	float		normalBias;				// self-intersection bias along normal
	float		viewBias;				// self-intersection bias along view

	// per-frame random rotation applied to the ray direction set (quaternion)
	idVec4		rayRotation;

	int			frameIndex;
	int			numLights;				// active projected lights in the light buffer
	float		bounceGain;				// multi-bounce feedback gain (r_ddgiBounceGain)
	int			updateScope;			// 0 = visible only, 1 = + neighbours, 2 = full

	// world -> clip rows for frustum-culling which probes update this frame
	idVec4		worldToClip0;
	idVec4		worldToClip1;
	idVec4		worldToClip2;
	idVec4		worldToClip3;

	idVec4		volumeCenter;			// xyz = camera-anchored volume centre; w = neighbour radius

	// dynamic-light-aware auto-skip: static-only probes trace on a slow stagger
	idVec4		ddgiSkipParams;			// x = autoSkip(0/1), y = staticPeriod, z = dynamicMargin (world units), w = pad
};
