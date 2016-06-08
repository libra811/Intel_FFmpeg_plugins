/*
 * Intel MediaSDK QSV public API
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef AVCODEC_QSV_H
#define AVCODEC_QSV_H

#include <mfx/mfxvideo.h>
#include "libavfilter/avfilter.h"

#define AVFILTER_NONE     0
#define AVFILTER_VPP_ONLY 1
#define AVFILTER_MORE     2

typedef struct AVQSVContext {
    mfxSession session;
    int iopattern;

    mfxExtBuffer **ext_buffers;
    int         nb_ext_buffers;
} AVQSVContext;

/**
 * Allocate a new context.
 *
 * It must be freed by the caller with av_free().
 */
AVQSVContext *av_qsv_alloc_context(void);
int av_qsv_pipeline_connect_codec( AVCodecContext *av_dec_ctx, AVCodecContext *av_enc_ctx, int vpp_type );
int av_qsv_pipeline_insert_vpp( AVCodecContext *av_dec_ctx, AVFilterContext* vpp_ctx );
int av_qsv_pipeline_config_vpp(AVCodecContext *dec_ctx, AVFilterContext *vpp_ctx, int frame_rate_num, int frame_rate_den);

#endif /* AVCODEC_QSV_H */
