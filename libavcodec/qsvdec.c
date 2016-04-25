/*
 * Intel MediaSDK QSV codec-independent code
 *
 * copyright (c) 2013 Luca Barbato
 * copyright (c) 2015 Anton Khirnov <anton@khirnov.net>
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

#include <string.h>
#include <sys/types.h>

#include <mfx/mfxvideo.h>

#include "libavutil/common.h"
#include "libavutil/mem.h"
#include "libavutil/log.h"
#include "libavutil/pixfmt.h"
#include "libavutil/time.h"

#include "avcodec.h"
#include "internal.h"
#include "qsv.h"
#include "qsv_internal.h"
#include "qsvenc.h"
#include "qsvdec.h"
#include "vaapi_allocator.h"

int ff_qsv_map_pixfmt(enum AVPixelFormat format)
{
    switch (format) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
        return AV_PIX_FMT_NV12;
    default:
        return AVERROR(ENOSYS);
    }
}

static void free_surfaces(QSVContext *q, mfxFrameAllocResponse *resp)
{
	//surface will be freed when decoder closed
	//
	/*VASurfaceID* surface = (VASurfaceID*)calloc(q->nb_surfaces, sizeof(VASurfaceID));

	int i = 0;
	QSVFrame* cur = q->work_frames;
	while( NULL != cur ){
	    vaapiMemId* memID = (vaapiMemId*)(cur->surface->Data.MemId);
		surface[i] = *(memID->m_surface);
		cur = cur->next;
		i++;
	}

    if( surface && q->nb_surfaces!=0 )
        vaDestroySurfaces( q->internal_qs.va_display, surface, i );
		
	av_freep( &surface );
	*/	
	av_freep( q->work_frames );
	
	q->nb_surfaces = 0;
}

static int alloc_frame(AVCodecContext *avctx, QSVFrame *frame)
{
    int ret;

    ret = ff_get_buffer(avctx, frame->frame, AV_GET_BUFFER_FLAG_REF);
    if (ret < 0)
        return ret;

    if (frame->frame->format == AV_PIX_FMT_QSV) {
        frame->surface = (mfxFrameSurface1*)frame->frame->data[3];
	    av_log(NULL, AV_LOG_INFO, "Pixel format is AV_PIX_FMT_QSV\n" );
    } else {
        frame->surface_internal.Info.BitDepthLuma   = 8;
        frame->surface_internal.Info.BitDepthChroma = 8;
        frame->surface_internal.Info.FourCC         = MFX_FOURCC_NV12;
        frame->surface_internal.Info.Width          = avctx->coded_width;
        frame->surface_internal.Info.Height         = avctx->coded_height;
        frame->surface_internal.Info.ChromaFormat   = MFX_CHROMAFORMAT_YUV420;

        frame->surface_internal.Data.PitchLow = frame->frame->linesize[0];
        frame->surface_internal.Data.Y        = frame->frame->data[0];
        frame->surface_internal.Data.UV       = frame->frame->data[1];

        frame->surface = &frame->surface_internal;
    }

    return 0;
}


static int codec_connect( QSVContext* qsv_dec_ctx, AVCodecContext* av_dec_ctx, AVCodecContext* av_enc_ctx, int vpp_type )
{
	int ret = 0;
    
    VADisplay va_dpy;
    QSVEncContext* qsv_enc_ctx = NULL;
    QSVContext* q =  qsv_dec_ctx;

	if( (NULL == av_dec_ctx ) || (NULL == av_enc_ctx) || (NULL == qsv_dec_ctx) )
		return -1;
   
	if(NULL == av_enc_ctx->priv_data)
		return ret;

	if( AVFILTER_MORE == vpp_type ) return ret;

	av_log(NULL, AV_LOG_INFO, "source: width = %d, height=%d \n output: width = %d, height = %d\n",
			                 av_dec_ctx->width, av_dec_ctx->height,
							 av_enc_ctx->width, av_enc_ctx->height);
   	if( strcmp(av_enc_ctx->codec->name, "h264_qsv")==0 ){
     	QSVH264EncContext* h264_enc_ctx;
     	h264_enc_ctx = (QSVH264EncContext*) av_enc_ctx->priv_data;
		qsv_enc_ctx = &(h264_enc_ctx->qsv);
	}

   	if( strcmp(av_enc_ctx->codec->name, "mpeg2_qsv")==0 ){
     	QSVMpeg2EncContext* mpeg2_enc_ctx;
     	mpeg2_enc_ctx = (QSVMpeg2EncContext*) av_enc_ctx->priv_data;
		qsv_enc_ctx = &(mpeg2_enc_ctx->qsv);
	}

  	if( strcmp(av_enc_ctx->codec->name, "hevc_qsv")==0 ){
     	QSVHEVCEncContext* hevc_enc_ctx;
     	hevc_enc_ctx = (QSVHEVCEncContext*) av_enc_ctx->priv_data;
		qsv_enc_ctx = &(hevc_enc_ctx->qsv);
	}

    q->enc_ctx = qsv_enc_ctx;

	//video memory is used when decoder and encoder are all supported with HW
	if( NULL != qsv_enc_ctx ){

        q->iopattern = MFX_IOPATTERN_OUT_VIDEO_MEMORY;
		ret = MFXVideoCORE_GetHandle(q->session, MFX_HANDLE_VA_DISPLAY, (mfxHDL*)&va_dpy);
		
		q->frame_allocator.pthis = q;
		q->frame_allocator.Alloc = frame_alloc;
		q->frame_allocator.Lock = frame_lock;
		q->frame_allocator.Unlock = frame_unlock;
		q->frame_allocator.GetHDL = frame_get_hdl;
		q->frame_allocator.Free = frame_free;
		
		MFXVideoCORE_SetFrameAllocator( q->session, &q->frame_allocator );
		av_log(NULL, AV_LOG_INFO, "DECODE: session=%p SetFrameAllocator dpy=%p\n",q->session, va_dpy);

     	qsv_enc_ctx->session = q->session;
        qsv_enc_ctx->iopattern = q->iopattern;
	}

	return ret;
}

#if 1 
int ff_qsv_pipeline_insert_vpp( AVCodecContext *av_dec_ctx, AVFilterContext* vpp_ctx )
{
    VPPContext *vpp = NULL;
	QSVContext *qsv = NULL;

	if(NULL == vpp_ctx) return 0;

    vpp = (VPPContext*) vpp_ctx->priv;
    
	if( (strcmp(av_dec_ctx->codec->name, "h264_qsv")==0) ||  (strcmp(av_dec_ctx->codec->name, "hevc_qsv")==0 ) ){
		QSVH2645Context* qsv_ctx = (QSVH2645Context*) av_dec_ctx->priv_data;
		qsv = &(qsv_ctx->qsv);//.pCodecConnect(&(qsv_ctx->qsv), av_dec_ctx, av_enc_ctx);
    }
        
	if( strcmp(av_dec_ctx->codec->name, "mpeg2_qsv")==0 ){
		QSVMPEG2Context* qsv_ctx = (QSVMPEG2Context*) av_dec_ctx->priv_data;
		qsv = &(qsv_ctx->qsv);//.pCodecConnect(&(qsv_ctx->qsv), av_dec_ctx, av_enc_ctx);
    }
        
	if( strcmp(av_dec_ctx->codec->name, "vc1_qsv")==0 )
    {
    	QSVVC1Context* qsv_ctx = (QSVVC1Context*) av_dec_ctx->priv_data;
		qsv = &(qsv_ctx->qsv);//.pCodecConnect(&(qsv_ctx->qsv), av_dec_ctx, av_enc_ctx);
    }

	if(NULL == qsv->enc_ctx) return 0;
	qsv->vpp = vpp;
	vpp->session = qsv->session;
	vpp->pFrameAllocator = &(qsv->frame_allocator);
	vpp->enc_ctx = qsv->enc_ctx;
		
	return 0;
}
#endif

int ff_qsv_pipeline_connect_codec( AVCodecContext *av_dec_ctx, AVCodecContext *av_enc_ctx, int vpp_type )
{
    QSVContext *qsv = NULL;
    if( (strcmp(av_dec_ctx->codec->name, "h264_qsv")==0) ||  (strcmp(av_dec_ctx->codec->name, "hevc_qsv")==0 ) ){
		QSVH2645Context* qsv_ctx = (QSVH2645Context*) av_dec_ctx->priv_data;
		qsv = &(qsv_ctx->qsv);//.pCodecConnect(&(qsv_ctx->qsv), av_dec_ctx, av_enc_ctx);
    }
        
	if( strcmp(av_dec_ctx->codec->name, "mpeg2_qsv")==0 ){
		QSVMPEG2Context* qsv_ctx = (QSVMPEG2Context*) av_dec_ctx->priv_data;
		qsv = &(qsv_ctx->qsv);//.pCodecConnect(&(qsv_ctx->qsv), av_dec_ctx, av_enc_ctx);
    }
        
	if( strcmp(av_dec_ctx->codec->name, "vc1_qsv")==0 )
    {
    	QSVVC1Context* qsv_ctx = (QSVVC1Context*) av_dec_ctx->priv_data;
		qsv = &(qsv_ctx->qsv);//.pCodecConnect(&(qsv_ctx->qsv), av_dec_ctx, av_enc_ctx);
    }
    
	if(NULL == qsv) return -1;

	return codec_connect(qsv, av_dec_ctx, av_enc_ctx, vpp_type);
}

int ff_qsv_decode_init_session(AVCodecContext *avctx, QSVContext *q)
{
	int ret = 0;
    av_log(avctx, AV_LOG_INFO, "DECODE: ff_qsv_decode_init_session\n");
    if (!q->session) {
		ret = ff_qsv_init_internal_session(avctx, &q->internal_qs, q->load_plugins);
		if (ret < 0){
			av_log(avctx, AV_LOG_ERROR, "ff_qsv_init_internal_session failed\n");		
            return ret;
		}

        q->session = q->internal_qs.session;			
    }

	//q->pCodecConnect = codec_connect;
    q->enc_ctx = NULL;
	q->vpp = NULL;

	return ret;
}

static int qsv_decode_init_sysmem(AVCodecContext *avctx, QSVContext *q, AVPacket *avpkt)
{
    mfxVideoParam param = { { 0 } };
    mfxBitstream bs   = { { { 0 } } };
    int ret;
    enum AVPixelFormat pix_fmts[3] = { AV_PIX_FMT_QSV,
                                       AV_PIX_FMT_NV12,
                                       AV_PIX_FMT_NONE };

    av_log(avctx, AV_LOG_INFO,"*******ff_qsv_decode_init_sysmem**********\n");
    q->iopattern  = MFX_IOPATTERN_OUT_SYSTEM_MEMORY;
#if 0
	if (!q->session) {
        if (avctx->hwaccel_context) {
            AVQSVContext *qsv = avctx->hwaccel_context;

            q->session        = qsv->session;
            q->iopattern      = qsv->iopattern;
            q->ext_buffers    = qsv->ext_buffers;
            q->nb_ext_buffers = qsv->nb_ext_buffers;
        }
		
        if (!q->session) {
            ret = ff_qsv_init_internal_session(avctx, &q->internal_qs,
                                               q->load_plugins);
            if (ret < 0)
                return ret;

            q->session = q->internal_qs.session;
        }
    }
#endif
    if (avpkt->size) {
        bs.Data       = avpkt->data;
        bs.DataLength = avpkt->size;
        bs.MaxLength  = bs.DataLength;
        bs.TimeStamp  = avpkt->pts;
    } else
        return AVERROR_INVALIDDATA;

    ret = ff_qsv_codec_id_to_mfx(avctx->codec_id);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported codec_id %08x\n", avctx->codec_id);
        return ret;
    }

    param.mfx.CodecId = ret;

    ret = MFXVideoDECODE_DecodeHeader(q->session, &bs, &param);
    if (MFX_ERR_MORE_DATA==ret) {
        /* this code means that header not found so we return packet size to skip
           a current packet
         */
        return avpkt->size;
    } else if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Decode header error %d\n", ret);
        return ff_qsv_error(ret);
    }
    param.IOPattern   = q->iopattern;
    param.AsyncDepth  = q->async_depth;
    param.ExtParam    = q->ext_buffers;
    param.NumExtParam = q->nb_ext_buffers;
    param.mfx.FrameInfo.BitDepthLuma   = 8;
    param.mfx.FrameInfo.BitDepthChroma = 8;

    ret = MFXVideoDECODE_Init(q->session, &param);
    if (ret < 0) {
        if (MFX_ERR_INVALID_VIDEO_PARAM==ret) {
            av_log(avctx, AV_LOG_ERROR,
                   "Error initializing the MFX video decoder, unsupported video\n");
        } else {
            av_log(avctx, AV_LOG_ERROR,
                   "Error initializing the MFX video decoder %d\n", ret);
        }
        return ff_qsv_error(ret);
    }

    ret = ff_get_format(avctx, pix_fmts);
    if (ret < 0)
        return ret;

    avctx->pix_fmt      = ret;
    avctx->profile      = param.mfx.CodecProfile;
    avctx->level        = param.mfx.CodecLevel;
    avctx->coded_width  = param.mfx.FrameInfo.Width;
    avctx->coded_height = param.mfx.FrameInfo.Height;
    avctx->width        = param.mfx.FrameInfo.CropW - param.mfx.FrameInfo.CropX;
    avctx->height       = param.mfx.FrameInfo.CropH - param.mfx.FrameInfo.CropY;

    /* maximum decoder latency should be not exceed max DPB size for h.264 and
       HEVC which is 16 for both cases.
       So weare  pre-allocating fifo big enough for 17 elements:
     */
    if (!q->async_fifo) {
        q->async_fifo = av_fifo_alloc((1 + 16) *
                                      (sizeof(mfxSyncPoint) + sizeof(QSVFrame*)));
        if (!q->async_fifo)
            return AVERROR(ENOMEM);
    }

    if (!q->input_fifo) {
        q->input_fifo = av_fifo_alloc(1024*16);
        if (!q->input_fifo)
            return AVERROR(ENOMEM);
    }

    if (!q->pkt_fifo) {
        q->pkt_fifo = av_fifo_alloc( sizeof(AVPacket) * (1 + 16) );
        if (!q->pkt_fifo)
            return AVERROR(ENOMEM);
    }
    q->engine_ready = 1;

    return 0;
}

static int qsv_decode_init_vidmem(AVCodecContext *avctx, QSVContext *q, AVPacket *avpkt)
{
    mfxVideoParam param = { { 0 } };
    mfxBitstream bs   = { { { 0 } } };
    int ret;
    //VADisplay va_dpy;
    enum AVPixelFormat pix_fmts[3] = { AV_PIX_FMT_QSV,
                                       AV_PIX_FMT_NV12,
                                       AV_PIX_FMT_NONE };

    av_log(avctx, AV_LOG_INFO,"*******ff_qsv_decode_init_vidmem**********\n");
    q->iopattern = MFX_IOPATTERN_OUT_VIDEO_MEMORY;
#if 0
    if (!q->session) {
		ret = ff_qsv_init_internal_session(avctx, &q->internal_qs, q->load_plugins);
		if (ret < 0){
			av_log(avctx, AV_LOG_ERROR, "ff_qsv_init_internal_session failed\n");		
            return ret;
		}

        q->session = q->internal_qs.session;			
    }
    
	ret = MFXVideoCORE_GetHandle(q->session, MFX_HANDLE_VA_DISPLAY, (mfxHDL*)&va_dpy);
 
    q->frame_allocator.pthis = q;
    q->frame_allocator.Alloc = frame_alloc;
    q->frame_allocator.Lock = frame_lock;
    q->frame_allocator.Unlock = frame_unlock;
    q->frame_allocator.GetHDL = frame_get_hdl;
    q->frame_allocator.Free = frame_free;

    MFXVideoCORE_SetFrameAllocator( q->session, &q->frame_allocator );
    av_log(avctx, AV_LOG_INFO, "DECODE: session=%08x SetFrameAllocator dpy=%p\n",q->session, va_dpy);

#endif
    if (avpkt->size) {
        bs.Data       = avpkt->data;
        bs.DataLength = avpkt->size;
        bs.MaxLength  = bs.DataLength;
        bs.TimeStamp  = avpkt->pts;
    } else
        return AVERROR_INVALIDDATA;

    ret = ff_qsv_codec_id_to_mfx(avctx->codec_id);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Unsupported codec_id %08x\n", avctx->codec_id);
        return ret;
    }

    param.mfx.CodecId = ret;

    ret = MFXVideoDECODE_DecodeHeader(q->session, &bs, &param);
    if (MFX_ERR_MORE_DATA==ret) {
        /* this code means that header not found so we return packet size to skip
           a current packet
         */
        return avpkt->size;
    } else if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Decode header error %d\n", ret);
        return ff_qsv_error(ret);
    }
    av_log(avctx, AV_LOG_INFO, "DECODE: Decode Header\n");
	
    param.IOPattern   = q->iopattern;
    param.AsyncDepth  = q->async_depth;
    param.ExtParam    = q->ext_buffers;
    param.NumExtParam = q->nb_ext_buffers;
    param.mfx.FrameInfo.BitDepthLuma   = 8;
    param.mfx.FrameInfo.BitDepthChroma = 8;
    
    //Added by GY: alloc surfaces for decoder
	q->request = av_mallocz(sizeof(mfxFrameAllocRequest));
	q->response = av_malloc(sizeof(mfxFrameAllocResponse));
    memset( q->request, 0, sizeof(mfxFrameAllocRequest) );
    memset( q->response, 0, sizeof(mfxFrameAllocResponse) );

    ret = MFXVideoDECODE_QueryIOSurf( q->session, &param, q->request );
    if( ret < 0 ){
        av_log(avctx, AV_LOG_ERROR, "QueryIOSurf failed with return %d\n", ret);
        return ff_qsv_error( ret );
    }
	
	if( NULL != q->enc_ctx ){
		if( NULL == q->vpp ){
			q->request->NumFrameSuggested += q->enc_ctx->req.NumFrameSuggested;
	    	q->request->NumFrameMin += q->enc_ctx->req.NumFrameSuggested;
		}else{
			q->request->NumFrameSuggested += q->vpp->req[0].NumFrameSuggested;
	    	q->request->NumFrameMin += q->vpp->req[0].NumFrameSuggested;
		}
			
	}


    av_log(avctx, AV_LOG_INFO, "DECODE: QueryIOSurf ret=%d W=%d H=%d NumFrameMin=%d NumFrameSuggested=%d FourCC=%08x\n",
            ret,q->request->Info.Width, q->request->Info.Height, q->request->NumFrameMin, q->request->NumFrameSuggested, q->request->Info.FourCC);

    av_log(avctx, AV_LOG_INFO, "DECODE: start  DECODE_Init\n");
   // q->frame_allocator.Alloc( q, q->request, q->response);
    ret = MFXVideoDECODE_Init(q->session, &param);
    if (ret < 0) {
        if (MFX_ERR_INVALID_VIDEO_PARAM==ret) {
            av_log(avctx, AV_LOG_ERROR,
                   "Error initializing the MFX video decoder, unsupported video\n");
        } else {
            av_log(avctx, AV_LOG_ERROR,
                   "Error initializing the MFX video decoder %d\n", ret);
        }
        return ff_qsv_error(ret);
    }
    av_log(avctx, AV_LOG_INFO, "DECODE: DECODE_Init ret=%d\n",ret);

    ret = ff_get_format(avctx, pix_fmts);
    if (ret < 0)
        return ret;

    avctx->pix_fmt      = ret;
    avctx->profile      = param.mfx.CodecProfile;
    avctx->level        = param.mfx.CodecLevel;
    avctx->coded_width  = param.mfx.FrameInfo.Width;
    avctx->coded_height = param.mfx.FrameInfo.Height;
    avctx->width        = param.mfx.FrameInfo.CropW - param.mfx.FrameInfo.CropX;
    avctx->height       = param.mfx.FrameInfo.CropH - param.mfx.FrameInfo.CropY;

    //set info to enc
	if( NULL != q->enc_ctx )
		q->enc_ctx->work_frames = q->work_frames;

    /* maximum decoder latency should be not exceed max DPB size for h.264 and
       HEVC which is 16 for both cases.
       So weare  pre-allocating fifo big enough for 17 elements:
     */
    if (!q->async_fifo) {
        q->async_fifo = av_fifo_alloc((1 + 16) *
                                      (sizeof(mfxSyncPoint) + sizeof(QSVFrame*)));
        if (!q->async_fifo)
            return AVERROR(ENOMEM);
    }

    if (!q->input_fifo) {
        q->input_fifo = av_fifo_alloc(1024*16);
        if (!q->input_fifo)
            return AVERROR(ENOMEM);
    }

    if (!q->pkt_fifo) {
        q->pkt_fifo = av_fifo_alloc( sizeof(AVPacket) * (1 + 16) );
        if (!q->pkt_fifo)
            return AVERROR(ENOMEM);
    }
    q->engine_ready = 1;

	av_log(avctx, AV_LOG_INFO, "ff_qsv_decode_init is done\n");

    return 0;
}

int ff_qsv_decode_init(AVCodecContext *avctx, QSVContext *q, AVPacket *avpkt)
{
	if(NULL != q->enc_ctx){
		return qsv_decode_init_vidmem(avctx, q, avpkt);
	}

	return qsv_decode_init_sysmem(avctx, q, avpkt);
}

static void qsv_clear_unused_frames(QSVContext *q)
{
    //printf("qsv_clear_unused_frames now!!!\n");

    QSVFrame *cur = q->work_frames;
    while (cur) {
        if (cur->surface && !cur->surface->Data.Locked && !cur->queued) {
            cur->surface = NULL;
            av_frame_unref(cur->frame);
        }
        cur = cur->next;
    }
}

static int get_surface(AVCodecContext *avctx, QSVContext *q, mfxFrameSurface1 **surf)
{
    QSVFrame *frame, **last;
    int ret;

    qsv_clear_unused_frames(q);

    frame = q->work_frames;
    last  = &q->work_frames;
    while (frame) {
        if (!frame->surface) {
            ret = alloc_frame(avctx, frame);
            if (ret < 0)
                return ret;
            *surf = frame->surface;
            return 0;
        }

        last  = &frame->next;
        frame = frame->next;
    }

    frame = av_mallocz(sizeof(*frame));
    if (!frame)
        return AVERROR(ENOMEM);
    frame->frame = av_frame_alloc();
    if (!frame->frame) {
        av_freep(&frame);
        return AVERROR(ENOMEM);
    }
    *last = frame;

    ret = alloc_frame(avctx, frame);
    if (ret < 0)
        return ret;

    *surf = frame->surface;

    return 0;
}

//add this functions to allocate more surface dynamically when video_memory is used 
//because decoder cannot know the frame count needed by vpp or encoder before they 
//are initilized.
/*static int alloc_frames_more(AVCodecContext *avctx, QSVContext *q, int num)
{
	q->request->NumFrameMin = num;
	q->request->NumFrameSuggested = num;
    q->frame_allocator.Alloc( q, q->request, q->response);
	av_log( avctx, AV_LOG_INFO, "frame_allocator more: NumFrameActual=%d\n",q->response->NumFrameActual);
	 
	return 0;
}*/

static int get_free_surface(AVCodecContext *avctx, QSVContext *q, mfxFrameSurface1 **surf)
{
   	QSVFrame *cur_frames = NULL;
	do {
        *surf = NULL;
	
		cur_frames = q->work_frames;
		while(NULL != cur_frames){
			if( !(cur_frames->surface->Data.Locked) && !cur_frames->queued ){
               /*   av_log( NULL, AV_LOG_INFO, "selected free surface=%p, NumExtParam=%d, width=%d, height=%d, PitchHigh=%d, PitchLow=%d, Y=%p, UV=%p, corrupted=%d \n",
					                       cur_frames->surface->Data.MemId,
										   cur_frames->surface->Data.NumExtParam,
										   cur_frames->surface->Info.Width, 
										   cur_frames->surface->Info.Height,
										   cur_frames->surface->Data.PitchHigh,
										   cur_frames->surface->Data.PitchLow,
										   cur_frames->surface->Data.Y,
										   cur_frames->surface->Data.UV,
										   cur_frames->surface->Data.Corrupted);*/
                *surf = (cur_frames->surface);
				break;
			}
			cur_frames = cur_frames->next;
		}

        if( *surf != NULL ){
			break;
		}
		else{
			av_log( avctx, AV_LOG_INFO, "waiting until there are free surface\n" );
//			alloc_frames_more(avctx, q, 10 );
			av_usleep(1000);
		}

    } while(1);

    return 0;
}

static QSVFrame *find_frame(QSVContext *q, mfxFrameSurface1 *surf)
{
    QSVFrame *cur;
	cur = q->work_frames;
   
	while (cur) {
        	if (surf == cur->surface)
            	return cur;
        	cur = cur->next;
    	}

    return NULL;
}

/*  This function uses for 'smart' releasing of consumed data
    from the input bitstream fifo.
    Since the input fifo mapped to mfxBitstream which does not understand
    a wrapping of data over fifo end, we should also to relocate a possible
    data rest to fifo begin. If rest of data is absent then we just reset fifo's
    pointers to initial positions.
    NOTE the case when fifo does contain unconsumed data is rare and typical
    amount of such data is 1..4 bytes.
*/
static void qsv_fifo_relocate(AVFifoBuffer *f, int bytes_to_free)
{
    int data_size;
    int data_rest = 0;

    av_fifo_drain(f, bytes_to_free);

    data_size = av_fifo_size(f);
    if (data_size > 0) {
        if (f->buffer!=f->rptr) {
            if ( (f->end - f->rptr) < data_size) {
                data_rest = data_size - (f->end - f->rptr);
                data_size-=data_rest;
                memmove(f->buffer+data_size, f->buffer, data_rest);
            }
            memmove(f->buffer, f->rptr, data_size);
            data_size+= data_rest;
        }
    }
    f->rptr = f->buffer;
    f->wptr = f->buffer + data_size;
    f->wndx = data_size;
    f->rndx = 0;
}

static void close_decoder_vidmem(QSVContext *q)
{
    free_surfaces( q, q->response );
	
    MFXVideoDECODE_Close(q->session);

    q->engine_ready   = 0;
    q->reinit_pending = 0;

    //av_log(NULL, AV_LOG_INFO,"close decoder done!\n");
}

static void close_decoder_sysmem(QSVContext *q)
{
    QSVFrame *cur;

    MFXVideoDECODE_Close(q->session);

    cur = q->work_frames;
    while (cur) {
        q->work_frames = cur->next;
        av_frame_free(&cur->frame);
        av_freep(&cur);
        cur = q->work_frames;
    }

    q->engine_ready   = 0;
    q->reinit_pending = 0;
}


static void close_decoder(QSVContext *q)
{
	switch(q->iopattern){
		case MFX_IOPATTERN_OUT_VIDEO_MEMORY:
			close_decoder_vidmem(q);
			break;
		case MFX_IOPATTERN_OUT_SYSTEM_MEMORY:			
		default:
			close_decoder_sysmem(q);
			break;
	}
}

static int do_qsv_decode(AVCodecContext *avctx, QSVContext *q,
                  AVFrame *frame, int *got_frame,
                  AVPacket *avpkt)
{
   	QSVFrame *out_frame;
	mfxFrameSurface1* insurface;
	mfxFrameSurface1* outsurface;
	mfxSyncPoint sync;
	
    static int count =0;
    mfxBitstream bs = { { { 0 } } };
    int ret;
    int n_out_frames;
    int buffered = 0;
    int flush    = !avpkt->size || q->reinit_pending;

    if (!q->engine_ready) {
        ret = ff_qsv_decode_init(avctx, q, avpkt);
        if (ret)
            return ret;
    }
    if (!flush) {
        if (av_fifo_size(q->input_fifo)) {
            /* we have got rest of previous packet into buffer */
            if (av_fifo_space(q->input_fifo) < avpkt->size) {
                ret = av_fifo_grow(q->input_fifo, avpkt->size);
                if (ret < 0)
                    return ret;
            }
            av_fifo_generic_write(q->input_fifo, avpkt->data, avpkt->size, NULL);
            bs.Data       = q->input_fifo->rptr;
            bs.DataLength = av_fifo_size(q->input_fifo);
            buffered = 1;
        } else {
            bs.Data       = avpkt->data;
            bs.DataLength = avpkt->size;
        }
        bs.MaxLength  = bs.DataLength;
        bs.TimeStamp  = avpkt->pts;
    }
    //bs.TimeStamp = 0;	
    while (1) {
		switch(q->iopattern){
		case MFX_IOPATTERN_OUT_VIDEO_MEMORY:
            ret = get_free_surface(avctx, q, &insurface);
			break;
		case MFX_IOPATTERN_OUT_SYSTEM_MEMORY:
		default:
            ret = get_surface(avctx, q, &insurface);
			break;
	    }

        if (ret < 0)
            return ret;
	
//      printf("DECODE: surface info: width=%d height=%d %d %d %d %d\n", 
//                      insurface->Info.Width, 
//                      insurface->Info.Height, 
//                      insurface->Info.CropX, 
//                      insurface->Info.CropY, 
//                      insurface->Info.CropW, 
//                      insurface->Info.CropH);
//    	printf("DECODE: surface info: FourCC=%d ChromaFormat=%d\n", 
//    	                (insurface->Info.FourCC!=MFX_FOURCC_NV12)?1:0, 
//    	                (insurface->Info.ChromaFormat!=MFX_CHROMAFORMAT_YUV420)?1:0);
//    	printf("DECODE: surface info: BitDepthLuma=%d, BitDepthChroma=%d, Shift=%d\n",
//    	                insurface->Info.BitDepthLuma, 
//    	                insurface->Info.BitDepthChroma, 
//    	                insurface->Info.Shift); 
//    	printf("DECODE: session=%08x, surafce=%p, length=%d flush=%d\n", 
//    	                q->session, 
//    	                insurface, 
//    	                bs.DataLength,
//    	                flush);
//
//        for(int j=0; j< 16;j++)
//                printf("%x ",bs.Data[j]);
	    do {
			ret = MFXVideoDECODE_DecodeFrameAsync(q->session, flush ? NULL : &bs, insurface, &(outsurface), &(sync) );
			if (ret != MFX_WRN_DEVICE_BUSY){
				break;
            }
	        av_usleep(500);
		} while (1);

	    //av_log(avctx, AV_LOG_ERROR, "DecodeFrameAsync return %d, offset=%d\n", ret, bs.DataOffset);

        if (MFX_WRN_VIDEO_PARAM_CHANGED==ret) {
        /* TODO: handle here minor sequence header changing */
        } else if (MFX_ERR_INCOMPATIBLE_VIDEO_PARAM==ret) {
               av_fifo_reset(q->input_fifo);
               flush = q->reinit_pending = 1;
               continue;
        } else if (MFX_ERR_UNDEFINED_BEHAVIOR == ret){
            av_log(avctx, AV_LOG_INFO, "before reset, bs.data=%p, bs.size=%u, bs.offset=%u\n",
                    bs.Data, bs.MaxLength, bs.DataOffset);
            ff_qsv_decode_reset(avctx, q);
        }

        if (sync) {
			count++;
			out_frame = find_frame( q, outsurface );
            if (!out_frame) {
                av_log(avctx, AV_LOG_ERROR,
                       "The returned surface does not correspond to any frame\n");
                return AVERROR_BUG;
            }

			out_frame->queued = 1;
			av_fifo_generic_write(q->async_fifo, &(out_frame), sizeof(out_frame), NULL);
			av_fifo_generic_write(q->async_fifo, &(sync), sizeof(sync), NULL);
            //printf("->>>>>>>>>out_frame = %p surface = %p, locked=%d count = %d\n",out_frame, outsurface, outsurface->Data.Locked,count);
            continue;
        }

        if (MFX_ERR_MORE_SURFACE != ret && ret < 0)
            break;
    }

    /* make sure we do not enter an infinite loop if the SDK
     * did not consume any data and did not return anything */
    if (!sync && !bs.DataOffset && !flush) {
        //av_log(avctx, AV_LOG_WARNING, "A decode call did not consume any data\n");
        bs.DataOffset = avpkt->size;
    }

    if (buffered) 
    {
        qsv_fifo_relocate(q->input_fifo, bs.DataOffset);
    } else if (bs.DataOffset!=avpkt->size) {
		/* some data of packet was not consumed. store it to local buffer */
		av_fifo_generic_write(q->input_fifo, avpkt->data+bs.DataOffset, avpkt->size - bs.DataOffset, NULL);
    }

    if (MFX_ERR_MORE_DATA!=ret && ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Error %d during QSV decoding.\n", ret);
        return ff_qsv_error(ret);
    }
    
    n_out_frames = av_fifo_size(q->async_fifo) / (sizeof(out_frame) + sizeof(sync));
   
    //if (n_out_frames > q->async_depth || (flush && n_out_frames) ){ 
    if( n_out_frames != 0 ){
		AVFrame *src_frame;
		av_fifo_generic_read(q->async_fifo, &(out_frame), sizeof(out_frame), NULL);
        av_fifo_generic_read(q->async_fifo, &(sync),      sizeof(sync),      NULL);
		out_frame->queued = 0;
		
        MFXVideoCORE_SyncOperation(q->session, sync, 60000);
    

		switch(q->iopattern){
		case MFX_IOPATTERN_OUT_VIDEO_MEMORY:
    	    frame->data[3] = (void*) out_frame->surface;
			break;
		case MFX_IOPATTERN_OUT_SYSTEM_MEMORY:
		default:
			src_frame = out_frame->frame;
			ret = av_frame_ref(frame, src_frame);
			if( ret < 0 ) return ret;
			break;
	    }

	    frame->width = avctx->width;
    	frame->height = avctx->height;

    	frame->format = avctx->pix_fmt;
        //printf("DECODE: %d:%s surface=%p MemId=%p\n\n",__LINE__,__func__,out_frame->surface,out_frame->surface->Data.MemId);
        frame->pkt_pts = frame->pts = out_frame->surface->Data.TimeStamp;
		//av_log(avctx, AV_LOG_INFO, "frame pts = %f, pkt dts=%f\n", out_frame->surface->Data.TimeStamp, avpkt->pts );
	

        frame->repeat_pict =
            out_frame->surface->Info.PicStruct & MFX_PICSTRUCT_FRAME_TRIPLING ? 4 :
            out_frame->surface->Info.PicStruct & MFX_PICSTRUCT_FRAME_DOUBLING ? 2 :
            out_frame->surface->Info.PicStruct & MFX_PICSTRUCT_FIELD_REPEATED ? 1 : 0;
        frame->top_field_first =
            out_frame->surface->Info.PicStruct & MFX_PICSTRUCT_FIELD_TFF;
        frame->interlaced_frame =
            !(out_frame->surface->Info.PicStruct & MFX_PICSTRUCT_PROGRESSIVE);

        *got_frame = 1;
		count--;
    }

    av_log(avctx, AV_LOG_DEBUG, "do_qsv_decode: frame=%p size=%dB got_frame=%d\n", frame, avpkt->size, *got_frame);

    return avpkt->size;
}

/*
 This function inserts a packet at fifo front.
*/
static void qsv_packet_push_front(QSVContext *q, AVPacket *avpkt)
{
    int fifo_size = av_fifo_size(q->pkt_fifo);
    if (!fifo_size) {
    /* easy case fifo is empty */
        av_fifo_generic_write(q->pkt_fifo, avpkt, sizeof(*avpkt), NULL);
    } else {
    /* realloc necessary */
    	AVPacket pkt;
    	AVFifoBuffer *fifo = av_fifo_alloc( fifo_size + av_fifo_space(q->pkt_fifo) );
    	av_fifo_generic_write( fifo, avpkt, sizeof(*avpkt), NULL );
	
    	while( av_fifo_size(q->pkt_fifo) ) {
    		av_fifo_generic_read( q->pkt_fifo, &pkt, sizeof(pkt), NULL );
    		av_fifo_generic_write( fifo, &pkt, sizeof(pkt), NULL );
     	}

    	av_fifo_free( q->pkt_fifo );
    	q->pkt_fifo = fifo;
	}
}

int ff_qsv_decode(AVCodecContext *avctx, QSVContext *q,
                  AVFrame *frame, int *got_frame,
                  AVPacket *avpkt)
{
    AVPacket pkt_ref = { 0 };
    int ret = 0;

    if (q->pkt_fifo && av_fifo_size(q->pkt_fifo) >= sizeof(AVPacket)) {
        /* we already have got some buffered packets. so add new to tail */
	ret = av_packet_ref(&pkt_ref, avpkt);
        if (ret < 0)
            return ret;
        av_fifo_generic_write(q->pkt_fifo, &pkt_ref, sizeof(pkt_ref), NULL);
    }

    if (q->reinit_pending) {
        ret = do_qsv_decode(avctx, q, frame, got_frame, avpkt);

    	if(!*got_frame) {
	    	close_decoder(q);
    	}
    }

    if(!q->reinit_pending) {
    	if( q->pkt_fifo && av_fifo_size(q->pkt_fifo) >= sizeof(AVPacket) ) {

        	while(!*got_frame && av_fifo_size(q->pkt_fifo) >= sizeof(AVPacket)) {
        		av_fifo_generic_read(q->pkt_fifo, &pkt_ref, sizeof(pkt_ref), NULL);
        		ret = do_qsv_decode( avctx, q, frame, got_frame, &pkt_ref );

         		if(q->reinit_pending) {
                    qsv_packet_push_front(q, &pkt_ref);
                } else {
                    av_packet_unref(&pkt_ref);
                }
           }
        } else {
            ret = do_qsv_decode(avctx, q, frame, got_frame, avpkt);

            if (q->reinit_pending) {
                ret = av_packet_ref(&pkt_ref, avpkt);
                if (ret < 0)
                    return ret;
                av_fifo_generic_write(q->pkt_fifo, &pkt_ref, sizeof(pkt_ref), NULL);
            }
        }
    }
	
    return ret;
}

/*
 This function resets decoder and corresponded buffers before seek operation
*/
void ff_qsv_decode_reset(AVCodecContext *avctx, QSVContext *q)
{
//    QSVFrame *cur;
    AVPacket pkt;
    int ret = 0;
    mfxVideoParam param = { { 0 } };

    av_log(avctx, AV_LOG_INFO, "%s() requested with %s\n", __FUNCTION__, 
        q->iopattern == MFX_IOPATTERN_OUT_VIDEO_MEMORY ? "vidmem" : "sysmem");
    if (q->reinit_pending) {
        close_decoder(q);
    } else if (q->engine_ready) {
        ret = MFXVideoDECODE_GetVideoParam(q->session, &param);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "MFX decode get param error %d\n", ret);
        }

        ret = MFXVideoDECODE_Reset(q->session, &param);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "MFX decode reset error %d\n", ret);
        }

        /* Free all frames*/
        /*cur = q->work_frames;
        while (cur) {
            q->work_frames = cur->next;
            av_frame_free(&cur->frame);
            av_freep(&cur);
            cur = q->work_frames;
        }*/
    }

    /* Reset output surfaces */
    if(q->async_fifo != NULL){
        av_fifo_reset(q->async_fifo);
    }

    /* Reset input packets fifo */
    while (q->pkt_fifo != NULL && av_fifo_size(q->pkt_fifo)) {
        av_fifo_generic_read(q->pkt_fifo, &pkt, sizeof(pkt), NULL);
        av_packet_unref(&pkt);
    }

    /* Reset input bitstream fifo */
    if(q->input_fifo != NULL){
        av_fifo_reset(q->input_fifo);
    }
}

int ff_qsv_decode_close(QSVContext *q)
{
	av_log(NULL, AV_LOG_INFO, "Close h264_qsv decoder now\n");

    close_decoder(q);

    q->session = NULL;

    ff_qsv_close_internal_session(&q->internal_qs);

    av_fifo_free(q->async_fifo);
    q->async_fifo = NULL;

    av_fifo_free(q->input_fifo);
    q->input_fifo = NULL;

    av_fifo_free(q->pkt_fifo);
    q->pkt_fifo = NULL;

	if( NULL != q->request ){
		av_freep(q->request);
		q->request = NULL;
	}
	if( NULL != q->response){
		av_freep(q->response);
		q->response = NULL;
	}
	
    return 0;
}


