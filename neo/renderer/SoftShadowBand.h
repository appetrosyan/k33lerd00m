/*
===========================================================================
Doom 3 BFG Edition GPL Source Code
Copyright (C) 2026 Aleksandr Petrosyan
This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").
It is free software under the GNU GPL v3 (or later).
===========================================================================
*/

// THE soft-shadow AAM stencil-band encoding - the single source of truth shared by the backend
// (RenderBackend.cpp band prepass + interaction ref selection) and the unit tests
// (SoftShadowPrimitives_test.cpp), so the tested contract IS the shipped contract. Pure constants.
//
// The encoding is SIGN-AGNOSTIC because the GPU's effective stencil-face convention (nvrhi front-face
// x Vulkan viewport flip x our procedural winding) is not knowable from the C++ side, and a signed
// scheme silently inverts if it is guessed wrong (SoftContract.shell_capped_volume_* caught exactly
// that: a watertight, camera-invariant shell whose containment read -1, not +1).
//
//   CORE  (engine z-fail shadow volumes, drawn FIRST): +-1 counting in the LOW bits around
//          SOFTBAND_CORE_BASE. Umbra <=> low bits != CORE_BASE - true for either sign. Counting from
//          0x20 also keeps every CLAMP-mapped transient far from 0/255 (the old 0-based counting let
//          DECR-before-INCR clamp and fabricate umbra on lit floors).
//   SHELL (capped inflated-silhouette volumes, drawn SECOND): z-fail INVERT of SOFTBAND_SHELL_BIT on
//          BOTH faces, write-masked to that bit - pure crossing parity, facing-free by construction.
//          Ring <=> parity set while low bits are untouched. Drawing order matters: the masked INVERT
//          cannot disturb the low bits, but a core DECR borrowing across the bit boundary could
//          disturb the parity bit, so the core must be counted BEFORE the shell toggles parity.
//
//   stencil == SOFTBAND_LIT_REF   (CORE_BASE, parity clear) -> LIT: cheap unshadowed pass
//   stencil == SOFTBAND_RING_REF  (CORE_BASE | SHELL_BIT)   -> PENUMBRA RING: coverage pass
//   anything else (low bits deviated)                       -> UMBRA: no pass draws it, black
//
// Known limit (measured by the SoftShadowDefects hole detector): TWO overlapping penumbra rings have
// even parity and read as lit-or-umbra by the other bits; upgrade the shell to a 2-bit counter if the
// detector ever shows it on real content.

#ifndef __SOFTSHADOWBAND_H__
#define __SOFTSHADOWBAND_H__

#define SOFTBAND_CORE_BASE		0x20	// low-bits counting base; prepass clears the light rect to this
#define SOFTBAND_SHELL_BIT		0x40	// parity bit the shell INVERTs (write-masked)
#define SOFTBAND_LIT_REF		SOFTBAND_CORE_BASE
#define SOFTBAND_RING_REF		( SOFTBAND_CORE_BASE | SOFTBAND_SHELL_BIT )
#define SOFTBAND_CORE_REPS		2		// prepass walks global+local lists once each (one draw per surf)

#endif // __SOFTSHADOWBAND_H__
