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
#ifndef RENDERER_PASSES_HDRGUICOMPOSITEPASS_H_
#define RENDERER_PASSES_HDRGUICOMPOSITEPASS_H_

/*
================================================================================

	HdrGuiCompositePass - composite the isolated 2D UI layer into the linear scRGB
	scene when presenting to an HDR display (r_hdrOutput).

	The 2D UI is rendered in sRGB/gamma space into its own buffer (guiCompositeImage);
	this compute pass converts it to linear, scales it to paper-white nits, and
	alpha-composites it over the already-linear scene held in ldrImage, in place.

	Only used in HDR output mode; the SDR path draws 2D straight into ldrImage and
	never invokes this pass.

================================================================================
*/

class idImage;

class HdrGuiCompositePass
{
public:
	HdrGuiCompositePass( nvrhi::IDevice* device );
	~HdrGuiCompositePass();

	// Composite guiCompositeImage over ldrImage (both globalImages), in place.
	void			Render( nvrhi::ICommandList* commandList );

private:
	nvrhi::DeviceHandle				m_Device;
	nvrhi::BufferHandle				m_ConstantBuffer;
	nvrhi::ShaderHandle				m_Shader;
	nvrhi::BindingLayoutHandle		m_BindingLayout;
	nvrhi::BindingSetHandle			m_BindingSet;
	nvrhi::ComputePipelineHandle	m_Pipeline;

	// rebuild the binding set when the backing GPU textures are (re)created on a
	// resize / reload / vid_restart. Tracked by texture handle, NOT idImage pointer:
	// ReloadImages/ResizeFramebuffers reallocate the texture while keeping the same
	// idImage object, so a pointer check would keep a stale (freed) handle bound.
	// Non-owning raw pointers, for identity comparison only.
	nvrhi::ITexture*				m_BoundGuiTex;
	nvrhi::ITexture*				m_BoundLdrTex;

	bool							m_Valid;
};

#endif
