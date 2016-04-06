/*
 * Intel MediaSDK Quick Sync Video VPP filter
 *
 * copyright (c) 2015 Sven Dueking
 *
 * This file is part of Libav.
 *
 * Libav is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Libav; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */
#ifndef AVFILTER_QSV_VPP_H
#define AVFILTER_QSV_VPP_H

#include <mfx/mfxvideo.h>
#include <mfx/mfxplugin.h>

#include "avfilter.h"
//#include "internal.h"

#include "libavutil/avassert.h"
#include "libavutil/opt.h"
#include "libavutil/time.h"
#include "libavutil/avstring.h"
#include "libavutil/error.h"
#include "libavcodec/avcodec.h"
#include "libavcodec/qsvenc.h"
#include "libavcodec/qsv_internal.h"


// number of video enhancement filters (denoise, procamp, detail, video_analysis, image stab)
#define ENH_FILTERS_COUNT           5

typedef struct {
    const AVClass *class;

    AVFilterContext *ctx;

    mfxSession session;
    QSVSession internal_qs;

    AVRational framerate;                           // target framerate

	QSVFrame *in_work_frames;                       // used for video memory
	QSVFrame *out_work_frames;                      // used for video memory

    mfxFrameSurface1 **in_surface;
    mfxFrameSurface1 **out_surface;

    mfxFrameAllocRequest req[2];                    // [0] - in, [1] - out
    mfxFrameAllocator *pFrameAllocator;	
    mfxFrameAllocResponse* out_response;
    QSVEncContext* enc_ctx;
	
	int num_surfaces_in;                            // input surfaces
    int num_surfaces_out;                           // output surfaces

    unsigned char * surface_buffers_out;            // output surface buffer

    char *load_plugins;

    mfxVideoParam*      pVppParam;

    /* VPP extension */
    mfxExtBuffer*       pExtBuf[1+ENH_FILTERS_COUNT];
    mfxExtVppAuxData    extVPPAuxData;

    /* Video Enhancement Algorithms */
    mfxExtVPPDeinterlacing  deinterlace_conf;
    mfxExtVPPFrameRateConversion frc_conf;
    mfxExtVPPDenoise denoise_conf;
    mfxExtVPPDetail detail_conf;

    int out_width;
    int out_height;

    int dpic;                   // destination picture structure
                                // -1 = unkown
                                // 0 = interlaced top field first
                                // 1 = progressive
                                // 2 = interlaced bottom field first 

    int deinterlace;            // deinterlace mode : 0=off, 1=bob, 2=advanced
    int denoise;                // Enable Denoise algorithm. Level is the optional value from the interval [0; 100]
    int detail;                 // Enable Detail Enhancement algorithm.
                                // Level is the optional value from the interval [0; 100]
    int async_depth;            // async dept used by encoder
    int max_b_frames;           // maxiumum number of b frames used by encoder

    int cur_out_idx;            // current surface in index

    int frame_number;

    int use_frc;                // use framerate conversion

} VPPContext;

#endif
