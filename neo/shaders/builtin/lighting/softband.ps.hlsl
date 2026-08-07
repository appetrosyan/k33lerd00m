/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2026 Aleksandr Petrosyan

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

===========================================================================
*/

// Analytic soft shadows - penumbra-band stencil prepass (pixel shader).
//
// The band prepass writes ONLY stencil (via two-sided z-fail); colour writes are masked off by the
// backend GL_State. This pixel shader therefore produces no output - its only job is to let the
// rasteriser generate fragments so the stencil op runs. See softband.vs.hlsl.

#include "global_inc.hlsl"

void main()
{
}
