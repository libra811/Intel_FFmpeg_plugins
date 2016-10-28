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

#include "internal.h"
#include "vf_vpp.h"
#include <float.h>
#include <pthread.h>
#include "libavutil/parseutils.h"
#include "libavutil/timestamp.h"
#include "libavutil/eval.h"
#include "libavcodec/qsv.h"
#include "libavcodec/qsvdec.h"
#include "libavcodec/vaapi_allocator.h"

/**
 * ToDo :
 *
 * - double check surface pointers for different fourccs
 * - handle empty extbuffers
 * - cropping
 * - use AV_PIX_FMT_QSV to pass surfaces to encoder
 * - deinterlace check settings etc.
 * - allocate number of surfaces depending modules and number of b frames
 */

#define VPP_ZERO_MEMORY(VAR)        { memset(&VAR, 0, sizeof(VAR)); }
#define VPP_ALIGN16(value)          (((value + 15) >> 4) << 4)          // round up to a multiple of 16
#define VPP_ALIGN32(value)          (((value + 31) >> 5) << 5)          // round up to a multiple of 32
#define VPP_CHECK_POINTER(P, ...)   {if (!(P)) {return __VA_ARGS__;}}
#define VPP_FLEX_MAIN 0

enum EOFAction {
    EOF_ACTION_REPEAT = 0,
    EOF_ACTION_ENDALL,
    EOF_ACTION_PASS
};

#define OFFSET(x) offsetof(VPPContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM

/* width must be a multiple of 16
+ * height must be a multiple of 16 in case of frame picture and a multiple of 32 in case of field picture
+ */
#define INIT_FRAMEINFO(frameinfo, format, w, h, pic_struct, tbn, tbd) {\
    (frameinfo).FourCC = avpix_fmt_to_mfx_fourcc(format); \
    (frameinfo).ChromaFormat = get_chroma_fourcc((frameinfo).FourCC); \
    (frameinfo).CropX = 0; \
    (frameinfo).CropY = 0; \
    (frameinfo).CropW = w; \
    (frameinfo).CropH = h; \
    (frameinfo).PicStruct = pic_struct; \
    (frameinfo).FrameRateExtN = tbn; \
    (frameinfo).FrameRateExtD = tbd; \
    (frameinfo).BitDepthLuma   = 8; \
    (frameinfo).BitDepthChroma = 8; \
    (frameinfo).Width = VPP_ALIGN16(w); \
    (frameinfo).Height = \
        (MFX_PICSTRUCT_PROGRESSIVE == pic_struct) ? \
        VPP_ALIGN16(h) : VPP_ALIGN32(h); \
}

 static const AVOption vpp_options[] = {
    { "deinterlace", "deinterlace mode: 0=off, 1=bob, 2=advanced",             OFFSET(deinterlace),  AV_OPT_TYPE_INT, {.i64=0}, 0, MFX_DEINTERLACING_ADVANCED, .flags = FLAGS },
    { "denoise",     "denoise level [0, 100]",                                 OFFSET(denoise),      AV_OPT_TYPE_INT, {.i64=0}, 0, 100, .flags = FLAGS },
    { "detail",      "detail enhancement level [0, 100]",                      OFFSET(detail),       AV_OPT_TYPE_INT, {.i64=0}, 0, 100, .flags = FLAGS },
    { "dpic",        "dest pic struct: 0=tff, 1=progressive [default], 2=bff", OFFSET(dpic),         AV_OPT_TYPE_INT, {.i64 = 1 }, 0, 2, .flags = FLAGS },
    { "framerate",   "output framerate",                                       OFFSET(framerate),    AV_OPT_TYPE_RATIONAL, { .dbl = 0.0 },0, DBL_MAX, .flags = FLAGS },
    { "async_depth", "Maximum processing parallelism [default = 4]",           OFFSET(async_depth),  AV_OPT_TYPE_INT, { .i64 = ASYNC_DEPTH_DEFAULT }, 0, INT_MAX, .flags = FLAGS },
    { "max_b_frames","Maximum number of b frames  [default = 3]",              OFFSET(max_b_frames), AV_OPT_TYPE_INT, { .i64 = 3 }, 0, INT_MAX, .flags = FLAGS },
    { "gpu_copy", "Enable gpu copy in sysmem mode [default = off]",            OFFSET(inter_vpp[0].internal_qs.gpu_copy), AV_OPT_TYPE_INT, { .i64 = MFX_GPUCOPY_OFF }, MFX_GPUCOPY_DEFAULT, MFX_GPUCOPY_OFF, .flags = FLAGS, "gpu_copy" },
    { "default", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_GPUCOPY_DEFAULT }, MFX_GPUCOPY_DEFAULT, MFX_GPUCOPY_OFF, .flags = FLAGS, "gpu_copy" },
    { "on", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_GPUCOPY_ON }, MFX_GPUCOPY_DEFAULT, MFX_GPUCOPY_OFF, .flags = FLAGS, "gpu_copy" },
    { "off", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_GPUCOPY_OFF }, MFX_GPUCOPY_DEFAULT, MFX_GPUCOPY_OFF, .flags = FLAGS, "gpu_copy" },
    { "thumbnail",   "Enable automatic thumbnail",                             OFFSET(use_thumbnail), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, .flags = FLAGS},
    { "thumb_interval","Thumbnail interval in frame",                          OFFSET(thumb_interval), AV_OPT_TYPE_INT, {.i64 = INT_MAX}, 1, INT_MAX, .flags = FLAGS},
    { "thumb_file",  "Thumbnail filename [default = thumbnail-%d.jpg]",        OFFSET(thumbnail_file), AV_OPT_TYPE_STRING, {.str = NULL}, 1, 128, .flags = FLAGS},

    { "procamp",     "Enable ProcAmp",                                         OFFSET(procamp),    AV_OPT_TYPE_INT,   {.i64 = 0}, 0, 1, .flags = FLAGS},
    { "hue",         "ProcAmp hue",                                            OFFSET(hue),        AV_OPT_TYPE_FLOAT, {.dbl = 0.0 }, -180.0, 180.0, .flags = FLAGS},
    { "saturation",  "ProcAmp saturation",                                     OFFSET(saturation), AV_OPT_TYPE_FLOAT, {.dbl = 1.0 }, 0.0, 10.0, .flags = FLAGS},
    { "contrast",    "ProcAmp contrast",                                       OFFSET(contrast),   AV_OPT_TYPE_FLOAT, {.dbl = 1.0 }, 0.0, 10.0, .flags = FLAGS},
    { "brightness",  "ProcAmp brightness",                                     OFFSET(brightness), AV_OPT_TYPE_FLOAT, {.dbl = 0.0 }, -100.0, 100.0, .flags = FLAGS},

    { "w", "Output video width", OFFSET(ow), AV_OPT_TYPE_STRING, {.str="iw"}, 0, 255, .flags = FLAGS },
    { "width", "Output video width", OFFSET(ow), AV_OPT_TYPE_STRING, {.str="iw"}, 0, 255, .flags = FLAGS },
    { "h", "Output video height", OFFSET(oh), AV_OPT_TYPE_STRING, {.str="w*ih/iw"}, 0, 255, .flags = FLAGS },
    { "height", "Output video height", OFFSET(oh), AV_OPT_TYPE_STRING, {.str="w*ih/iw"}, 0, 255, .flags = FLAGS },

    { "overlay_type", "Overlay enable", OFFSET(use_composite), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, .flags = FLAGS},
#if VPP_FLEX_MAIN
    { "main_x", "Main x position", OFFSET(main_ox), AV_OPT_TYPE_STRING, {.str="0"}, 0, 255, .flags = FLAGS},
    { "main_y", "Main y position", OFFSET(main_oy), AV_OPT_TYPE_STRING, {.str="0"}, 0, 255, .flags = FLAGS},
    { "main_w", "Main width", OFFSET(main_ow), AV_OPT_TYPE_STRING, {.str="0"}, 0, 255, .flags = FLAGS},
    { "main_h", "Main height", OFFSET(main_oh), AV_OPT_TYPE_STRING, {.str="0"}, 0, 255, .flags = FLAGS},
    { "main_alpha",  "Main global alpha", OFFSET(layout[VPP_PAD_MAIN].GlobalAlpha), AV_OPT_TYPE_INT, {.i64 = 255}, 0, 255, .flags = FLAGS},
#endif
    { "overlay_x", "Overlay x position", OFFSET(overlay_ox), AV_OPT_TYPE_STRING, {.str="0"}, 0, 255, .flags = FLAGS},
    { "overlay_y", "Overlay y position", OFFSET(overlay_oy), AV_OPT_TYPE_STRING, {.str="0"}, 0, 255, .flags = FLAGS},
    { "overlay_w", "Overlay width", OFFSET(overlay_ow), AV_OPT_TYPE_STRING, {.str="overlay_iw"}, 0, 255, .flags = FLAGS},
    { "overlay_h", "Overlay height", OFFSET(overlay_oh), AV_OPT_TYPE_STRING, {.str="overlay_ih*overlay_w/overlay_iw"}, 0, 255, .flags = FLAGS},
    { "overlay_alpha", "Overlay global alpha", OFFSET(layout[VPP_PAD_OVERLAY].GlobalAlpha), AV_OPT_TYPE_INT, {.i64 = 255}, 0, 255, .flags = FLAGS},
    { "overlay_pixel_alpha", "Overlay per-piexel alpha", OFFSET(layout[VPP_PAD_OVERLAY].PixelAlphaEnable), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, .flags = FLAGS},
    { "eof_action", "Action to take when encountering EOF from overlay input", OFFSET(eof_action), AV_OPT_TYPE_INT, { .i64 = EOF_ACTION_REPEAT }, EOF_ACTION_REPEAT, EOF_ACTION_ENDALL, .flags = FLAGS, "eof_action" },
        { "repeat", "Repeat the previous frame.",   0, AV_OPT_TYPE_CONST, { .i64 = EOF_ACTION_REPEAT }, .flags = FLAGS, "eof_action" },
        { "endall", "End both streams.",            0, AV_OPT_TYPE_CONST, { .i64 = EOF_ACTION_ENDALL }, .flags = FLAGS, "eof_action" },

    { NULL }
 };

AVFILTER_DEFINE_CLASS(vpp);

static const char *const var_names[] = {
    "main_iw",    "iw", "in_w",
    "main_ih",    "ih", "in_h",
    "overlay_iw",
    "overlay_ih",
    "main_w",     "ow", "out_w", "w",
    "main_h",     "oh", "out_h", "h",
#if VPP_FLEX_MAIN
    "main_ox",
    "main_oy",
    "main_ow",
    "main_oh",
#endif
    "overlay_x",  "x",
    "overlay_y",  "y",
    "overlay_w",
    "overlay_h",
    NULL
};

enum var_name {
    VAR_MAIN_iW,    VAR_iW, VAR_IN_W,
    VAR_MAIN_iH,    VAR_iH, VAR_IN_H,
    VAR_OVERLAY_iW,
    VAR_OVERLAY_iH,
    VAR_MAIN_W,     VAR_oW, VAR_OUT_W, VAR_W,
    VAR_MAIN_H,     VAR_oH, VAR_OUT_H, VAR_H,
#if VPP_FLEX_MAIN
    VAR_MAIN_oX,
    VAR_MAIN_oY,
    VAR_MAIN_oW,
    VAR_MAIN_oH,
#endif
    VAR_OVERLAY_X, VAR_X,
    VAR_OVERLAY_Y, VAR_Y,
    VAR_OVERLAY_W,
    VAR_OVERLAY_H,
    VAR_VARS_NB
};

static int option_id_to_mfx_pic_struct(int id)
{
    switch (id) {
        case 0:
            return MFX_PICSTRUCT_FIELD_TFF;
        case 1:
            return MFX_PICSTRUCT_PROGRESSIVE;
        case 2:
            return MFX_PICSTRUCT_FIELD_BFF;
        default:
            return MFX_PICSTRUCT_UNKNOWN;
    }
    return MFX_PICSTRUCT_UNKNOWN;
}

static int get_chroma_fourcc(unsigned int fourcc)
{
    switch (fourcc) {
        case MFX_FOURCC_YUY2:
            return MFX_CHROMAFORMAT_YUV422;
        case MFX_FOURCC_RGB4:
            return MFX_CHROMAFORMAT_YUV444;
        default:
            return MFX_CHROMAFORMAT_YUV420;
    }
    return MFX_CHROMAFORMAT_YUV420;
}

static int avframe_id_to_mfx_pic_struct(AVFrame * pic)
{
    if (pic->interlaced_frame == 0)
        return MFX_PICSTRUCT_PROGRESSIVE;

    if (pic->top_field_first == 1)
        return MFX_PICSTRUCT_FIELD_TFF;

    return MFX_PICSTRUCT_FIELD_BFF;
}

static int avpix_fmt_to_mfx_fourcc(int format)
{
    switch(format){
        case AV_PIX_FMT_YUV420P:
            return MFX_FOURCC_YV12;
        case AV_PIX_FMT_NV12:
            return MFX_FOURCC_NV12;
        case AV_PIX_FMT_YUYV422:
            return MFX_FOURCC_YUY2;
        case AV_PIX_FMT_RGB32:
            return MFX_FOURCC_RGB4;
    }

    return MFX_FOURCC_NV12;
}

static int field_order_to_mfx_pic_struct(AVCodecContext *ctx)
{
    if ( (ctx->field_order == AV_FIELD_BB) || (ctx->field_order == AV_FIELD_TB) )
        return MFX_PICSTRUCT_FIELD_BFF;

    if ( (ctx->field_order == AV_FIELD_TT) || (ctx->field_order == AV_FIELD_BT) )
        return MFX_PICSTRUCT_FIELD_TFF;

    return MFX_PICSTRUCT_PROGRESSIVE;
}

static void input_init_surface(VPPContext *vpp, int vppidx)
{
    VPPInterContext *inter_vpp = &vpp->inter_vpp[vppidx];
    mfxFrameAllocator *pFrameAllocator = NULL;

    inter_vpp->in_surface  =
        av_calloc(inter_vpp->nb_inputs, sizeof(mfxFrameSurface1*));
    VPP_CHECK_POINTER(inter_vpp->in_surface);

    inter_vpp->num_surfaces_in =
        av_calloc(inter_vpp->nb_inputs, sizeof(*inter_vpp->num_surfaces_in));
    VPP_CHECK_POINTER(inter_vpp->num_surfaces_in);

    if (inter_vpp->pVppParam->IOPattern & MFX_IOPATTERN_IN_VIDEO_MEMORY) {
        if (vpp->pFrameAllocator)
            pFrameAllocator = vpp->pFrameAllocator;
        else
            pFrameAllocator = &vpp->inter_alloc;

        inter_vpp->in_response =
            av_calloc(inter_vpp->nb_inputs, sizeof(*inter_vpp->in_response));
        VPP_CHECK_POINTER(inter_vpp->in_response);

        for (int i = VPP_PAD_OVERLAY; i < inter_vpp->nb_inputs; i++) {
            mfxFrameAllocRequest req = inter_vpp->req[0];
            INIT_FRAMEINFO(req.Info,
                vpp->ctx->inputs[i]->format,
                vpp->ctx->inputs[i]->w,
                vpp->ctx->inputs[i]->h,
                MFX_PICSTRUCT_UNKNOWN,
                vpp->ctx->inputs[i]->frame_rate.num,
                vpp->ctx->inputs[i]->frame_rate.den);

            pFrameAllocator->Alloc(pFrameAllocator->pthis,
                &req, &inter_vpp->in_response[i]);

            inter_vpp->num_surfaces_in[i] = inter_vpp->in_response[i].NumFrameActual;
        }
    } else {
        for (int i = 0; i < inter_vpp->nb_inputs; i++)
            inter_vpp->num_surfaces_in[i] = FFMAX(inter_vpp->req[0].NumFrameSuggested,
                vpp->async_depth + vpp->max_b_frames + 1);
    }

    for (int i = 0; i < inter_vpp->nb_inputs; i++) {
        inter_vpp->in_surface[i] =
            av_calloc(inter_vpp->num_surfaces_in[i], sizeof(mfxFrameSurface1));

        for (int j = 0; j < inter_vpp->num_surfaces_in[i]; j++) {
            if (i == VPP_PAD_MAIN)
                memcpy(&inter_vpp->in_surface[i][j].Info,
                    &inter_vpp->pVppParam->vpp.In,
                    sizeof(inter_vpp->in_surface[i][j].Info));
            else {
                INIT_FRAMEINFO(inter_vpp->in_surface[i][j].Info,
                    vpp->ctx->inputs[i]->format,
                    vpp->ctx->inputs[i]->w,
                    vpp->ctx->inputs[i]->h,
                    MFX_PICSTRUCT_UNKNOWN,
                    vpp->ctx->inputs[i]->frame_rate.num,
                    vpp->ctx->inputs[i]->frame_rate.den);
                if (inter_vpp->in_response)
                    inter_vpp->in_surface[i][j].Data.MemId =
                        inter_vpp->in_response[i].mids[j];
            }
        }
    }
}

static void output_init_surface(VPPContext *vpp, int vppidx)
{
    VPPInterContext *inter_vpp = &vpp->inter_vpp[vppidx];
    mfxFrameAllocator *pFrameAllocator = NULL;

    if (inter_vpp->pVppParam->IOPattern & MFX_IOPATTERN_OUT_VIDEO_MEMORY) {
        if (vpp->pFrameAllocator)
            pFrameAllocator = vpp->pFrameAllocator;
        else
            pFrameAllocator = &vpp->inter_alloc;

        inter_vpp->out_response = av_mallocz(sizeof(*inter_vpp->out_response));
        VPP_CHECK_POINTER(inter_vpp->out_response);

        pFrameAllocator->Alloc(pFrameAllocator->pthis,
            &(inter_vpp->req[1]), inter_vpp->out_response);

        inter_vpp->num_surfaces_out = inter_vpp->out_response->NumFrameActual;
    } else
        inter_vpp->num_surfaces_out = FFMAX(inter_vpp->req[1].NumFrameSuggested, 1);

    inter_vpp->out_surface = av_mallocz(sizeof(*inter_vpp->out_surface)
        * inter_vpp->num_surfaces_out);
    VPP_CHECK_POINTER(inter_vpp->out_surface);

    for (int i = 0; i < inter_vpp->num_surfaces_out; i++) {
        inter_vpp->out_surface[i] = av_mallocz(sizeof(*inter_vpp->out_surface[i]));
        VPP_CHECK_POINTER(inter_vpp->out_surface[i]);

        memcpy(&(inter_vpp->out_surface[i]->Info), &(inter_vpp->pVppParam->vpp.Out),
            sizeof(inter_vpp->out_surface[i]->Info));

        if (inter_vpp->out_response)
            inter_vpp->out_surface[i]->Data.MemId = inter_vpp->out_response->mids[i];
    }
}

static void vpp_init_surface(VPPContext *vpp)
{
    av_log(vpp->ctx, AV_LOG_INFO, "vpp_init_surface:");

    if (NULL != vpp->enc_ctx) {
        vpp->inter_vpp[vpp->num_vpp - 1].req[1].NumFrameSuggested
            += vpp->enc_ctx->req.NumFrameSuggested;
        av_log(vpp->ctx, AV_LOG_INFO, "enc_ctx.num=%d \n",
            vpp->enc_ctx->req.NumFrameSuggested);
    }

    /* Allocate input surfaces.
     * Note: main input's surfaces are allocated by decoder, so we just allocate
     * overlay's surfaces.
     */
    for (int vppidx = 0; vppidx < vpp->num_vpp; vppidx++) {
        av_log(vpp->ctx, AV_LOG_INFO, "vpp[%d]: in.num = %d, out.num = %d\n", vppidx,
            vpp->inter_vpp[vppidx].req[0].NumFrameSuggested,
            vpp->inter_vpp[vppidx].req[1].NumFrameSuggested);
        input_init_surface(vpp, vppidx);
        output_init_surface(vpp, vppidx);
    }
}

static void vpp_free_surface(VPPContext *vpp)
{
    mfxFrameAllocator *pFrameAllocator = NULL;

    if (vpp->pFrameAllocator)
        pFrameAllocator = vpp->pFrameAllocator;
    else
        pFrameAllocator = &vpp->inter_alloc;

    for (int vppidx = 0; vppidx < vpp->num_vpp; vppidx++) {
        if (NULL != vpp->inter_vpp[vppidx].in_surface) {
            for (int i = 0; i < vpp->inter_vpp[vppidx].nb_inputs; i++)
                av_freep(&vpp->inter_vpp[vppidx].in_surface[i]);
            av_freep(&vpp->inter_vpp[vppidx].in_surface);
        }

        if (NULL != vpp->inter_vpp[vppidx].in_response) {
            for(int i = VPP_PAD_OVERLAY; i < vpp->inter_vpp[vppidx].nb_inputs; i++)
                pFrameAllocator->Free(pFrameAllocator->pthis, &vpp->inter_vpp[vppidx].in_response[i]);
            av_freep(&vpp->inter_vpp[vppidx].in_response);
        }

        if (NULL != vpp->inter_vpp[vppidx].out_surface) {
            for (int i = 0; i < vpp->inter_vpp[vppidx].num_surfaces_out; i++)
                av_freep(&vpp->inter_vpp[vppidx].out_surface[i]);
            av_freep(&vpp->inter_vpp[vppidx].out_surface);
        }

        if (NULL != vpp->inter_vpp[vppidx].out_response) {
            pFrameAllocator->Free(pFrameAllocator->pthis, vpp->inter_vpp[vppidx].out_response);
            av_freep(&vpp->inter_vpp[vppidx].out_response);
        }

        av_freep(&vpp->inter_vpp[vppidx].num_surfaces_in);
        vpp->inter_vpp[vppidx].num_surfaces_out = 0;
    }
}

static int get_free_surface_index_in(AVFilterContext *ctx, mfxFrameSurface1 *surface_pool, int pool_size)
{
    if (surface_pool) {
        for (mfxU16 i = 0; i < pool_size; i++) {
            if (0 == surface_pool[i].Data.Locked)
                return i;
        }
    }

    av_log(ctx, AV_LOG_ERROR, "Error getting a free surface, pool size is %d\n", pool_size);
    return MFX_ERR_NOT_FOUND;
}

static int get_free_surface_index_out(mfxFrameSurface1 ** surface_pool, int start_idx, int pool_size)
{
    if (surface_pool) {
        for (int i = start_idx; i < pool_size; i++)
            if (0 == surface_pool[i]->Data.Locked)
                return i;
    }

    return MFX_ERR_NOT_FOUND;
}
static int sysmem_map_frame_to_surface(VPPContext *vpp, AVFrame *frame, mfxFrameSurface1 *surface)
{
    surface->Info.PicStruct = avframe_id_to_mfx_pic_struct(frame);

    switch (frame->format) {
        case AV_PIX_FMT_NV12:
            surface->Data.Y = frame->data[0];
            surface->Data.VU = frame->data[1];
        break;

        case AV_PIX_FMT_YUV420P:
            surface->Data.Y = frame->data[0];
            surface->Data.U = frame->data[1];
            surface->Data.V = frame->data[2];
        break;

        case AV_PIX_FMT_YUYV422:
            surface->Data.Y = frame->data[0];
            surface->Data.U = frame->data[0] + 1;
            surface->Data.V = frame->data[0] + 3;
        break;

        case AV_PIX_FMT_RGB32:
            surface->Data.B = frame->data[0];
            surface->Data.G = frame->data[0] + 1;
            surface->Data.R = frame->data[0] + 2;
            surface->Data.A = frame->data[0] + 3;
        break;

        default:
            return MFX_ERR_UNSUPPORTED;
    }
    surface->Data.Pitch = frame->linesize[0];
    surface->Data.TimeStamp = frame->pts;

    return 0;
}

static int vidmem_map_frame_to_surface(VPPContext *vpp, AVFrame *frame, mfxFrameSurface1 *surface)
{
    mfxFrameData data;
    mfxFrameAllocator *pFrameAllocator = NULL;

    if (vpp->pFrameAllocator)
        pFrameAllocator = vpp->pFrameAllocator;
    else
        pFrameAllocator = &vpp->inter_alloc;

    pFrameAllocator->Lock(pFrameAllocator->pthis, surface->Data.MemId, &data);
    switch (frame->format) {
        case AV_PIX_FMT_NV12:
            for(int i=0; i<frame->height; i++)
                memcpy(data.Y + data.Pitch*i, frame->data[0] + frame->linesize[0]*i, frame->linesize[0]);
            for(int i=0; i<frame->height/2; i++)
                memcpy(data.UV + data.Pitch*i, frame->data[1] + frame->linesize[1]*i, frame->linesize[1]);
        break;

        case AV_PIX_FMT_YUV420P:
            for(int i=0; i<frame->height; i++)
                memcpy(data.Y + data.Pitch*i, frame->data[0] + frame->linesize[0]*i, frame->linesize[0]);
            for(int i=0; i<frame->height/2; i++)
                memcpy(data.U + data.Pitch/2*i, frame->data[1] + frame->linesize[1]*i, frame->linesize[1]);
            for(int i=0; i<frame->height/2; i++)
                memcpy(data.V + data.Pitch/2*i, frame->data[2] + frame->linesize[2]*i, frame->linesize[2]);
        break;

        case AV_PIX_FMT_YUYV422:
            for(int i=0; i<frame->height; i++)
                memcpy(data.Y + data.Pitch*i, frame->data[0] + frame->linesize[0]*i, frame->linesize[0]);
        break;

        case AV_PIX_FMT_RGB32:
            for(int i=0; i<frame->height; i++)
                memcpy(data.B + data.Pitch*i, frame->data[0] + frame->linesize[0]*i, frame->linesize[0]);
        break;

        default:
            return MFX_ERR_UNSUPPORTED;
    }
    pFrameAllocator->Unlock(pFrameAllocator->pthis, surface->Data.MemId, &data);
    surface->Data.TimeStamp = frame->pts;
    surface->Info.PicStruct = avframe_id_to_mfx_pic_struct(frame);

    return 0;
}

static int input_get_surface(AVFilterLink *inlink, int vppidx, AVFrame* picref, mfxFrameSurface1 **surface)
{
    int              in_idx = FF_INLINK_IDX(inlink);
    AVFilterContext *ctx    = inlink->dst;
    VPPContext      *vpp    = ctx->priv;
    int              surf_idx  = 0;
    VPPInterContext *inter_vpp = &vpp->inter_vpp[vppidx];
    int (*map_frame_to_surface)(VPPContext *, AVFrame *, mfxFrameSurface1 *);

    if (inter_vpp->pVppParam->IOPattern & MFX_IOPATTERN_IN_VIDEO_MEMORY) {
        /*
         * When video-mem mode, frame coming from main pad must be of PIXFMT_QSV.
         */
        if (in_idx == VPP_PAD_MAIN) {
            if (picref->data[3] != NULL) {
                *surface = (mfxFrameSurface1*)picref->data[3];
                (*surface)->Data.TimeStamp = picref->pts;
                return 0;
            } else
                return MFX_ERR_NOT_FOUND;
        } else
            /*
             * For sub pad, we copy data from frame to pre-allocated surface.
             */
            map_frame_to_surface = vidmem_map_frame_to_surface;
    } else
        map_frame_to_surface = sysmem_map_frame_to_surface;

    surf_idx = get_free_surface_index_in(ctx, inter_vpp->in_surface[in_idx],
            inter_vpp->num_surfaces_in[in_idx]);
    if (MFX_ERR_NOT_FOUND == surf_idx)
        return MFX_ERR_NOT_FOUND;

    *surface = &inter_vpp->in_surface[in_idx][surf_idx];

    return map_frame_to_surface(vpp, picref, *surface);
}

static int output_get_surface( AVFilterLink *inlink, int vppidx, AVFrame *frame, mfxFrameSurface1 **surface )
{
    AVFilterContext *ctx = inlink->dst;
    VPPContext      *vpp = ctx->priv;
    int              out_idx = 0;
    VPPInterContext *inter_vpp = &vpp->inter_vpp[vppidx];

    out_idx = get_free_surface_index_out(inter_vpp->out_surface,
            inter_vpp->sysmem_cur_out_idx, inter_vpp->num_surfaces_out);
    if (MFX_ERR_NOT_FOUND == out_idx)
        return MFX_ERR_NOT_FOUND;

    *surface = inter_vpp->out_surface[out_idx];

    if (inter_vpp->pVppParam->IOPattern & MFX_IOPATTERN_OUT_SYSTEM_MEMORY) {
        inter_vpp->sysmem_cur_out_idx = out_idx + 1;
        if (inter_vpp->sysmem_cur_out_idx >= inter_vpp->num_surfaces_out)
            inter_vpp->sysmem_cur_out_idx = 0;

        if (vppidx == (vpp->num_vpp - 1))
            sysmem_map_frame_to_surface(vpp, frame, *surface);
    } else {
        /*For video mem, we use AVFrame->data[3] to transfer surface*/
        frame->data[3] = (uint8_t *)(*surface);
    }

    return 0;
}

static int init_vpp_param(VPPContext *vpp, int format, int input_w, int input_h,
        int frame_rate_num, int frame_rate_den, int pic_struct )
{
    if (!(frame_rate_num*frame_rate_den)) {
        frame_rate_den = vpp->framerate.den;
        frame_rate_num = vpp->framerate.num;
    }

    if (vpp->framerate.num * frame_rate_den != vpp->framerate.den * frame_rate_num)
        vpp->use_frc = 1;
    else
        vpp->use_frc = 0;

    vpp->num_vpp = 1;

    /*
     * Init 1st qsvvpp, used as scaler & compositer.
     */
    vpp->inter_vpp[0].nb_inputs = 1;//vpp->ctx->nb_inputs;
    vpp->inter_vpp[0].pVppParam = &vpp->inter_vpp[0].VppParam;
    vpp->inter_vpp[0].pVppParam->IOPattern =
        vpp->pFrameAllocator ? MFX_IOPATTERN_IN_VIDEO_MEMORY :
        MFX_IOPATTERN_IN_SYSTEM_MEMORY;

    // input data
    INIT_FRAMEINFO(vpp->inter_vpp[0].pVppParam->vpp.In,
            format,
            input_w,
            input_h,
            pic_struct,
            frame_rate_num,
            frame_rate_den);

    // output data
    INIT_FRAMEINFO(vpp->inter_vpp[0].pVppParam->vpp.Out,
            AV_PIX_FMT_NV12,
            vpp->out_width,
            vpp->out_height,
            option_id_to_mfx_pic_struct(vpp->dpic),
            vpp->framerate.num,
            vpp->framerate.den);

    vpp->inter_vpp[0].pVppParam->NumExtParam = 0;
    vpp->inter_vpp[0].pVppParam->ExtParam = (mfxExtBuffer**)vpp->inter_vpp[0].pExtBuf;
    if (vpp->deinterlace) {
        av_log(vpp->ctx, AV_LOG_DEBUG, "Deinterlace enabled\n");
        memset(&vpp->deinterlace_conf, 0, sizeof(mfxExtVPPDeinterlacing));
        vpp->deinterlace_conf.Header.BufferId = MFX_EXTBUFF_VPP_DEINTERLACING;
        vpp->deinterlace_conf.Header.BufferSz = sizeof(mfxExtVPPDeinterlacing);
        vpp->deinterlace_conf.Mode              = vpp->deinterlace == 1 ?
            MFX_DEINTERLACING_BOB : MFX_DEINTERLACING_ADVANCED;

        vpp->inter_vpp[0].pVppParam->ExtParam[vpp->inter_vpp[0].pVppParam->NumExtParam++]
                = (mfxExtBuffer*)&(vpp->deinterlace_conf);
    }

    if (vpp->use_frc) {
        av_log(vpp->ctx, AV_LOG_DEBUG, "Framerate conversion enabled\n");
        memset(&vpp->frc_conf, 0, sizeof(mfxExtVPPFrameRateConversion));
        vpp->frc_conf.Header.BufferId = MFX_EXTBUFF_VPP_FRAME_RATE_CONVERSION;
        vpp->frc_conf.Header.BufferSz = sizeof(mfxExtVPPFrameRateConversion);
        vpp->frc_conf.Algorithm       = MFX_FRCALGM_DISTRIBUTED_TIMESTAMP; // make optional

        vpp->inter_vpp[0].pVppParam->ExtParam[vpp->inter_vpp[0].pVppParam->NumExtParam++]
                = (mfxExtBuffer*)&(vpp->frc_conf);
    }

    if (vpp->denoise) {
        av_log(vpp->ctx, AV_LOG_DEBUG, "Denoise enabled\n");
        memset(&vpp->denoise_conf, 0, sizeof(mfxExtVPPDenoise));
        vpp->denoise_conf.Header.BufferId = MFX_EXTBUFF_VPP_DENOISE;
        vpp->denoise_conf.Header.BufferSz = sizeof(mfxExtVPPDenoise);
        vpp->denoise_conf.DenoiseFactor   = vpp->denoise;

        vpp->inter_vpp[0].pVppParam->ExtParam[vpp->inter_vpp[0].pVppParam->NumExtParam++]
                = (mfxExtBuffer*)&(vpp->denoise_conf);
    }

    if (vpp->detail) {
        av_log(vpp->ctx, AV_LOG_DEBUG, "Detail enabled\n");
        memset(&vpp->detail_conf, 0, sizeof(mfxExtVPPDetail));
        vpp->detail_conf.Header.BufferId  = MFX_EXTBUFF_VPP_DETAIL;
        vpp->detail_conf.Header.BufferSz  = sizeof(mfxExtVPPDetail);
        vpp->detail_conf.DetailFactor      = vpp->detail;

        vpp->inter_vpp[0].pVppParam->ExtParam[vpp->inter_vpp[0].pVppParam->NumExtParam++]
                = (mfxExtBuffer*)&(vpp->detail_conf);
    }

    if (vpp->procamp) {
        av_log(vpp->ctx, AV_LOG_DEBUG, "ProcAmp enabled\n");
        memset(&vpp->procamp_conf, 0, sizeof(mfxExtVPPProcAmp));
        vpp->procamp_conf.Header.BufferId  = MFX_EXTBUFF_VPP_PROCAMP;
        vpp->procamp_conf.Header.BufferSz  = sizeof(mfxExtVPPProcAmp);
        vpp->procamp_conf.Hue              = vpp->hue;
        vpp->procamp_conf.Saturation       = vpp->saturation;
        vpp->procamp_conf.Contrast         = vpp->contrast;
        vpp->procamp_conf.Brightness       = vpp->brightness;

        vpp->inter_vpp[0].pVppParam->ExtParam[vpp->inter_vpp[0].pVppParam->NumExtParam++]
                = (mfxExtBuffer*)&(vpp->procamp_conf);
    }

    /*
+     * Init 2nd qsvvpp, used as frc/detail/deinterlace/denoise.
+     */
    if (vpp->use_composite) {
        vpp->num_vpp++;
        vpp->inter_vpp[1].nb_inputs = vpp->ctx->nb_inputs;
        vpp->inter_vpp[1].pVppParam = &vpp->inter_vpp[1].VppParam;
        /*
+         * 2 linked vpp will be used, so we use video memory to pass
+         * frames between them.
+         */
        vpp->inter_vpp[0].pVppParam->IOPattern |= MFX_IOPATTERN_OUT_VIDEO_MEMORY;
        vpp->inter_vpp[1].pVppParam->IOPattern =  MFX_IOPATTERN_IN_VIDEO_MEMORY;
        if(NULL != vpp->pFrameAllocator)
            vpp->inter_vpp[1].pVppParam->IOPattern |= MFX_IOPATTERN_OUT_VIDEO_MEMORY;
        else
            vpp->inter_vpp[1].pVppParam->IOPattern |= MFX_IOPATTERN_OUT_SYSTEM_MEMORY;

        INIT_FRAMEINFO(vpp->inter_vpp[1].pVppParam->vpp.In,
            AV_PIX_FMT_NV12,
            vpp->out_width,
            vpp->out_height,
            option_id_to_mfx_pic_struct(vpp->dpic),
            vpp->framerate.num,
            vpp->framerate.den);

        INIT_FRAMEINFO(vpp->inter_vpp[1].pVppParam->vpp.Out,
            AV_PIX_FMT_NV12,
            vpp->out_width,
            vpp->out_height,
            option_id_to_mfx_pic_struct(vpp->dpic),
            vpp->framerate.num,
            vpp->framerate.den);

        vpp->inter_vpp[1].pVppParam->NumExtParam = 0;
        vpp->inter_vpp[1].pVppParam->ExtParam = (mfxExtBuffer**)vpp->inter_vpp[1].pExtBuf;

        av_log(vpp->ctx, AV_LOG_INFO, "Composite enabled\n");
        memset(&vpp->composite_conf, 0, sizeof(vpp->composite_conf));
        vpp->composite_conf.Header.BufferId = MFX_EXTBUFF_VPP_COMPOSITE;
        vpp->composite_conf.Header.BufferSz = sizeof(vpp->composite_conf);
        vpp->composite_conf.R = 0;
        vpp->composite_conf.G = 0;
        vpp->composite_conf.B = 0;
        vpp->composite_conf.NumInputStream = vpp->inter_vpp[1].nb_inputs;
        vpp->composite_conf.InputStream    = vpp->layout;

        vpp->inter_vpp[1].pVppParam->ExtParam[vpp->inter_vpp[1].pVppParam->NumExtParam++]
                = (mfxExtBuffer*)&vpp->composite_conf;
    } else {
        if (NULL != vpp->pFrameAllocator)
            vpp->inter_vpp[0].pVppParam->IOPattern |=  MFX_IOPATTERN_OUT_VIDEO_MEMORY;
        else
            vpp->inter_vpp[0].pVppParam->IOPattern |=  MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
    }

    for (int vppidx = 0; vppidx < vpp->num_vpp; vppidx++)
        av_log(vpp->ctx, AV_LOG_INFO,
                "VPP[%d]: In %dx%d %4.2f fps\t Out %dx%d %4.2f fps\n",
                vppidx,
                (vpp->inter_vpp[vppidx].pVppParam->vpp.In.Width),
                (vpp->inter_vpp[vppidx].pVppParam->vpp.In.Height),
                vpp->inter_vpp[vppidx].pVppParam->vpp.In.FrameRateExtN
                 / (float)vpp->inter_vpp[vppidx].pVppParam->vpp.In.FrameRateExtD,
                (vpp->inter_vpp[vppidx].pVppParam->vpp.Out.Width),
                (vpp->inter_vpp[vppidx].pVppParam->vpp.Out.Height),
                vpp->inter_vpp[vppidx].pVppParam->vpp.Out.FrameRateExtN
                 / (float)vpp->inter_vpp[vppidx].pVppParam->vpp.Out.FrameRateExtD);

    return 0;
}

static int initial_vpp(VPPContext *vpp)
{
    int ret = 0;

    vpp->frame_number = 0;

    av_log(vpp->ctx, AV_LOG_INFO, "vpp configuration and call mfxVideoVPP_Init\n");
    if (!vpp->inter_vpp[0].session) {
        av_log(vpp->ctx, AV_LOG_DEBUG, "sysmem-vpp: GPUCopy %s.\n",
                vpp->inter_vpp[0].internal_qs.gpu_copy == MFX_GPUCOPY_ON ?
                "enabled" : "disabled");
        ret = ff_qsv_init_internal_session((AVCodecContext *)vpp->ctx,
                &vpp->inter_vpp[0].internal_qs);
        if (ret < 0)
            return ret;

        vpp->inter_vpp[0].session = vpp->inter_vpp[0].internal_qs.session;
    }
    av_log(vpp->ctx, AV_LOG_INFO, "vpp[0] initializing with session = %p\n",
        vpp->inter_vpp[0].session);

    if (vpp->num_vpp > 1) {
        ret = ff_qsv_clone_session(vpp->inter_vpp[0].session,
                &vpp->inter_vpp[1].session);
        if (ret < 0) {
            av_log(vpp->ctx, AV_LOG_ERROR, "clone session failed.\n");
            return ret;
        }

        av_log(vpp->ctx, AV_LOG_INFO, "vpp[1] initializing with session = %p\n",
            vpp->inter_vpp[1].session);
        if (vpp->pFrameAllocator)
            MFXVideoCORE_SetFrameAllocator(vpp->inter_vpp[1].session, vpp->pFrameAllocator);
        else {
            QSVContext *qsvctx = av_mallocz(sizeof(*qsvctx));
            vpp->inter_alloc.Alloc  = ff_qsv_frame_alloc;
            vpp->inter_alloc.Lock   = ff_qsv_frame_lock;
            vpp->inter_alloc.Unlock = ff_qsv_frame_unlock;
            vpp->inter_alloc.GetHDL = ff_qsv_frame_get_hdl;
            vpp->inter_alloc.Free   = ff_qsv_frame_free;
            vpp->inter_alloc.pthis  = qsvctx;
            qsvctx->internal_qs = vpp->inter_vpp[0].internal_qs;
            MFXVideoCORE_SetFrameAllocator(vpp->inter_vpp[0].session, &vpp->inter_alloc);
            MFXVideoCORE_SetFrameAllocator(vpp->inter_vpp[1].session, &vpp->inter_alloc);
        }
    }

    for (int vppidx = 0; vppidx < vpp->num_vpp; vppidx++) {
        memset(&vpp->inter_vpp[vppidx].req, 0, sizeof(mfxFrameAllocRequest) * 2);
        ret = MFXVideoVPP_QueryIOSurf(vpp->inter_vpp[vppidx].session,
                vpp->inter_vpp[vppidx].pVppParam, &vpp->inter_vpp[vppidx].req[0]);
        if (ret < 0) {
            av_log(vpp->ctx, AV_LOG_ERROR, "Error querying the VPP IO surface\n");
            return ff_qsv_error(ret);
        }

        ret = MFXVideoVPP_Init(vpp->inter_vpp[vppidx].session, vpp->inter_vpp[vppidx].pVppParam);
        if (MFX_WRN_PARTIAL_ACCELERATION == ret)
            av_log(vpp->ctx, AV_LOG_WARNING, "VPP will work with partial HW acceleration\n");
        else if (ret < 0) {
            av_log(vpp->ctx, AV_LOG_ERROR, "Error initializing the VPP[%d]\n", vppidx);
            return ff_qsv_error(ret);
        }
    }

    if (vpp->num_vpp > 1)
        vpp->inter_vpp[0].req[1].NumFrameSuggested += vpp->inter_vpp[1].req[0].NumFrameSuggested;

    vpp_init_surface(vpp);

    vpp->vpp_ready = 1;

    return 0;
}

int av_qsv_pipeline_config_vpp(AVCodecContext *dec_ctx, AVFilterContext *vpp_ctx, int frame_rate_num, int frame_rate_den)
{
    VPPContext *vpp = vpp_ctx->priv;

	av_log(vpp->ctx, AV_LOG_INFO, "vpp initializing with session = %p\n", vpp->inter_vpp[0].session);

    init_vpp_param(vpp, dec_ctx->pix_fmt, dec_ctx->width, dec_ctx->height,
		                frame_rate_num, frame_rate_den,
		                field_order_to_mfx_pic_struct(dec_ctx));
    return initial_vpp(vpp);
}

static int config_vpp(AVFilterLink *inlink, AVFrame * pic)
{
    AVFilterContext *ctx = inlink->dst;
    VPPContext      *vpp = ctx->priv;

    init_vpp_param(vpp, inlink->format, inlink->w, inlink->h,
		                inlink->frame_rate.num, inlink->frame_rate.den,
		                avframe_id_to_mfx_pic_struct(pic));

    return initial_vpp(vpp);
}

static void deconf_vpp(AVFilterContext *ctx)
{
    VPPContext *vpp = ctx->priv;

    for (int vppidx = vpp->num_vpp - 1; vppidx >= 0; vppidx--) {
        MFXVideoVPP_Close(vpp->inter_vpp[vppidx].session);
        if (vppidx > 0)
            MFXClose(vpp->inter_vpp[vppidx].session);
    }

    vpp_free_surface(vpp);
    if (vpp->inter_alloc.pthis)
        av_freep(&vpp->inter_alloc.pthis);
    ff_qsv_close_internal_session(&vpp->inter_vpp[0].internal_qs);

    vpp->vpp_ready = 0;
}

static int take_thumbnail(VPPContext *vpp, AVFrame *frame, const char *filename)
{
    int             ret   = 0;
    AVPacket        pkt;
    AVFrame         frame_out;
    int             got_frame = 0;

    memset(&frame_out, 0, sizeof(frame_out));
    av_init_packet(&pkt);

    if(!vpp->thm_swsctx || !vpp->thm_mux)
        goto failed;

    /*Open output file(JPEG) for mux.*/
    if(!(vpp->thm_mux->flags & AVFMT_NOFILE))
        if(avio_open(&vpp->thm_mux->pb, filename, AVIO_FLAG_WRITE) < 0)
            goto failed;

    /*Thus output file is opened, we write head info into it.*/
    ret = avformat_write_header(vpp->thm_mux, NULL);
    if(ret < 0)
        goto failed;

    /*Debug*/
    av_dump_format(vpp->thm_mux, 0, filename, 1);

    /*Change frame->pix_fmt from NV12 to YUVJ420P*/
    av_frame_copy_props(&frame_out, frame);
    frame_out.width  = frame->width;
    frame_out.height = frame->height;
    frame_out.format = AV_PIX_FMT_YUVJ420P;
    av_frame_get_buffer(&frame_out, 32);

    ret = sws_scale(vpp->thm_swsctx, (const uint8_t * const*)frame->data, frame->linesize, 0, 
        frame->height, frame_out.data, frame_out.linesize);
    if(ret < 0)
        goto failed;

    /*Ready to encode now.*/
    ret = avcodec_encode_video2(vpp->thm_enc, &pkt, &frame_out, &got_frame);
    if(ret < 0 || !got_frame)
        goto failed;

    /*Write the coded frame to output file "Interleavly"*/
    pkt.stream_index = vpp->thm_stream->index;
    ret = av_interleaved_write_frame(vpp->thm_mux, &pkt);
    if(ret < 0)
        goto failed;

    /*Do the end action.*/
    ret = av_write_trailer(vpp->thm_mux);

failed:
    av_frame_unref(&frame_out);
    av_frame_free(&frame);
    av_free_packet(&pkt);
    if(vpp->thm_mux)
        if(vpp->thm_mux->pb && !(vpp->thm_mux->flags & AVFMT_NOFILE))
            avio_close(vpp->thm_mux->pb);

    return ret;
}

static void *thumbnail_task(void *arg)
{
    VPPContext *vpp = arg;
    int         count = 0;
    AVFrame    *frame = NULL;
    int         ret = 0;
    char        filename[128];

    while(!vpp->task_exit){
        if(av_fifo_size(vpp->thm_framebuffer) > 0){
            ret = av_fifo_generic_read(vpp->thm_framebuffer, &frame, sizeof(frame), NULL);
            if(ret == 0){
                av_log(vpp->ctx, AV_LOG_INFO, "get a thumbnail frame, total = %d\n", count);
                snprintf(filename, sizeof(filename), vpp->thumbnail_file, count);
                take_thumbnail(vpp, frame, filename);
                count++;
            }
        }
        av_usleep(1000);
    }

    pthread_exit(NULL);

    return NULL;
}

/*
 * Real filter func.
 * Push frame into mSDK and pop out filtered frames.
 */
static int process_frame(AVFilterLink *inlink, int vppidx, AVFrame *picref)
{
    int               ret         = 0;
    int               filter_frame_ret = 0;
    AVFilterContext  *ctx         = inlink->dst;
    VPPContext       *vpp         = ctx->priv;
    mfxSyncPoint      sync        = NULL;
    mfxFrameSurface1 *pInSurface  = NULL;
    mfxFrameSurface1 *pOutSurface = NULL;
    AVFilterLink     *outlink     = inlink->dst->outputs[0];
    AVFrame          *out         = NULL;

    do {
        /*
         * get an AVFrame for output.
         * @NOTE: frame buffer is aligned with 128x64 to compat with GPU-copy.
         */
        out = ff_get_video_buffer(outlink, FFALIGN(vpp->out_width, 128), FFALIGN(vpp->out_height, 64));
        if (!out) {
            ret = MFX_ERR_MEMORY_ALLOC;
            break;
        }
        av_frame_copy_props(out, picref);
        out->width = vpp->out_width;
        out->height = vpp->out_height;
        out->interlaced_frame = vpp->dpic == 0 || vpp->dpic == 2;
        out->top_field_first  = vpp->dpic == 0;

        /*get input surface*/
        input_get_surface(inlink, vppidx, picref, &pInSurface);
        output_get_surface(inlink, vppidx, out, &pOutSurface);
        if (!pInSurface || !pOutSurface) {
            av_log(ctx, AV_LOG_ERROR, "no free input or output surface\n");
            av_frame_free(&out);
            ret = MFX_ERR_MEMORY_ALLOC;
            break;
        }

        do {
            ret = MFXVideoVPP_RunFrameVPPAsync(vpp->inter_vpp[vppidx].session,
                    pInSurface, pOutSurface, NULL, &sync);
            if (ret == MFX_WRN_DEVICE_BUSY) {
                av_usleep(500);
                continue;
            }
            break;
        } while (1);

        if (ret < 0 && ret != MFX_ERR_MORE_SURFACE) {
            av_frame_free(&out);
            /*Ignore more_data error*/
            if (ret == MFX_ERR_MORE_DATA)
                ret = 0;
            break;
        }

        if (ret == MFX_WRN_INCOMPATIBLE_VIDEO_PARAM)
            av_log(ctx, AV_LOG_WARNING,
                    "EncodeFrameAsync returned 'incompatible param' code\n");

        if (vpp->num_vpp == vppidx + 1) {
            MFXVideoCORE_SyncOperation(vpp->inter_vpp[vppidx].session, sync, 60000);
            /*
             * @NOTE: if composite is used, mSDK will not generate timestamp
             * for out-surfaces any more. We use source frame's pts instead.
             */
            if (vpp->inter_vpp[vppidx].nb_inputs > 1)
                out->pts = av_rescale_q(vpp->fs->pts, vpp->fs->time_base, outlink->time_base);
            else
                out->pts = av_rescale_q(pOutSurface->Data.TimeStamp,
                            (AVRational){1, 90000}, outlink->time_base);

            if ((vpp->thm_pendding || (vpp->use_thumbnail && (vpp->frame_number % vpp->thumb_interval == 0)))
                    && av_fifo_space(vpp->thm_framebuffer) > sizeof(AVFrame*)) {
                AVFrame *pframe = av_frame_clone(out);
                if(NULL != vpp->pFrameAllocator){
                    mfxFrameData data;
                    int i;
                    memset(&data, 0, sizeof(data));
                    vpp->pFrameAllocator->Lock(vpp->pFrameAllocator->pthis, pOutSurface->Data.MemId, &data);
                    /*Copy Y, NOTE that pframe->linesize != data.Pitch when video memory*/
                    for(i=0; i<pframe->height; i++)
                        memcpy(pframe->data[0] + pframe->linesize[0]*i, data.Y + data.Pitch*i, pframe->linesize[0]);
                    /*Copy UV, NOTE that pframe->linesize != data.Pitch when video memory*/
                    for(i=0; i<pframe->height/2; i++)
                        memcpy(pframe->data[1] + pframe->linesize[1]*i, data.UV + data.Pitch*i, pframe->linesize[1]);
                    vpp->pFrameAllocator->Unlock(vpp->pFrameAllocator->pthis, pOutSurface->Data.MemId, &data);
                }
                av_fifo_generic_write(vpp->thm_framebuffer, &pframe, sizeof(pframe), NULL);
                vpp->thm_pendding = 0;
            }

            filter_frame_ret = ff_filter_frame(outlink, out);
            if (filter_frame_ret < 0)
                break;

            vpp->frame_number++;
        } else {
            out->pts = pOutSurface->Data.TimeStamp;
            filter_frame_ret = ff_framesync_filter_frame(vpp->fs, inlink, out);
            if (filter_frame_ret < 0)
                break;
        }
    } while(ret == MFX_ERR_MORE_SURFACE);

    return ret < 0 ? ff_qsv_error(ret) : filter_frame_ret;
}

/*
 * Callback from framesync.
 * Framesync will "on_event" and call this function once a new frame is pushed to fs
 * via main input.
 */
static int fs_process_frame(FFFrameSync *fs)
{
    AVFilterContext *ctx = fs->parent;
    VPPContext      *vpp = fs->opaque;
    AVFrame         *pic = NULL;
    int              ret = 0;

    /*
     * Request frames from each input of fs.
     * @NOTE: We take main pad's frames' pts as the current pts.
     */
    for (int i = VPP_PAD_MAIN; i < ctx->nb_inputs; i++) {
        if ((ret = ff_framesync_get_frame(vpp->fs, i, &pic, 0)) < 0)
            break;
        if ((ret = process_frame(ctx->inputs[i], 1, pic)) != 0)
            break;
    }
    /*
     * Once some frames are filtered with error, we drop this cycle.
     */
    if (ret != 0)
        ff_framesync_drop(vpp->fs);

    return ret;
}

static int eval_expr(AVFilterContext *ctx)
{
#define PASS_EXPR(e, s) {\
    ret = av_expr_parse(&e, s, var_names, NULL, NULL, NULL, NULL, 0, ctx); \
    if (ret < 0) {\
        av_log(ctx, AV_LOG_ERROR, "Error when passing '%s'.\n", s);\
        return ret;\
    }\
}
#define CALC_EXPR(e, v, i) {\
    i = v = av_expr_eval(e, var_values, NULL); \
}
    VPPContext *vpp = ctx->priv;
    double  var_values[VAR_VARS_NB] = { NAN };
    AVExpr *w_expr = NULL, *h_expr = NULL;
#if VPP_FLEX_MAIN
    AVExpr *mx_expr = NULL, *my_expr = NULL;
    AVExpr *mw_expr = NULL, *mh_expr = NULL;
#endif
    AVExpr *ox_expr = NULL, *oy_expr = NULL;
    AVExpr *ow_expr = NULL, *oh_expr = NULL;
    int     ret = 0;

    /*
     * Pass expressions into AVExpr
     */
    PASS_EXPR(w_expr, vpp->ow);
    PASS_EXPR(h_expr, vpp->oh);
#if VPP_FLEX_MAIN
    PASS_EXPR(mx_expr, vpp->main_ox);
    PASS_EXPR(my_expr, vpp->main_oy);
    PASS_EXPR(mw_expr, vpp->main_ow);
    PASS_EXPR(mh_expr, vpp->main_oh);
#endif
    PASS_EXPR(ox_expr, vpp->overlay_ox);
    PASS_EXPR(oy_expr, vpp->overlay_oy);
    PASS_EXPR(ow_expr, vpp->overlay_ow);
    PASS_EXPR(oh_expr, vpp->overlay_oh);

    /*
     * Fill constant values.
     * iW and iH are fixed values.
     */
    var_values[VAR_iW] =
    var_values[VAR_MAIN_iW] =
    var_values[VAR_IN_W] = ctx->inputs[VPP_PAD_MAIN]->w;

    var_values[VAR_iH] =
    var_values[VAR_MAIN_iH] =
    var_values[VAR_IN_H] = ctx->inputs[VPP_PAD_MAIN]->h;

    if (ctx->nb_inputs > 1) {
        var_values[VAR_OVERLAY_iW] = ctx->inputs[VPP_PAD_OVERLAY]->w;
        var_values[VAR_OVERLAY_iH] = ctx->inputs[VPP_PAD_OVERLAY]->h;
    } else {
        var_values[VAR_OVERLAY_iW] = NAN;
        var_values[VAR_OVERLAY_iH] = NAN;
    }

    /*
     * Calc the user-defined values.
     */
    CALC_EXPR(w_expr,
            var_values[VAR_MAIN_W] = var_values[VAR_OUT_W] = var_values[VAR_oW] = var_values[VAR_W],
            vpp->out_width);
    CALC_EXPR(h_expr,
            var_values[VAR_MAIN_H] = var_values[VAR_OUT_H] = var_values[VAR_oH] = var_values[VAR_H],
            vpp->out_height);
    ///calc again in case ow is relative to oh
    CALC_EXPR(w_expr,
            var_values[VAR_MAIN_W] = var_values[VAR_OUT_W] = var_values[VAR_oW] = var_values[VAR_W],
            vpp->out_width);

#if VPP_FLEX_MAIN
    CALC_EXPR(mw_expr, var_values[VAR_MAIN_oW], vpp->layout[VPP_PAD_MAIN].DstW);
    CALC_EXPR(mh_expr, var_values[VAR_MAIN_oH], vpp->layout[VPP_PAD_MAIN].DstH);
    CALC_EXPR(mw_expr, var_values[VAR_MAIN_oW], vpp->layout[VPP_PAD_MAIN].DstW);

    CALC_EXPR(mx_expr, var_values[VAR_MAIN_oX], vpp->layout[VPP_PAD_MAIN].DstX);
    CALC_EXPR(my_expr, var_values[VAR_MAIN_oY], vpp->layout[VPP_PAD_MAIN].DstY);
    CALC_EXPR(mx_expr, var_values[VAR_MAIN_oX], vpp->layout[VPP_PAD_MAIN].DstX);

    CALC_EXPR(mw_expr, var_values[VAR_MAIN_oW], vpp->layout[VPP_PAD_MAIN].DstW);
    CALC_EXPR(mh_expr, var_values[VAR_MAIN_oH], vpp->layout[VPP_PAD_MAIN].DstH);
    CALC_EXPR(mw_expr, var_values[VAR_MAIN_oW], vpp->layout[VPP_PAD_MAIN].DstW);
#endif

    CALC_EXPR(ow_expr, var_values[VAR_OVERLAY_W], vpp->layout[VPP_PAD_OVERLAY].DstW);
    CALC_EXPR(oh_expr, var_values[VAR_OVERLAY_H], vpp->layout[VPP_PAD_OVERLAY].DstH);
    ///calc again in case ow is relative to oh
    CALC_EXPR(ow_expr, var_values[VAR_OVERLAY_W], vpp->layout[VPP_PAD_OVERLAY].DstW);

    CALC_EXPR(ox_expr,
            var_values[VAR_OVERLAY_X] = var_values[VAR_X],
            vpp->layout[VPP_PAD_OVERLAY].DstX);
    CALC_EXPR(oy_expr,
            var_values[VAR_OVERLAY_Y] = var_values[VAR_Y],
            vpp->layout[VPP_PAD_OVERLAY].DstY);
    ///calc again in case ox is relative to oy
    CALC_EXPR(ox_expr,
            var_values[VAR_OVERLAY_X] = var_values[VAR_X],
            vpp->layout[VPP_PAD_OVERLAY].DstX);

    CALC_EXPR(ow_expr, var_values[VAR_OVERLAY_W], vpp->layout[VPP_PAD_OVERLAY].DstW);
    CALC_EXPR(oh_expr, var_values[VAR_OVERLAY_H], vpp->layout[VPP_PAD_OVERLAY].DstH);
    ///calc again in case ow is relative to oh
    CALC_EXPR(ow_expr, var_values[VAR_OVERLAY_W], vpp->layout[VPP_PAD_OVERLAY].DstW);

    av_expr_free(w_expr);
    av_expr_free(h_expr);
#if VPP_FLEX_MAIN
    av_expr_free(mx_expr);
    av_expr_free(my_expr);
    av_expr_free(mw_expr);
    av_expr_free(mh_expr);
#endif
    av_expr_free(ox_expr);
    av_expr_free(oy_expr);
    av_expr_free(ow_expr);
    av_expr_free(oh_expr);

#undef PASS_EXPR
#undef CALC_EXPR

    return ret;
}

/*
 * Configure each inputs.
 */
static int config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    VPPContext      *vpp = ctx->priv;
    int              idx = FF_INLINK_IDX(inlink);

    av_log(ctx, AV_LOG_DEBUG, "Input[%d]'s format is %s, size %dx%d\n", idx,
            av_get_pix_fmt_name(inlink->format), inlink->w, inlink->h);

    if (vpp->layout[idx].GlobalAlpha < 255)
        vpp->layout[idx].GlobalAlphaEnable = 1;

    if (vpp->layout[idx].PixelAlphaEnable) {
        av_log(ctx, AV_LOG_DEBUG, "enable per-pixel alpha for %s\n", inlink->dstpad->name);
        if (vpp->layout[idx].GlobalAlphaEnable)
            vpp->layout[idx].GlobalAlphaEnable = 0;
    }

#if !VPP_FLEX_MAIN
    if (idx == VPP_PAD_MAIN) {
        vpp->layout[idx].GlobalAlphaEnable = 0;
        vpp->layout[idx].PixelAlphaEnable  = 0;
    }
#endif

    return 0;
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    VPPContext      *vpp = ctx->priv;
    int              idx = 0;
    AVFilterLink    *main_in = ctx->inputs[VPP_PAD_MAIN];
    int              ret = 0;
    AVCodec         *codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);

    ret = eval_expr(ctx);
    if (ret != 0)
        return ret;

    /*
     * Take main input's framerate as default.
     */
    if (vpp->framerate.den == 0 || vpp->framerate.num == 0)
        vpp->framerate = main_in->frame_rate;

    /*
     * if out_w is not set(<=0), we calc it based on out_h;
     * if out_h is not set(<=0), we calc it based on out_w;
     * if both are not set, we set out_rect = in_rect.
     */
    if (vpp->out_width <= 0)
        vpp->out_width  = av_rescale(vpp->out_height, main_in->w, main_in->h);
    if (vpp->out_height <= 0)
        vpp->out_height = av_rescale(vpp->out_width, main_in->h, main_in->w);
    if (vpp->out_height <= 0 || vpp->out_width <= 0) {
        vpp->out_width  = main_in->w;
        vpp->out_height = main_in->h;
    }

    /*
     * Check if any pad's layout is out of range.
     */
    for (idx = 0; idx < ctx->nb_inputs; idx++) {
        /*
         * By default, when multi-input, overlay_out_rect = overlay_in_rect
         */
        if (!vpp->layout[idx].DstW || !vpp->layout[idx].DstH) {
            vpp->layout[idx].DstW =
                    FFMIN(ctx->inputs[idx]->w, vpp->out_width - vpp->layout[idx].DstX);
            vpp->layout[idx].DstH =
                    FFMIN(ctx->inputs[idx]->h, vpp->out_height - vpp->layout[idx].DstY);
        }

        if ((vpp->layout[idx].DstW > vpp->out_width) ||
            (vpp->layout[idx].DstH > vpp->out_height) ||
            (vpp->layout[idx].DstX + vpp->layout[idx].DstW > vpp->out_width) ||
            (vpp->layout[idx].DstY + vpp->layout[idx].DstH > vpp->out_height)) {
            av_log(ctx, AV_LOG_ERROR, "Rect[%s] beyonds the output rect.\n",
                ctx->input_pads[idx].name);
            return AVERROR(EINVAL);
        }
    }

    outlink->w             = vpp->out_width;
    outlink->h             = vpp->out_height;
    outlink->frame_rate    = vpp->framerate;
    outlink->time_base     = av_inv_q(vpp->framerate);
    outlink->format        = AV_PIX_FMT_NV12;

    vpp->thm_swsctx = sws_getContext(outlink->w, outlink->h, outlink->format,
            outlink->w, outlink->h, AV_PIX_FMT_YUVJ420P,
            SWS_BICUBIC, NULL, NULL, NULL);
    if (NULL == vpp->thm_swsctx) {
        av_log(ctx, AV_LOG_WARNING, "Swscale init failed.\n");
        return 0;
    }

    ret = avformat_alloc_output_context2(&vpp->thm_mux, NULL, "mjpeg", NULL);
    if (ret < 0) {
        av_log(ctx, AV_LOG_WARNING, "mux init failed with %s.\n", av_err2str(ret));
        return 0;
    }

    vpp->thm_stream        = avformat_new_stream(vpp->thm_mux, codec);
    vpp->thm_enc           = vpp->thm_stream->codec;
    vpp->thm_enc->width    = outlink->w;
    vpp->thm_enc->height   = outlink->h;
    vpp->thm_enc->pix_fmt  = AV_PIX_FMT_YUVJ420P;
    vpp->thm_stream->time_base = vpp->thm_enc->time_base = av_inv_q(vpp->framerate);
    ret = avcodec_open2(vpp->thm_enc, vpp->thm_enc->codec, NULL);
    if (ret < 0) {
        av_log(ctx, AV_LOG_WARNING, "Thumbnail encoder init failed.\n");
        return 0;
    }

    return ret;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *picref)
{
    AVFilterContext *ctx = inlink->dst;
    VPPContext      *vpp = ctx->priv;
    int              ret;
    int              link_idx = FF_INLINK_IDX(inlink);

    av_log(ctx, AV_LOG_DEBUG, "Filtering frame from %s, count %"PRIi64", ts %s\n",
            inlink->src->name, inlink->frame_count, av_ts2str(picref->pts));

    /*
     * we re-config local params when getting 1st main frame.
     */
    if(!vpp->vpp_ready && link_idx == VPP_PAD_MAIN){
        ret = config_vpp(inlink, picref);
        if(ret < 0){
            av_frame_free(&picref);
            return ret;
        }
        vpp->vpp_ready = 1;
    }

    picref->pts = av_rescale_q(picref->pts, inlink->time_base, (AVRational){1, 90000});
    if (link_idx != VPP_PAD_MAIN)
        ret = ff_framesync_filter_frame(vpp->fs, inlink, picref);
    else {
        if (vpp->use_frc) {
            if (vpp->first_pts == AV_NOPTS_VALUE)
                vpp->first_pts = picref->pts;
            else {
                int delta = av_rescale_q(picref->pts - vpp->first_pts,
                        (AVRational){1, 90000}, av_inv_q(vpp->framerate)) - vpp->frame_number;
                if (delta < 0){
                    av_frame_free(&picref);
                    return 0;
                }
            }
        }
        ret = process_frame(inlink, 0, picref);
        av_frame_free(&picref);
    }

    return ret;
}

static int request_frame(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    VPPContext *vpp = ctx->priv;

    if (vpp->fs)
        return ff_framesync_request_frame(vpp->fs, outlink);

    return ff_request_frame(ctx->inputs[0]);
}

static int query_formats(AVFilterContext *ctx)
{
    VPPContext *vpp = ctx->priv;

    static const enum AVPixelFormat main_in_fmts[] = {
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_NV12,
        AV_PIX_FMT_YUYV422,
        AV_PIX_FMT_RGB32,
        AV_PIX_FMT_QSV,
        AV_PIX_FMT_NONE
    };
    static const enum AVPixelFormat overlay_in_fmts[] = {
        AV_PIX_FMT_RGB32,
        AV_PIX_FMT_NONE
    };
    static const enum AVPixelFormat out_pix_fmts[] = {
        AV_PIX_FMT_NV12,
        AV_PIX_FMT_QSV,
        AV_PIX_FMT_NONE
    };

    /*
     * If per-pixel alpha is enabled, we consider this input
     * is of RGB32 format.
     */
    for(int idx=0; idx<ctx->nb_inputs; idx++)
        if(vpp->layout[idx].PixelAlphaEnable)
            ff_formats_ref(ff_make_format_list(overlay_in_fmts),
                    &ctx->inputs[1]->out_formats);
        else
            ff_formats_ref(ff_make_format_list(main_in_fmts),
                    &ctx->inputs[idx]->out_formats);

    ff_formats_ref(ff_make_format_list(out_pix_fmts), &ctx->outputs[0]->in_formats);

    return 0;
}

static av_cold int vpp_init(AVFilterContext *ctx)
{
    VPPContext    *vpp = ctx->priv;
    AVFilterPad    pad = { 0 };
    FFFrameSyncIn *in  = NULL;
    int            nb_inputs = 1;

    vpp->frame_number    = 0;
    vpp->pFrameAllocator = NULL;
    vpp->vpp_ready       = 0;
    vpp->ctx             = ctx;
    vpp->thm_framebuffer = av_fifo_alloc(sizeof(AVFrame*)*8);
    vpp->task_exit       = 0;
    vpp->thm_pendding    = 0;
    vpp->first_pts        = AV_NOPTS_VALUE;
    if(!vpp->thumbnail_file)
        vpp->thumbnail_file = av_strdup("thumbnail-%d.jpg");
    pthread_create(&vpp->thumbnail_task, NULL, thumbnail_task, ctx->priv);

    /*
     * If use composite, there must be a sub-input.
     */
    if (vpp->use_composite) {
        nb_inputs ++;

        /*
         * Config framesync. Frames from front filters will be inserted into fs first.
         * Be careful that, vpp->fs must be allocated dynamically!!!
         * See details @framesync->in[0].
         * Set fs->time_base to 1/90K as mSDK requested.
         */
        vpp->fs = av_mallocz(sizeof(*vpp->fs) + sizeof(vpp->fs->in[0]) * (nb_inputs - 1));
        vpp->fs->on_event  = fs_process_frame;
        vpp->fs->opaque    = vpp;
        vpp->fs->time_base = (AVRational){1, 90000};
        ff_framesync_init(vpp->fs, ctx, nb_inputs);

        in = vpp->fs->in;
        in[VPP_PAD_MAIN].before = EXT_STOP;
        in[VPP_PAD_MAIN].after  = EXT_STOP;
        in[VPP_PAD_MAIN].sync   = 2;
        in[VPP_PAD_MAIN].time_base = (AVRational){1, 90000};

        for (int i = VPP_PAD_OVERLAY; i < nb_inputs; i++) {
            /*Insert overlay input pads*/
            pad.type         = AVMEDIA_TYPE_VIDEO;
            pad.name         = av_asprintf("overlay_%d", i);
            if (!pad.name)
                return AVERROR(ENOMEM);
            pad.filter_frame = filter_frame;
            pad.config_props = config_input;
            ff_insert_inpad(ctx, i, &pad);

            /*Init fs->in params*/
            in[i].sync       = 0;
            in[i].before     = EXT_NULL;
            if (vpp->eof_action == EOF_ACTION_ENDALL) {
                in[i].after  = EXT_STOP;
            } else if (vpp->eof_action == EOF_ACTION_REPEAT) {
                in[i].after  = EXT_INFINITY;
                in[i].sync   = 1;
            } else
                in[i].after  = EXT_NULL;
            in[i].time_base = (AVRational){1, 90000};
        }

        return ff_framesync_configure(vpp->fs);
    }

    return 0;
}

static av_cold void vpp_uninit(AVFilterContext *ctx)
{
    VPPContext *vpp    = ctx->priv;
    AVFrame    *pframe = NULL;
    int         ret    = 0;

    vpp->task_exit     = 1;
    pthread_join(vpp->thumbnail_task, NULL);

    if(vpp->thm_enc)
        avcodec_close(vpp->thm_enc);

    if(vpp->thm_mux)
        avformat_free_context(vpp->thm_mux);

    if(vpp->thm_swsctx)
        sws_freeContext(vpp->thm_swsctx);

    if(vpp->thm_framebuffer){
        while(av_fifo_size(vpp->thm_framebuffer) > 0){
            ret = av_fifo_generic_read(vpp->thm_framebuffer, &pframe, sizeof(pframe), NULL);
            if(ret < 0)
                break;
            av_frame_free(&pframe);
        }
        av_fifo_freep(&vpp->thm_framebuffer);
    }

    deconf_vpp(ctx);

    for (int i=VPP_PAD_OVERLAY; i<ctx->nb_inputs; i++)
        av_freep(&ctx->input_pads[i].name);

    if (vpp->fs) {
        ff_framesync_uninit(vpp->fs);
        av_freep(&vpp->fs);
    }
}

static int vpp_cmd_thumbnail(AVFilterContext *ctx, const char *arg)
{
    VPPContext *vpp    = ctx->priv;
    vpp->thm_pendding = 1;
    return 0;
}

static int vpp_cmd_size(AVFilterContext *ctx, const char *arg)
{
    VPPContext *vpp    = ctx->priv;
    int         w,h,ret;

    ret = av_parse_video_size(&w, &h, arg);
    if(ret != 0)
        return ret;

    if(w != vpp->out_width || h != vpp->out_height){
        if(vpp->vpp_ready)
            deconf_vpp(ctx);
        vpp->out_width = w;
        vpp->out_height = h;
    }

    return ret;
}

static int vpp_process_cmd(AVFilterContext *ctx, const char *cmd, const char *arg, char *res, int res_len, int flags)
{
    int ret = 0, i;

#undef NELEMS
#define NELEMS(_x_) (sizeof(_x_)/sizeof((_x_)[0]))
    static const struct{
        const char *short_name;
        const char *long_name;
        const char *desc;
        int (*func)(AVFilterContext*, const char *);
        int need_arg;
        const char *arg_desc;
    }cmdlist[] = {
        {"h", "help",        "Show this help.",   NULL,              0, NULL},
        {"p", "printscreen", "Take a thumbnail",  vpp_cmd_thumbnail, 0, NULL},
        {"s", "size",        "Output resolution", vpp_cmd_size,      1, "wxh"},
    };

    for(i = 0; i < NELEMS(cmdlist); i++){
        if(!av_strcasecmp(cmd, cmdlist[i].long_name)
            || !av_strcasecmp(cmd, cmdlist[i].short_name))
            break;
    }

    if((i > NELEMS(cmdlist)) || (i <= 0) || (cmdlist[i].need_arg && !arg)){
        for(i = 0; i < NELEMS(cmdlist); i++)
            av_log(ctx, AV_LOG_INFO, "%2s|%-12s %12s\t%s\n", cmdlist[i].short_name,
                cmdlist[i].long_name, cmdlist[i].desc, cmdlist[i].arg_desc);

        return AVERROR(EINVAL);
    }

    if(cmdlist[i].func)
        ret = cmdlist[i].func(ctx, arg);
    av_log(ctx, AV_LOG_DEBUG, "Dealing with cmd: %s, args: %s, ret: %d.\n", cmd, arg, ret);

    return ret;
}

static const AVFilterPad vpp_inputs[] = {
    {
        .name          = "main",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_input,
        .filter_frame  = filter_frame,
    },
    { NULL }
};

static const AVFilterPad vpp_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_output,
        .request_frame = request_frame,
    },
    { NULL }
};

AVFilter ff_vf_vpp = {
    .name          = "vpp",
    .description   = NULL_IF_CONFIG_SMALL("Quick Sync Video VPP."),
    .priv_size     = sizeof(VPPContext),
    .query_formats = query_formats,
    .init          = vpp_init,
    .uninit        = vpp_uninit,
    .inputs        = vpp_inputs,
    .outputs       = vpp_outputs,
    .priv_class    = &vpp_class,
    .process_command = vpp_process_cmd,
    .flags         = AVFILTER_FLAG_DYNAMIC_INPUTS,
};
