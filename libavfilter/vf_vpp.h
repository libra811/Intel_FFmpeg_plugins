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
#include "framesync.h"

#include "libavutil/avassert.h"
#include "libavutil/opt.h"
#include "libavutil/time.h"
#include "libavutil/avstring.h"
#include "libavutil/error.h"
#include "libavcodec/avcodec.h"
#include "libavcodec/qsvenc.h"
#include "libavcodec/qsv_internal.h"
#include "libavformat/avformat.h"
#include "libavutil/pixdesc.h"
#include "libswscale/swscale.h"

// number of video enhancement filters (denoise, procamp, detail, video_analysis, image stab)
#define ENH_FILTERS_COUNT           5
enum{
    VPP_PAD_MAIN = 0,
    VPP_PAD_OVERLAY,
    VPP_PAD_NUM,
};

typedef struct VPPInterContext {
    mfxSession session;
    QSVSession internal_qs;
    mfxVideoParam  VppParam;
    mfxVideoParam *pVppParam;
    mfxFrameAllocRequest req[2];              // [0] - in, [1] - out
    mfxFrameAllocResponse *in_response;
    mfxFrameAllocResponse *out_response;
    mfxFrameSurface1 **in_surface;
    mfxFrameSurface1 **out_surface;
    int *num_surfaces_in;                      // input surfaces
    int num_surfaces_out;                     // output surfaces
    int sysmem_cur_out_idx;
    int nb_inputs;                            // number inputs for composite
    /* VPP extension */
    mfxExtBuffer*       pExtBuf[1+ENH_FILTERS_COUNT];
    mfxExtVppAuxData    extVPPAuxData;
} VPPInterContext;

typedef struct {
    const AVClass *class;

    AVFilterContext *ctx;
    QSVEncContext* enc_ctx;

    int             num_vpp;
    VPPInterContext inter_vpp[2];
    mfxFrameAllocator inter_alloc;

    AVFifoBuffer      *thm_framebuffer;
    pthread_t          thumbnail_task;
    int                task_exit;
    int                thm_pendding;
    AVFormatContext   *thm_mux;
    AVStream          *thm_stream;
    AVCodecContext    *thm_enc;
    struct SwsContext *thm_swsctx;

    mfxFrameAllocator *pFrameAllocator;
    FFFrameSync       *fs;
    int                frame_number;
    int                vpp_ready;
    int64_t            first_pts;

    /* Video Enhancement Algorithms */
    mfxExtVPPDeinterlacing  deinterlace_conf;
    mfxExtVPPFrameRateConversion frc_conf;
    mfxExtVPPDenoise denoise_conf;
    mfxExtVPPDetail detail_conf;
    mfxExtVPPProcAmp procamp_conf;
    mfxExtVPPComposite composite_conf;

    /*user-defined parameters*/
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
    int use_frc;                // use framerate conversion
    char *load_plugins;
    char *thumbnail_file;
    int thumb_interval;
    int use_thumbnail;

    /* param for the procamp */
    int    procamp;            //enable the procamp
    float  hue;
    float  saturation;
    float  contrast;
    float  brightness;

    int use_composite;          // 1 = use composite; 0=none
    int use_crop;               // 1 = use crop; 0=none
    int crop_w;
    int crop_h;
    int crop_x;
    int crop_y;
    mfxVPPCompInputStream layout[VPP_PAD_NUM];
    char *ow, *oh;
    char *main_ox, *main_oy, *main_ow, *main_oh;
    char *cx, *cy, *cw, *ch;
    char *overlay_ox, *overlay_oy, *overlay_ow, *overlay_oh;
    AVRational framerate;       // target framerate
    int eof_action;             // actions when encountering EOF of overlay
} VPPContext;

#endif
