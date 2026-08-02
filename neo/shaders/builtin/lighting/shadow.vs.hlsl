/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.

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

===========================================================================
*/

// Stencil shadow-volume extrusion. The shadow vertex cache stores each model-space
// xyz duplicated: w = 1 for the near-cap vertex (stays on the model) and w = 0 for the
// end-cap vertex (a direction away from the light that the perspective divide sends to
// infinity). rpLocalLightOrigin is the light origin in model space with w = 0.

#include "global_inc.hlsl"
#include "renderParmSet0.inc.hlsl"

// *INDENT-OFF*
struct VS_IN
{
	float4 position	: POSITION;
};

struct VS_OUT
{
	float4 position : SV_Position;
};
// *INDENT-ON*

void main( VS_IN vertex, out VS_OUT result )
{
	// The stencil shadow pass carries the model-space light origin (w = 0) in rpLocalViewOrigin
	// rather than rpLocalLightOrigin: this shader uses renderParmSet0 (constant-buffer-only
	// binding), which does not expose rpLocalLightOrigin, and the shadow pass is the only shader
	// active while it runs, so rpLocalViewOrigin is free to repurpose. Set in StencilShadowPass.
	// vPos.w carries the vertex w (0 = extrude to infinity, 1 = on the surface).
	float4 vPos = vertex.position - pc.rpLocalViewOrigin;
	vPos = ( vPos.wwww * pc.rpLocalViewOrigin ) + vPos;

	result.position.x = dot4( vPos, pc.rpMVPmatrixX );
	result.position.y = dot4( vPos, pc.rpMVPmatrixY );
	result.position.z = dot4( vPos, pc.rpMVPmatrixZ );
	result.position.w = dot4( vPos, pc.rpMVPmatrixW );
}
