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

// The procedural wedge-coverage prog (softwedge_proc.vs) reuses the exact per-edge coverage pixel shader.
// The builtin loader pairs VS+PS by shared base name, so this thin file exists only to give softwedge_proc
// a matching .ps; the coverage logic (CoverageSphere, f - hard, silWeight, halfWidth) lives once in
// softwedge.ps.hlsl. Both files include renderParmSet11, so the shared PS reconstructs the receiver here too.
#include "softwedge.ps.hlsl"
