/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 2014-2022 Robert Beckebans
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

#include "../RenderCommon.h"
#include "../Framebuffer.h"

#include "sys/DeviceManager.h"

extern DeviceManager* deviceManager;

static void R_ListFramebuffers_f( const idCmdArgs& args )
{
	// TODO
}

Framebuffer::Framebuffer( const char* name, int w, int h )
	: fboName( name )
	, frameBuffer( 0 )
	, colorFormat( 0 )
	, depthBuffer( 0 )
	, depthFormat( 0 )
	, stencilFormat( 0 )
	, stencilBuffer( 0 )
	, width( w )
	, height( h )
	, msaaSamples( false )
{
	framebuffers.Append( this );
}

Framebuffer::Framebuffer( const char* name, const nvrhi::FramebufferDesc& desc )
	: fboName( name )
	, frameBuffer( 0 )
	, colorFormat( 0 )
	, depthBuffer( 0 )
	, depthFormat( 0 )
	, stencilFormat( 0 )
	, stencilBuffer( 0 )
	, msaaSamples( false )
{
	framebuffers.Append( this );
	apiObject = deviceManager->GetDevice()->createFramebuffer( desc );
	width = apiObject->getFramebufferInfo().width;
	height = apiObject->getFramebufferInfo().height;
}

Framebuffer::~Framebuffer()
{
	apiObject.Reset();
}

void Framebuffer::Init()
{
	cmdSystem->AddCommand( "listFramebuffers", R_ListFramebuffers_f, CMD_FL_RENDERER, "lists framebuffers" );

	// HDR
	ResizeFramebuffers();
}

void Framebuffer::CheckFramebuffers()
{
	//int screenWidth = renderSystem->GetWidth();
	//int screenHeight = renderSystem->GetHeight();
}

void Framebuffer::Shutdown()
{
	framebuffers.DeleteContents( true );

	// Release the ONE ref-counted nvrhi handle living in the static globalFramebuffers struct. Left
	// alone, the static's destructor runs inside exit() AFTER the Vulkan device is gone and the
	// texture release segfaults - the crash on every quit (backtrace: ~globalFramebuffers_t ->
	// RefCounter<ITexture>::Release -> vulkan::Texture::~Texture, reproducible headless).
	globalFramebuffers.softShadowRateImage = nullptr;
	globalFramebuffers.softShadowNormalImage = nullptr;	// same exit-teardown guard as the rate image
}

void Framebuffer::ResizeFramebuffers( bool reloadImages )
{
	backEnd.ClearCaches();

	// RB: FIXME I think allocating new Framebuffers lead to a memory leak
	framebuffers.DeleteContents( true );

	if( reloadImages )
	{
		ReloadImages();
	}

	uint32_t backBufferCount = deviceManager->GetBackBufferCount();
	globalFramebuffers.swapFramebuffers.Resize( backBufferCount );
	globalFramebuffers.swapFramebuffers.SetNum( backBufferCount );

	for( uint32_t index = 0; index < backBufferCount; index++ )
	{
		globalFramebuffers.swapFramebuffers[index] = new Framebuffer(
			va( "_swapChain%d", index ),
			nvrhi::FramebufferDesc()
			.addColorAttachment( deviceManager->GetBackBuffer( index ) ) );
	}

	for( int arr = 0; arr < 6; arr++ )
	{
		for( int mip = 0; mip < MAX_SHADOWMAP_RESOLUTIONS; mip++ )
		{
			globalFramebuffers.shadowFBO[mip][arr] = new Framebuffer( va( "_shadowMap%i_%i", mip, arr ),
					nvrhi::FramebufferDesc().setDepthAttachment(
						nvrhi::FramebufferAttachment()
						.setTexture( globalImages->shadowImage[mip]->GetTextureHandle().Get() )
						.setArraySlice( arr ) ) );
		}
	}

	globalFramebuffers.shadowAtlasFBO = new Framebuffer( "_shadowAtlas",
			nvrhi::FramebufferDesc()
			.setDepthAttachment( globalImages->shadowAtlasImage->texture ) );

	globalFramebuffers.ldrFBO = new Framebuffer( "_ldr",
			nvrhi::FramebufferDesc()
			.addColorAttachment( globalImages->ldrImage->texture )
			.setDepthAttachment( globalImages->currentDepthImage->texture ) );

	// Image-driven VRS rate image for the soft-shadow interaction draw (Milestone 1 plumbing). R8_UINT sized to
	// the device tile grid; UAV (a compute pre-pass writes it) + ShadingRateSurface (the draw reads it). Created
	// unconditionally (small); only ATTACHED to hdrFBO when r_softShadowVRS>=3 so the default keeps the hdr render
	// pass VRS-free (byte-identical). Toggling the cvar needs a framebuffer rebuild (vid_restart / set at launch).
	extern idCVar r_softShadowVRS;
	nvrhi::IDevice* vrsDev = deviceManager->GetDevice();
	globalFramebuffers.softShadowRateImage = nullptr;
	globalFramebuffers.softShadowRateTile = 0;
	if( vrsDev != NULL && vrsDev->queryFeatureSupport( nvrhi::Feature::VariableRateShading ) )
	{
		nvrhi::VariableRateShadingFeatureInfo vinfo = {};
		vrsDev->queryFeatureSupport( nvrhi::Feature::VariableRateShading, &vinfo, sizeof( vinfo ) );
		uint32_t tile = vinfo.shadingRateImageTileSize > 0 ? vinfo.shadingRateImageTileSize : 16;
		globalFramebuffers.softShadowRateTile = tile;
		const uint32_t rw = globalImages->currentRenderHDRImage->texture->getDesc().width;
		const uint32_t rh = globalImages->currentRenderHDRImage->texture->getDesc().height;
		nvrhi::TextureDesc rd;
		rd.width = ( rw + tile - 1 ) / tile;
		rd.height = ( rh + tile - 1 ) / tile;
		rd.format = nvrhi::Format::R8_UINT;
		rd.dimension = nvrhi::TextureDimension::Texture2D;
		rd.isUAV = true;
		rd.isShadingRateSurface = true;
		rd.initialState = nvrhi::ResourceStates::ShadingRateSurface;
		rd.keepInitialState = true;
		rd.debugName = "_softShadowRate";
		globalFramebuffers.softShadowRateImage = vrsDev->createTexture( rd );
	}

	nvrhi::FramebufferDesc hdrDesc = nvrhi::FramebufferDesc()
			.addColorAttachment( globalImages->currentRenderHDRImage->texture )
			.setDepthAttachment( globalImages->currentDepthImage->texture );
	if( globalFramebuffers.softShadowRateImage && r_softShadowVRS.GetInteger() >= 3 )
	{
		hdrDesc.setShadingRateAttachment( globalFramebuffers.softShadowRateImage );
	}
	globalFramebuffers.hdrFBO = new Framebuffer( "_hdr", hdrDesc );

	// HDR output: isolated 2D UI layer (shares the depth/stencil with ldrFBO so GUI
	// masking still works), composited into ldrImage in linear light before present.
	globalFramebuffers.guiFBO = new Framebuffer( "_guiComposite",
			nvrhi::FramebufferDesc()
			.addColorAttachment( globalImages->guiCompositeImage->texture )
			.setDepthAttachment( globalImages->currentDepthImage->texture ) );

	globalFramebuffers.postProcFBO = new Framebuffer( "_postProc",
			nvrhi::FramebufferDesc()
			.addColorAttachment( globalImages->currentRenderImage->texture ) );

	globalFramebuffers.taaMotionVectorsFBO = new Framebuffer( "_taaMotionVectors",
			nvrhi::FramebufferDesc()
			.addColorAttachment( globalImages->taaMotionVectorsImage->texture ) );

	globalFramebuffers.taaResolvedFBO = new Framebuffer( "_taaResolved",
			nvrhi::FramebufferDesc()
			.addColorAttachment( globalImages->taaResolvedImage->texture ) );

	globalFramebuffers.envprobeFBO = new Framebuffer( "_envprobeRender",
			nvrhi::FramebufferDesc()
			.addColorAttachment( globalImages->envprobeHDRImage->texture )
			.setDepthAttachment( globalImages->envprobeDepthImage->texture ) );

	for( int i = 0; i < MAX_SSAO_BUFFERS; i++ )
	{
		globalFramebuffers.ambientOcclusionFBO[i] = new Framebuffer( va( "_aoRender%i", i ),
				nvrhi::FramebufferDesc()
				.addColorAttachment( globalImages->ambientOcclusionImage[i]->texture ) );
	}

	// HIERARCHICAL Z BUFFER
	for( int i = 0; i < MAX_HIERARCHICAL_ZBUFFERS; i++ )
	{
		globalFramebuffers.csDepthFBO[i] = new Framebuffer( va( "_csz%d", i ),
				nvrhi::FramebufferDesc().addColorAttachment(
					nvrhi::FramebufferAttachment()
					.setTexture( globalImages->hierarchicalZbufferImage->texture )
					.setMipLevel( i ) ) );
	}

	globalFramebuffers.geometryBufferFBO = new Framebuffer( "_gbuffer",
			nvrhi::FramebufferDesc()
			.addColorAttachment( globalImages->gbufferNormalsRoughnessImage->texture )
			.setDepthAttachment( globalImages->currentDepthImage->texture ) );

	// Soft shadow volumes: the light visibility buffer (rtShadowMaskImage as colour) shares the
	// scene depth-stencil so the hard shadow-volume stencil stamped into currentDepthImage gates
	// the umbra fill, and the wedge pass can depth-test against the scene.
	globalFramebuffers.softShadowMaskFBO = new Framebuffer( "_softShadowMask",
			nvrhi::FramebufferDesc()
			.addColorAttachment( globalImages->rtShadowMaskImage->texture )
			.setDepthAttachment( globalImages->currentDepthImage->texture ) );

	// Analytic soft shadow volumes accumulate signed coverage into softShadowAccumImage (R32F).
	// The baseline hard-shadow pass needs the scene depth-stencil attached (stencil stamp + depth
	// test); the wedge pass instead READS the scene depth as a shader resource to reconstruct the
	// receiver, so it uses a colour-only target (the same accum image can't be DSV and SRV at once).
	globalFramebuffers.softShadowAccumFBO = new Framebuffer( "_softShadowAccum",
			nvrhi::FramebufferDesc()
			.addColorAttachment( globalImages->softShadowAccumImage->texture )
			.setDepthAttachment( globalImages->currentDepthImage->texture ) );

	globalFramebuffers.softShadowWedgeFBO = new Framebuffer( "_softShadowWedge",
			nvrhi::FramebufferDesc()
			.addColorAttachment( globalImages->softShadowAccumImage->texture ) );

	// Analytic soft shadows, compute decoupling (r_softShadowCompute): the softpos pass re-draws
	// the depth-prepassed surfaces at depth-EQUAL against the scene depth, writing the exact
	// interpolated world position (attachment 0) for the softterm compute integral, plus the world
	// shading normal (attachment 1) for the term's N.L<=0 back-facing early-out. The normal target is
	// a STANDALONE nvrhi texture (see the softShadowNormalImage note in Framebuffer.h - keeping it out
	// of idImageManager avoids a latent layout-overflow crash); render res, RGBA16F, RT + SRV.
	{
		const uint32_t sw = globalImages->softShadowPosImage->texture->getDesc().width;
		const uint32_t sh = globalImages->softShadowPosImage->texture->getDesc().height;
		nvrhi::TextureDesc nd;
		nd.width = sw;
		nd.height = sh;
		nd.format = nvrhi::Format::RGBA16_FLOAT;
		nd.dimension = nvrhi::TextureDimension::Texture2D;
		nd.isRenderTarget = true;
		nd.initialState = nvrhi::ResourceStates::ShaderResource;
		nd.keepInitialState = true;
		nd.debugName = "_softShadowNormal";
		globalFramebuffers.softShadowNormalImage = deviceManager->GetDevice()->createTexture( nd );
	}
	globalFramebuffers.softShadowPosFBO = new Framebuffer( "_softShadowPos",
			nvrhi::FramebufferDesc()
			.addColorAttachment( globalImages->softShadowPosImage->texture )
			.addColorAttachment( globalFramebuffers.softShadowNormalImage )
			.setDepthAttachment( globalImages->currentDepthImage->texture ) );

	globalFramebuffers.smaaInputFBO = new Framebuffer( "_smaaInput",
			nvrhi::FramebufferDesc()
			.addColorAttachment( globalImages->smaaInputImage->texture ) );

	globalFramebuffers.smaaEdgesFBO = new Framebuffer( "_smaaEdges",
			nvrhi::FramebufferDesc()
			.addColorAttachment( globalImages->smaaEdgesImage->texture ) );

	globalFramebuffers.smaaBlendFBO = new Framebuffer( "_smaaBlend",
			nvrhi::FramebufferDesc()
			.addColorAttachment( globalImages->smaaBlendImage->texture ) );

	for( int i = 0; i < MAX_BLOOM_BUFFERS; i++ )
	{
		globalFramebuffers.bloomRenderFBO[i] = new Framebuffer( va( "_bloomRender%i", i ),
				nvrhi::FramebufferDesc()
				.addColorAttachment( globalImages->bloomRenderImage[i]->texture ) );
	}

	globalFramebuffers.guiRenderTargetFBO = new Framebuffer( "_guiEdit",
			nvrhi::FramebufferDesc()
			.addColorAttachment( globalImages->guiEdit->texture )
			.setDepthAttachment( globalImages->guiEditDepthStencilImage->texture ) );

	globalFramebuffers.accumFBO = new Framebuffer( "_accum",
			nvrhi::FramebufferDesc()
			.addColorAttachment( globalImages->accumImage->texture ) );

	Framebuffer::Unbind();
}

void Framebuffer::ReloadImages()
{
	backEnd.commandList->open();
	globalImages->ldrImage->Reload( false, backEnd.commandList );
	globalImages->guiCompositeImage->Reload( false, backEnd.commandList );
	globalImages->currentRenderImage->Reload( false, backEnd.commandList );
	globalImages->currentDepthImage->Reload( false, backEnd.commandList );
	globalImages->currentRenderHDRImage->Reload( false, backEnd.commandList );
	for( int i = 0; i < MAX_SSAO_BUFFERS; i++ )
	{
		globalImages->ambientOcclusionImage[i]->Reload( false, backEnd.commandList );
	}
	globalImages->hierarchicalZbufferImage->Reload( false, backEnd.commandList );
	globalImages->gbufferNormalsRoughnessImage->Reload( false, backEnd.commandList );
	globalImages->rtShadowMaskImage->Reload( false, backEnd.commandList );
	globalImages->rtShadowCoarseImage->Reload( false, backEnd.commandList );
	globalImages->softShadowPosImage->Reload( false, backEnd.commandList );
	globalImages->taaMotionVectorsImage->Reload( false, backEnd.commandList );
	globalImages->taaFeedback1Image->Reload( false, backEnd.commandList );
	globalImages->taaFeedback2Image->Reload( false, backEnd.commandList );
	globalImages->taaResolvedImage->Reload( false, backEnd.commandList );

	globalImages->smaaInputImage->Reload( false, backEnd.commandList );
	globalImages->smaaEdgesImage->Reload( false, backEnd.commandList );
	globalImages->smaaBlendImage->Reload( false, backEnd.commandList );

	globalImages->shadowAtlasImage->Reload( false, backEnd.commandList );
	for( int i = 0; i < MAX_SHADOWMAP_RESOLUTIONS; i++ )
	{
		globalImages->shadowImage[i]->Reload( false, backEnd.commandList );
	}
	for( int i = 0; i < MAX_BLOOM_BUFFERS; i++ )
	{
		globalImages->bloomRenderImage[i]->Reload( false, backEnd.commandList );
	}
	globalImages->guiEdit->Reload( false, backEnd.commandList );
	globalImages->accumImage->Reload( false, backEnd.commandList );
	backEnd.commandList->close();
	deviceManager->GetDevice()->executeCommandList( backEnd.commandList );
}

void Framebuffer::Bind()
{
	if( backEnd.currentFrameBuffer != this )
	{
		backEnd.currentPipeline = nullptr;
	}

	backEnd.lastFrameBuffer = backEnd.currentFrameBuffer;
	backEnd.currentFrameBuffer = this;
}

bool Framebuffer::IsBound()
{
	return backEnd.currentFrameBuffer == this;
}

void Framebuffer::Unbind()
{
	globalFramebuffers.swapFramebuffers[deviceManager->GetCurrentBackBufferIndex()]->Bind();
}

bool Framebuffer::IsDefaultFramebufferActive()
{
	return backEnd.currentFrameBuffer == globalFramebuffers.swapFramebuffers[deviceManager->GetCurrentBackBufferIndex()];
}

Framebuffer* Framebuffer::GetActiveFramebuffer()
{
	return backEnd.currentFrameBuffer;
}

void Framebuffer::AddColorBuffer( int format, int index, int multiSamples )
{
}

void Framebuffer::AddDepthBuffer( int format, int multiSamples )
{
}

void Framebuffer::AddStencilBuffer( int format, int multiSamples )
{
}

void Framebuffer::AttachImage2D( int target, idImage* image, int index, int mipmapLod )
{
}

void Framebuffer::AttachImageDepth( int target, idImage* image )
{
}

void Framebuffer::AttachImageDepthLayer( idImage* image, int layer )
{
}

void Framebuffer::Check()
{
}

idScreenRect Framebuffer::GetViewPortInfo() const
{
	nvrhi::Viewport viewport = apiObject->getFramebufferInfo().getViewport();
	idScreenRect screenRect;
	screenRect.Clear();
	screenRect.AddPoint( viewport.minX, viewport.minY );
	screenRect.AddPoint( viewport.maxX, viewport.maxY );
	return screenRect;
}