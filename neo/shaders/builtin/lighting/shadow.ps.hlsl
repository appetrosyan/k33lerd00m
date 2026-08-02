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

// Stencil shadow-volume fill. The shadow pass writes only the stencil buffer (colour and
// depth writes are masked off), so this output is normally discarded; it is used only when
// r_showShadows visualises the volumes, where rpColor tints them.

#include "global_inc.hlsl"
#include "renderParmSet0.inc.hlsl"

// *INDENT-OFF*
struct PS_OUT
{
	float4 color : SV_Target0;
};
// *INDENT-ON*

void main( out PS_OUT result )
{
	result.color = pc.rpColor;
}
