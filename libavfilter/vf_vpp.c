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
#include "libavcodec/qsv.h"

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

// number of video enhancement filters (denoise, procamp, detail, video_analysis, image stab)
//#define ENH_FILTERS_COUNT           5

#define OFFSET(x) offsetof(VPPContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM

static const AVOption vpp_options[] = {
    { "deinterlace", "deinterlace mode: 0=off, 1=bob, 2=advanced",             OFFSET(deinterlace),  AV_OPT_TYPE_INT, {.i64=0}, 0, MFX_DEINTERLACING_ADVANCED, .flags = FLAGS },
    { "denoise",     "denoise level [0, 100]",                                 OFFSET(denoise),      AV_OPT_TYPE_INT, {.i64=0}, 0, 100, .flags = FLAGS },
    { "detail",      "detail enhancement level [0, 100]",                      OFFSET(detail),       AV_OPT_TYPE_INT, {.i64=0}, 0, 100, .flags = FLAGS },
    { "w",           "Output video width",                                     OFFSET(out_width),    AV_OPT_TYPE_INT, {.i64=0}, 0, 4096, .flags = FLAGS },
    { "width",       "Output video width",                                     OFFSET(out_width),    AV_OPT_TYPE_INT, {.i64=0}, 0, 4096, .flags = FLAGS },
    { "h",           "Output video height",                                    OFFSET(out_height),   AV_OPT_TYPE_INT, {.i64=0}, 0, 2304, .flags = FLAGS },
    { "height",      "Output video height : ",                                 OFFSET(out_height),   AV_OPT_TYPE_INT, {.i64=0}, 0, 2304, .flags = FLAGS },
    { "dpic",        "dest pic struct: 0=tff, 1=progressive [default], 2=bff", OFFSET(dpic),         AV_OPT_TYPE_INT, {.i64 = 1 }, 0, 2, .flags = FLAGS },
    { "framerate",   "output framerate",                                       OFFSET(framerate),    AV_OPT_TYPE_RATIONAL, { .dbl = 0.0 },0, DBL_MAX, .flags = FLAGS },
    { "async_depth", "Maximum processing parallelism [default = 4]",           OFFSET(async_depth),  AV_OPT_TYPE_INT, { .i64 = ASYNC_DEPTH_DEFAULT }, 0, INT_MAX, .flags = FLAGS },
    { "max_b_frames","Maximum number of b frames  [default = 3]",              OFFSET(max_b_frames), AV_OPT_TYPE_INT, { .i64 = 3 }, 0, INT_MAX, .flags = FLAGS },
    { "gpu_copy", "Enable gpu copy in sysmem mode [default = off]", OFFSET(internal_qs.gpu_copy), AV_OPT_TYPE_INT, { .i64 = MFX_GPUCOPY_OFF }, MFX_GPUCOPY_DEFAULT, MFX_GPUCOPY_OFF, .flags = FLAGS, "gpu_copy" },
    { "default", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_GPUCOPY_DEFAULT }, MFX_GPUCOPY_DEFAULT, MFX_GPUCOPY_OFF, .flags = FLAGS, "gpu_copy" },
    { "on", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_GPUCOPY_ON }, MFX_GPUCOPY_DEFAULT, MFX_GPUCOPY_OFF, .flags = FLAGS, "gpu_copy" },
    { "off", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_GPUCOPY_OFF }, MFX_GPUCOPY_DEFAULT, MFX_GPUCOPY_OFF, .flags = FLAGS, "gpu_copy" },
    { "thumbnail",   "Enable automatic thumbnail",                             OFFSET(use_thumbnail), AV_OPT_TYPE_INT, {.i64 = 0}, 0, 1, .flags = FLAGS},
    { "thumb_interval","Thumbnail interval in frame",                          OFFSET(thumb_interval), AV_OPT_TYPE_INT, {.i64 = INT_MAX}, 1, INT_MAX, .flags = FLAGS},
    { "thumb_file",  "Thumbnail filename [default = thumbnail-%d.jpg]",        OFFSET(thumbnail_file), AV_OPT_TYPE_STRING, {.str = NULL}, 1, 128, .flags = FLAGS},
    { NULL }
};

AVFILTER_DEFINE_CLASS(vpp);

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

static void vidmem_init_surface( VPPContext *vpp)
{
	av_log(vpp->ctx, AV_LOG_INFO, "vpp: vidmem_init_surface:");

	if( NULL == vpp->pFrameAllocator)
        return;

	if( NULL != vpp->enc_ctx ){
        vpp->req[1].NumFrameSuggested += vpp->enc_ctx->req.NumFrameSuggested;
	    av_log(vpp->ctx, AV_LOG_INFO, " num = %d, enc_ctx.num=%d \n", vpp->req[1].NumFrameSuggested, vpp->enc_ctx->req.NumFrameSuggested );
	}
    av_log(vpp->ctx, AV_LOG_INFO, "vpp: in.num = %d, out.num = %d\n", vpp->req[0].NumFrameSuggested, vpp->req[1].NumFrameSuggested);

    vpp->num_surfaces_out = FFMAX(vpp->req[1].NumFrameSuggested, 1);
    vpp->out_response     = av_mallocz(sizeof(*vpp->out_response));
    VPP_CHECK_POINTER(vpp->out_response);
    vpp->pFrameAllocator->Alloc( vpp->pFrameAllocator->pthis, &(vpp->req[1]), vpp->out_response);
	vpp->out_surface      = av_mallocz(sizeof(*vpp->out_surface) * vpp->num_surfaces_out);
    VPP_CHECK_POINTER(vpp->out_surface);
    for (int i = 0; i < vpp->num_surfaces_out; i++) {
        vpp->out_surface[i] = av_mallocz(sizeof(*vpp->out_surface[i]));
        VPP_CHECK_POINTER(vpp->out_surface[i]);
        memcpy(&(vpp->out_surface[i]->Info), &(vpp->pVppParam->vpp.Out), sizeof(vpp->out_surface[i]->Info));
        vpp->out_surface[i]->Data.MemId = vpp->out_response->mids[i];
    }
}

static void vidmem_free_surface(AVFilterContext *ctx)
{
    VPPContext *vpp= ctx->priv;

	av_log(vpp->ctx, AV_LOG_DEBUG, "vpp: vidmem_free_surface\n");
    if(NULL != vpp->out_surface){
        for (unsigned int i = 0; i < vpp->num_surfaces_out; i++)
            av_freep(&vpp->out_surface[i]);
        av_freep(&vpp->out_surface);
    }

	if(NULL != vpp->out_response){
		vpp->pFrameAllocator->Free(vpp->pFrameAllocator->pthis, vpp->out_response);
        av_freep(&vpp->out_response);
    }

    vpp->num_surfaces_in  = 0;
    vpp->num_surfaces_out = 0;
}

static void sysmem_init_surface(VPPContext *vpp)
{
	av_log(vpp->ctx, AV_LOG_INFO, "vpp: sysmem_init_surface\n");
    av_log(vpp->ctx, AV_LOG_INFO, "vpp: in.num = %d, out.num = %d\n", 
            vpp->req[0].NumFrameSuggested, vpp->req[1].NumFrameSuggested);
    vpp->num_surfaces_in  = FFMAX(vpp->req[0].NumFrameSuggested, vpp->async_depth + vpp->max_b_frames + 1);
    vpp->num_surfaces_out = FFMAX(vpp->req[1].NumFrameSuggested, 1);
    vpp->in_surface = av_mallocz(sizeof(*vpp->in_surface) * vpp->num_surfaces_in);
    VPP_CHECK_POINTER(vpp->in_surface);
    for (int i = 0; i < vpp->num_surfaces_in; i++) {
        vpp->in_surface[i] = av_mallocz(sizeof(*vpp->in_surface[i]));
        VPP_CHECK_POINTER(vpp->in_surface[i]);
        memcpy(&(vpp->in_surface[i]->Info), &(vpp->pVppParam->vpp.In), sizeof(vpp->in_surface[i]->Info));
    }

    vpp->out_surface = av_mallocz(sizeof(*vpp->out_surface) * vpp->num_surfaces_out);
    VPP_CHECK_POINTER(vpp->out_surface);
    for (int i = 0; i < vpp->num_surfaces_out; i++) {
        vpp->out_surface[i] = av_mallocz(sizeof(*vpp->out_surface[i]));
        VPP_CHECK_POINTER(vpp->out_surface[i]);
        memcpy(&(vpp->out_surface[i]->Info), &(vpp->pVppParam->vpp.Out), sizeof(vpp->out_surface[i]->Info));
    }
}

static void sysmem_free_surface(AVFilterContext *ctx)
{
    VPPContext *vpp= ctx->priv;

	av_log(vpp->ctx, AV_LOG_DEBUG, "vpp: sysmem_free_surface\n");
    if(NULL != vpp->in_surface){
        for (unsigned int i = 0; i < vpp->num_surfaces_in; i++)
           av_freep(&vpp->in_surface[i]);
        av_freep(&vpp->in_surface);
    }

    if(NULL != vpp->out_surface){
        for (unsigned int i = 0; i < vpp->num_surfaces_out; i++)
            av_freep(&vpp->out_surface[i]);
        av_freep(&vpp->out_surface);
    }

    vpp->num_surfaces_in  = 0;
    vpp->num_surfaces_out = 0;
}

static int get_free_surface_index_in(AVFilterContext *ctx, mfxFrameSurface1 ** surface_pool, int pool_size)
{
    if (surface_pool) {
        for (mfxU16 i = 0; i < pool_size; i++) {
            if (0 == surface_pool[i]->Data.Locked)
                return i;
        }
    }

    av_log(ctx, AV_LOG_ERROR, "Error getting a free surface, pool size is %d\n", pool_size);
    return MFX_ERR_NOT_FOUND;
}

static int get_free_surface_index_out(AVFilterContext *ctx, mfxFrameSurface1 ** surface_pool, int pool_size)
{
    if (surface_pool) {
        for (mfxU16 i = 0; i < pool_size; i++)
            if (0 == surface_pool[i]->Data.Locked)
                return i;
    }

    av_log(ctx, AV_LOG_ERROR, "Error getting a free surface, pool size is %d\n", pool_size);
    return MFX_ERR_NOT_FOUND;
}

static int sysmem_input_get_surface( AVFilterLink *inlink, AVFrame* picref, mfxFrameSurface1 **surface )
{
	AVFilterContext *ctx = inlink->dst;
    VPPContext *vpp = ctx->priv;
    int in_idx = 0;

	in_idx = get_free_surface_index_in(ctx, vpp->in_surface, vpp->num_surfaces_in);
    if ( MFX_ERR_NOT_FOUND == in_idx )
        return in_idx;

    vpp->in_surface[in_idx]->Data.TimeStamp = av_rescale_q(picref->pts, inlink->time_base, (AVRational){1, 90000});
    if (inlink->format == AV_PIX_FMT_NV12) {
        vpp->in_surface[in_idx]->Data.Y = picref->data[0];
        vpp->in_surface[in_idx]->Data.VU = picref->data[1];
        vpp->in_surface[in_idx]->Data.Pitch = picref->linesize[0];
    } else if (inlink->format == AV_PIX_FMT_YUV420P) {
        vpp->in_surface[in_idx]->Data.Y = picref->data[0];
        vpp->in_surface[in_idx]->Data.U = picref->data[1];
        vpp->in_surface[in_idx]->Data.V = picref->data[2];
        vpp->in_surface[in_idx]->Data.Pitch = picref->linesize[0];
    } else if (inlink->format == AV_PIX_FMT_YUYV422 ) {
        vpp->in_surface[in_idx]->Data.Y = picref->data[0];
        vpp->in_surface[in_idx]->Data.U = picref->data[0] + 1;
        vpp->in_surface[in_idx]->Data.V = picref->data[0] + 3;
        vpp->in_surface[in_idx]->Data.Pitch = picref->linesize[0];
    } else if (inlink->format == AV_PIX_FMT_RGB32) { 
        vpp->in_surface[in_idx]->Data.B = picref->data[0];
        vpp->in_surface[in_idx]->Data.G = picref->data[0] + 1;
        vpp->in_surface[in_idx]->Data.R = picref->data[0] + 2;
        vpp->in_surface[in_idx]->Data.A = picref->data[0] + 3;
        vpp->in_surface[in_idx]->Data.Pitch = picref->linesize[0];
    }

    *surface = vpp->in_surface[in_idx];

	return 0;
}

static int vidmem_input_get_surface( AVFilterLink *inlink, AVFrame* picref, mfxFrameSurface1 **surface )
{
    if( picref->data[3] != NULL ){
        *surface = (mfxFrameSurface1*)picref->data[3];
       // av_log(vpp->ctx, AV_LOG_ERROR, "ENCODE: surface MemId=%p Lock=%d\n", (*surface)->Data.MemId, (*surface)->Data.Locked);
        (*surface)->Data.TimeStamp = av_rescale_q(picref->pts, inlink->time_base, (AVRational){1, 90000}); 
        return 0;
    }

    return -1;
}

static int sysmem_output_get_surface( AVFilterLink *inlink, mfxFrameSurface1 **surface )
{
    AVFilterContext *ctx = inlink->dst;
    VPPContext *vpp = ctx->priv;
    int out_idx = 0;

    if (vpp->out_surface) {
        for (out_idx = vpp->sysmem_cur_out_idx; out_idx < vpp->num_surfaces_out; out_idx++)
            if (0 == vpp->out_surface[out_idx]->Data.Locked)
                break;
    }else
        return MFX_ERR_NOT_INITIALIZED;

    if ( vpp->num_surfaces_out == out_idx )
        return MFX_ERR_NOT_FOUND;

    *surface = vpp->out_surface[out_idx];

    vpp->sysmem_cur_out_idx = out_idx + 1;
    if(vpp->sysmem_cur_out_idx >= vpp->num_surfaces_out)
        vpp->sysmem_cur_out_idx = 0;

    return 0;
}

static int vidmem_output_get_surface( AVFilterLink *inlink, mfxFrameSurface1 **surface )
{
	AVFilterContext *ctx = inlink->dst;
    VPPContext      *vpp = ctx->priv;
    int              out_idx = 0;

    out_idx = get_free_surface_index_out(ctx, vpp->out_surface, vpp->num_surfaces_out);
 
 	if ( MFX_ERR_NOT_FOUND == out_idx )
        return out_idx;

    *surface = vpp->out_surface[out_idx];
    
	return 0;
}

static int init_vpp_param( VPPContext *vpp, int format, int input_w, int input_h, int frame_rate_num, int frame_rate_den, int pic_struct )
{
    // input data
    vpp->pVppParam->vpp.In.FourCC = avpix_fmt_to_mfx_fourcc(format);
    vpp->pVppParam->vpp.In.ChromaFormat = get_chroma_fourcc(vpp->pVppParam->vpp.In.FourCC);
    vpp->pVppParam->vpp.In.CropX = 0;
    vpp->pVppParam->vpp.In.CropY = 0;
    vpp->pVppParam->vpp.In.CropW = input_w;
    vpp->pVppParam->vpp.In.CropH = input_h;
    vpp->pVppParam->vpp.In.PicStruct = pic_struct;
    vpp->pVppParam->vpp.In.FrameRateExtN = frame_rate_num;
    vpp->pVppParam->vpp.In.FrameRateExtD = frame_rate_den;
    vpp->pVppParam->vpp.In.BitDepthLuma   = 8; 
    vpp->pVppParam->vpp.In.BitDepthChroma = 8;

    // width must be a multiple of 16
    // height must be a multiple of 16 in case of frame picture and a multiple of 32 in case of field picture
    vpp->pVppParam->vpp.In.Width = VPP_ALIGN16(vpp->pVppParam->vpp.In.CropW);
    vpp->pVppParam->vpp.In.Height =
        (MFX_PICSTRUCT_PROGRESSIVE == vpp->pVppParam->vpp.In.PicStruct) ?
        VPP_ALIGN16(vpp->pVppParam->vpp.In.CropH) :
        VPP_ALIGN32(vpp->pVppParam->vpp.In.CropH);

    // output data
    vpp->pVppParam->vpp.Out.FourCC = MFX_FOURCC_NV12;
    vpp->pVppParam->vpp.Out.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
    vpp->pVppParam->vpp.Out.CropX = 0;
    vpp->pVppParam->vpp.Out.CropY = 0;
    vpp->pVppParam->vpp.Out.CropW = vpp->out_width;
    vpp->pVppParam->vpp.Out.CropH = vpp->out_height;
    vpp->pVppParam->vpp.Out.PicStruct = option_id_to_mfx_pic_struct(vpp->dpic);
    vpp->pVppParam->vpp.Out.FrameRateExtN = vpp->framerate.num;
    vpp->pVppParam->vpp.Out.FrameRateExtD = vpp->framerate.den;
    vpp->pVppParam->vpp.Out.BitDepthLuma   = 8;
    vpp->pVppParam->vpp.Out.BitDepthChroma = 8;

    if ((vpp->pVppParam->vpp.In.FrameRateExtN / vpp->pVppParam->vpp.In.FrameRateExtD) != 
        (vpp->pVppParam->vpp.Out.FrameRateExtN / vpp->pVppParam->vpp.Out.FrameRateExtD))
       vpp->use_frc = 1;
    else
        vpp->use_frc = 0;

    // width must be a multiple of 16
    // height must be a multiple of 16 in case of frame picture and a multiple of 32 in case of field picture
    vpp->pVppParam->vpp.Out.Width = VPP_ALIGN16(vpp->pVppParam->vpp.Out.CropW);
    vpp->pVppParam->vpp.Out.Height =
        (MFX_PICSTRUCT_PROGRESSIVE == vpp->pVppParam->vpp.Out.PicStruct) ?
        VPP_ALIGN16(vpp->pVppParam->vpp.Out.CropH) :
        VPP_ALIGN32(vpp->pVppParam->vpp.Out.CropH);

	if(NULL != vpp->pFrameAllocator){
		vpp->pVppParam->IOPattern =  MFX_IOPATTERN_IN_VIDEO_MEMORY | MFX_IOPATTERN_OUT_VIDEO_MEMORY;
	}else{
		vpp->pVppParam->IOPattern =  MFX_IOPATTERN_IN_SYSTEM_MEMORY | MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
	}

    av_log(vpp->ctx, AV_LOG_INFO, "VPP: In %dx%d %4.2f fps\t Out %dx%d %4.2f fps\n",
           (vpp->pVppParam->vpp.In.Width),
           (vpp->pVppParam->vpp.In.Height),
           vpp->pVppParam->vpp.In.FrameRateExtN / (float)vpp->pVppParam->vpp.In.FrameRateExtD,
           (vpp->pVppParam->vpp.Out.Width),
           (vpp->pVppParam->vpp.Out.Height),
           vpp->pVppParam->vpp.Out.FrameRateExtN / (float)vpp->pVppParam->vpp.Out.FrameRateExtD);

    if (vpp->use_frc == 1)
        av_log(vpp->ctx, AV_LOG_INFO, "VPP: Framerate conversion enabled\n");

    return 0;
}

static int initial_vpp( VPPContext *vpp )
{
    int ret = 0;
	vpp->pVppParam->NumExtParam = 0;
	vpp->frame_number = 0;
	vpp->pVppParam->ExtParam = (mfxExtBuffer**)vpp->pExtBuf;
	
	if (vpp->deinterlace) {
		memset(&vpp->deinterlace_conf, 0, sizeof(mfxExtVPPDeinterlacing));
		vpp->deinterlace_conf.Header.BufferId = MFX_EXTBUFF_VPP_DEINTERLACING;
		vpp->deinterlace_conf.Header.BufferSz = sizeof(mfxExtVPPDeinterlacing);
		vpp->deinterlace_conf.Mode			  = vpp->deinterlace == 1 ? MFX_DEINTERLACING_BOB : MFX_DEINTERLACING_ADVANCED;

		vpp->pVppParam->ExtParam[vpp->pVppParam->NumExtParam++] = (mfxExtBuffer*)&(vpp->deinterlace_conf);
	}
	
	if (vpp->use_frc) {
		memset(&vpp->frc_conf, 0, sizeof(mfxExtVPPFrameRateConversion));
		vpp->frc_conf.Header.BufferId = MFX_EXTBUFF_VPP_FRAME_RATE_CONVERSION;
		vpp->frc_conf.Header.BufferSz = sizeof(mfxExtVPPFrameRateConversion);
		vpp->frc_conf.Algorithm 	  = MFX_FRCALGM_DISTRIBUTED_TIMESTAMP; // make optional
	
		vpp->pVppParam->ExtParam[vpp->pVppParam->NumExtParam++] = (mfxExtBuffer*)&(vpp->frc_conf);
	}
	
	if (vpp->denoise) {
		memset(&vpp->denoise_conf, 0, sizeof(mfxExtVPPDenoise));
		vpp->denoise_conf.Header.BufferId = MFX_EXTBUFF_VPP_DENOISE;
		vpp->denoise_conf.Header.BufferSz = sizeof(mfxExtVPPDenoise);
		vpp->denoise_conf.DenoiseFactor   = vpp->denoise;
	
		vpp->pVppParam->ExtParam[vpp->pVppParam->NumExtParam++] = (mfxExtBuffer*)&(vpp->denoise_conf);
	}
	
	if (vpp->detail) {
		memset(&vpp->detail_conf, 0, sizeof(mfxExtVPPDetail));
		vpp->detail_conf.Header.BufferId  = MFX_EXTBUFF_VPP_DETAIL;
		vpp->detail_conf.Header.BufferSz  = sizeof(mfxExtVPPDetail);
		vpp->detail_conf.DetailFactor	  = vpp->detail;

		vpp->pVppParam->ExtParam[vpp->pVppParam->NumExtParam++] = (mfxExtBuffer*)&(vpp->detail_conf);
	}
	
#if 0
	ret = MFXVideoVPP_Query(vpp->session, vpp->pVppParam, vpp->pVppParam);
	if (ret >= MFX_ERR_NONE) {
		av_log(ctx, AV_LOG_INFO, "MFXVideoVPP_Query returned %d \n", ret);
	} else {
		av_log(ctx, AV_LOG_ERROR, "Error %d querying the VPP parameters\n", ret);
		return ff_qsv_error(ret);
	}
#endif
	
	memset(&vpp->req, 0, sizeof(mfxFrameAllocRequest) * 2);
    ret = MFXVideoVPP_QueryIOSurf(vpp->session, vpp->pVppParam, &vpp->req[0]);
    if (ret < 0) {
        av_log(vpp->ctx, AV_LOG_ERROR, "Error querying the VPP IO surface\n");
        return ff_qsv_error(ret);
    }

    if( NULL != vpp->pFrameAllocator ){
        vidmem_init_surface(vpp);
    }else{
        sysmem_init_surface(vpp);
    }

    ret = MFXVideoVPP_Init(vpp->session, vpp->pVppParam);

    if (MFX_WRN_PARTIAL_ACCELERATION == ret) {
	  av_log(vpp->ctx, AV_LOG_WARNING, "VPP will work with partial HW acceleration\n");
    } else if (ret < 0) {
	  av_log(vpp->ctx, AV_LOG_ERROR, "Error initializing the VPP\n");
	  return ff_qsv_error(ret);
    }

	vpp->vpp_ready = 1;
    return 0;
}

int av_qsv_pipeline_config_vpp(AVCodecContext *dec_ctx, AVFilterContext *vpp_ctx, int frame_rate_num, int frame_rate_den)
{
	mfxVideoParam mfxParamsVideo;
    VPPContext *vpp = vpp_ctx->priv;

	av_log(vpp->ctx, AV_LOG_INFO, "vpp initializing with session = %p\n", vpp->session);

	VPP_ZERO_MEMORY(mfxParamsVideo);
    vpp->pVppParam = &mfxParamsVideo;

    init_vpp_param(vpp, dec_ctx->pix_fmt, dec_ctx->width, dec_ctx->height, 
		                frame_rate_num, frame_rate_den, 
		                field_order_to_mfx_pic_struct(dec_ctx) );
    return initial_vpp( vpp );
}

static int config_vpp(AVFilterLink *inlink, AVFrame * pic)
{
    AVFilterContext *ctx = inlink->dst;
    VPPContext *vpp= ctx->priv;
    mfxVideoParam mfxParamsVideo;
    int           ret;

	av_log(vpp->ctx, AV_LOG_INFO, "vpp configuration and call mfxVideoVPP_Init\n");
    if (!vpp->session) {
        av_log(vpp->ctx, AV_LOG_DEBUG, "sysmem-vpp: GPUCopy %s.\n",
                vpp->internal_qs.gpu_copy == MFX_GPUCOPY_ON ? "enabled" : "disabled");
        ret = ff_qsv_init_internal_session((AVCodecContext *)ctx, &vpp->internal_qs,
                                               vpp->load_plugins);
        if (ret < 0)
            return ret;

        vpp->session = vpp->internal_qs.session;
	}

   	av_log(ctx, AV_LOG_INFO, "vpp initializing with session = %p\n", vpp->session);
	VPP_ZERO_MEMORY(mfxParamsVideo);
    vpp->pVppParam = &mfxParamsVideo;

    init_vpp_param(vpp, inlink->format, inlink->w, inlink->h, 
		                inlink->frame_rate.num, inlink->frame_rate.den,
		                avframe_id_to_mfx_pic_struct(pic) );

    return initial_vpp( vpp );
}

static void deconf_vpp(AVFilterContext *ctx)
{
    VPPContext      *vpp = ctx->priv;

    MFXVideoVPP_Close(vpp->session);

    if(NULL != vpp->pFrameAllocator)
		vidmem_free_surface(ctx);
	else
		sysmem_free_surface(ctx);

    ff_qsv_close_internal_session(&vpp->internal_qs);
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

static int config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    VPPContext      *vpp = ctx->priv;

    av_log(ctx, AV_LOG_INFO, "main input format is %s\n", 
        av_get_pix_fmt_name(inlink->format));
    if(vpp->framerate.den == 0 || vpp->framerate.num == 0)
        vpp->framerate = inlink->frame_rate;

    /*By default, out_rect = main_in_rect*/
    if(vpp->out_height == 0 || vpp->out_width == 0){
        vpp->out_width  = inlink->w;
        vpp->out_height = inlink->h;
    }

    return 0;
}

static int config_output(AVFilterLink *outlink)
{
    int              ret = 0;
    AVFilterContext *ctx = outlink->src;
    VPPContext      *vpp = ctx->priv;
    AVCodec         *codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);

    /*out wxh should NOT be aligned!!!! keep it origin*/
    outlink->w             = vpp->out_width;
    outlink->h             = vpp->out_height;
    outlink->frame_rate    = vpp->framerate;
    outlink->time_base     = av_inv_q(vpp->framerate);
    outlink->format        = AV_PIX_FMT_NV12;

    vpp->thm_swsctx = sws_getContext(outlink->w, outlink->h, outlink->format,
            outlink->w, outlink->h, AV_PIX_FMT_YUVJ420P, 
            SWS_BICUBIC, NULL, NULL, NULL);
    
    ret = avformat_alloc_output_context2(&vpp->thm_mux, NULL, "mjpeg", NULL);
    if(ret < 0){
        av_log(ctx, AV_LOG_ERROR, "mux init failed with %s.\n", av_err2str(ret));
        return ret;
    }

    vpp->thm_stream        = avformat_new_stream(vpp->thm_mux, codec);
    vpp->thm_enc           = vpp->thm_stream->codec;
    vpp->thm_enc->width    = outlink->w;
    vpp->thm_enc->height   = outlink->h;
    vpp->thm_enc->pix_fmt  = AV_PIX_FMT_YUVJ420P;
    vpp->thm_stream->time_base = vpp->thm_enc->time_base = av_inv_q(vpp->framerate);
    ret = avcodec_open2(vpp->thm_enc, vpp->thm_enc->codec, NULL);

    return ret;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *picref)
{
    int               ret  = 0;
    int               filter_frame_ret = 0;
    AVFilterContext  *ctx         = inlink->dst;
    VPPContext       *vpp         = ctx->priv;
    mfxSyncPoint      sync        = NULL;
    mfxFrameSurface1 *pInSurface  = NULL;
    mfxFrameSurface1 *pOutSurface = NULL;
    AVFilterLink     *outlink     = inlink->dst->outputs[0];
    AVFrame          *out         = NULL;

    /*we re-config local params when getting 1st frame.*/
    if ( !vpp->vpp_ready ) 
        config_vpp(inlink, picref);

    do {
        /*get input surface*/
        if( NULL != vpp->pFrameAllocator){
            vidmem_input_get_surface( inlink, picref, &pInSurface );
            vidmem_output_get_surface( inlink, &pOutSurface );
        }else{
            sysmem_input_get_surface( inlink, picref, &pInSurface );
            sysmem_output_get_surface( inlink, &pOutSurface );
        }

        if( !pInSurface || !pOutSurface ){
            av_log(ctx, AV_LOG_ERROR, "no free input or output surface\n");
            ret = MFX_ERR_MEMORY_ALLOC;
            break;
        }

        /*get an AVFrame for output*/
        out = ff_get_video_buffer(outlink, FFALIGN(vpp->out_width, 128), FFALIGN(vpp->out_height, 64));
        if (!out) {
            ret = MFX_ERR_MEMORY_ALLOC;
            break;
        }
        av_frame_copy_props(out, picref);
        out->width = vpp->out_width;
        out->height = vpp->out_height;

        /*map avframe->data into outsurface*/
        if( NULL == vpp->pFrameAllocator){
            pOutSurface->Data.Y  = out->data[0];
            pOutSurface->Data.VU = out->data[1];
            pOutSurface->Data.Pitch = out->linesize[0];
        }

        do {
            ret = MFXVideoVPP_RunFrameVPPAsync(vpp->session, pInSurface, pOutSurface, NULL, &sync);
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

        MFXVideoCORE_SyncOperation(vpp->session, sync, 60000);

        out->interlaced_frame = vpp->dpic == 1 || vpp->dpic == 3;
        out->top_field_first  = vpp->dpic == 1;
        if(pOutSurface->Data.TimeStamp == MFX_TIMESTAMP_UNKNOWN)
            out->pts = AV_NOPTS_VALUE;
        else
            out->pts = av_rescale_q(pOutSurface->Data.TimeStamp, (AVRational){1, 90000}, outlink->time_base);
        /*For video mem, we use AVFrame->data[3] to transfer surface*/
        if( NULL != vpp->pFrameAllocator)
    	    out->data[3] = (void*) pOutSurface;

        if((vpp->thm_pendding || (vpp->use_thumbnail && (vpp->frame_number % vpp->thumb_interval == 0)))
                && av_fifo_space(vpp->thm_framebuffer) > sizeof(AVFrame*)){
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
 
        filter_frame_ret = ff_filter_frame(inlink->dst->outputs[0], out);
        if (filter_frame_ret < 0)
            break;

        vpp->frame_number++;
    } while(ret == MFX_ERR_MORE_SURFACE);

    av_frame_free(&picref);

    return ret < 0 ? ff_qsv_error(ret) : filter_frame_ret;
}

static int query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *in_fmts, *out_fmts;
    static const enum AVPixelFormat in_pix_fmts[] = {
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_NV12,
        AV_PIX_FMT_YUYV422,
        AV_PIX_FMT_RGB32,
        AV_PIX_FMT_QSV,
        AV_PIX_FMT_NONE
    };
    static const enum AVPixelFormat out_pix_fmts[] = {
        AV_PIX_FMT_NV12,
        AV_PIX_FMT_QSV,
        AV_PIX_FMT_NONE
    };

    in_fmts  = ff_make_format_list(in_pix_fmts);
    out_fmts = ff_make_format_list(out_pix_fmts);
    ff_formats_ref(in_fmts, &ctx->inputs[0]->out_formats);
    ff_formats_ref(out_fmts, &ctx->outputs[0]->in_formats);

    return 0;
}

static av_cold int vpp_init(AVFilterContext *ctx)
{
    VPPContext *vpp= ctx->priv;

    vpp->frame_number    = 0;
    vpp->pFrameAllocator = NULL;
	vpp->vpp_ready       = 0;
    vpp->ctx             = ctx;
    vpp->thm_framebuffer = av_fifo_alloc(sizeof(AVFrame*)*8);
    vpp->task_exit       = 0;
    vpp->thm_pendding    = 0;
    vpp->sysmem_cur_out_idx = 0;
    if(!vpp->thumbnail_file)
        vpp->thumbnail_file = av_strdup("thumbnail-%d.jpg");
    pthread_create(&vpp->thumbnail_task, NULL, thumbnail_task, ctx->priv);

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
        .name          = "default",
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
};
