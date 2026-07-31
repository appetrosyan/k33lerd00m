/*
* Copyright (c) 2014-2021, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

#pragma once

struct ToneMappingConstants
{
	idVec2i viewOrigin;
	idVec2i viewSize;

	float logLuminanceScale;
	float logLuminanceBias;
	float histogramLowPercentile;
	float histogramHighPercentile;

	float eyeAdaptationSpeedUp;
	float eyeAdaptationSpeedDown;
	float minAdaptedLuminance;
	float maxAdaptedLuminance;

	float frameTime;
	float exposureScale;
	float whitePointInvSquared;
	uint sourceSlice;

	idVec2 colorLUTTextureSize;
	idVec2 colorLUTTextureSizeInv;

	// HDR display output (scRGB) - keep in sync with shaders/builtin/post/tonemapping_cb.h
	float hdrEnabled;		// 0 = SDR, 1 = HDR
	float hdrPaperScale;	// paperWhiteNits / 80
	float hdrPeak;			// maxNits / paperWhiteNits
	float hdrOperator;		// runtime tone curve: 0 linear, 1 Reinhard, 2 ACES, 3 Hable

	float hdrStrength;		// blend operator toward linear (0..1)
	float ssaaScale;		// SSAA: supersample factor of the source vs native output (1 = off)
	float hdrPad1;
	float hdrPad2;
};