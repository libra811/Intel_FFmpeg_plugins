/*
 * Intel MediaSDK QSV based H.264 / HEVC decoder
 *
 * copyright (c) 2013 Luca Barbato
 * copyright (c) 2015 Anton Khirnov
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


#include <stdint.h>
#include <string.h>

#include <mfx/mfxvideo.h>

#include "libavutil/common.h"
#include "libavutil/fifo.h"
#include "libavutil/opt.h"

#include "avcodec.h"
#include "internal.h"
#include "qsvdec.h"
#include "h264.h"

enum LoadPlugin {
    LOAD_PLUGIN_NONE,
    LOAD_PLUGIN_HEVC_SW,
    LOAD_PLUGIN_HEVC_HW,
    LOAD_PLUGIN_DEFAULT,
};

static const char* hevc_plugins[] = {
    NULL,
    "15dd936825ad475ea34e35f3f54217a6", /*LOAD_PLUGIN_HEVC_SW*/
    "33a61c0b4c27454ca8d85dde757c6f8e", /*LOAD_PLUGIN_HEVC_HW*/
    "33a61c0b4c27454ca8d85dde757c6f8e:15dd936825ad475ea34e35f3f54217a6", /*LOAD_PLUGIN_HEVC_DEFAULT*/
};

static int is_extra(const uint8_t *buf, int buf_size)
{
    int cnt= buf[5]&0x1f;
    const uint8_t *p= buf+6;
    while(cnt--){
        int nalsize= AV_RB16(p) + 2;
        if(nalsize > buf_size - (p-buf) || p[2]!=0x67)
            return 0;
        p += nalsize;
    }
    cnt = *(p++);
    if(!cnt)
        return 0;
    while(cnt--){
        int nalsize= AV_RB16(p) + 2;
        if(nalsize > buf_size - (p-buf) || p[2]!=0x68)
            return 0;
        p += nalsize;
    }
    return 1;
}

static av_cold int qsv_decode_close(AVCodecContext *avctx)
{
    QSVH2645Context *s = avctx->priv_data;

    ff_qsv_decode_close(&s->qsv);

    av_bitstream_filter_close(s->bsf);
    if(s->avctx_internal){
        if(s->avctx_internal->extradata)
            av_freep(&s->avctx_internal->extradata);
        avcodec_free_context(&s->avctx_internal);
    }

    return 0;
}

static av_cold int qsv_decode_init(AVCodecContext *avctx)
{
    QSVH2645Context *s = avctx->priv_data;
    int ret;

    if (avctx->codec_id == AV_CODEC_ID_HEVC && s->load_plugin != LOAD_PLUGIN_NONE) {
        if (s->qsv.load_plugins[0]) {
            av_log(avctx, AV_LOG_WARNING,
                   "load_plugins is not empty, but load_plugin is not set to 'none'."
                   "The load_plugin value will be ignored.\n");
        } else {
            av_freep(&s->qsv.load_plugins);
            s->qsv.load_plugins = av_strdup(hevc_plugins[s->load_plugin]);
            if (!s->qsv.load_plugins)
                return AVERROR(ENOMEM);
        }
    }

    if (avctx->codec_id == AV_CODEC_ID_H264)
        s->bsf = av_bitstream_filter_init("h264_mp4toannexb");
    else
        s->bsf = av_bitstream_filter_init("hevc_mp4toannexb");

    if (!s->bsf) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    s->avctx_internal = avcodec_alloc_context3(NULL);
    if (avctx->extradata) {
        s->avctx_internal->extradata = av_mallocz(avctx->extradata_size + FF_INPUT_BUFFER_PADDING_SIZE);
        memcpy(s->avctx_internal->extradata, avctx->extradata,
               avctx->extradata_size);
        s->avctx_internal->extradata_size = avctx->extradata_size;
    }

    ret = ff_qsv_decode_init_session(avctx, &s->qsv);
    return ret;
fail:
    qsv_decode_close(avctx);
    return ret;
}

static int qsv_decode_frame(AVCodecContext *avctx, void *data,
                            int *got_frame, AVPacket *avpkt)
{
    QSVH2645Context *s = avctx->priv_data;
    AVFrame *frame    = data;
    int ret;
    uint8_t *p_filtered = NULL;
    int      n_filtered = 0;
    AVPacket pkt_filtered = { 0 };
    int side_size = 0;
    uint8_t *side_data = NULL;
    int need_free = 0;

    pkt_filtered = *avpkt;
    if (avpkt->size) {
        /*If avpkt->size > 3 && not begin with {0, 0, 0, 1}, we treat it as AVC mode.*/
        if(avpkt->size > 3 && AV_RB32(avpkt->data) > 1){
            /* no annex-b prefix. try to restore: */
            /*Check if there's extra_data in pkt. For AVC, SPS/PPS is stored in extra_data.*/
            if (av_packet_get_side_data(avpkt, AV_PKT_DATA_NEW_EXTRADATA, NULL)) {
                side_data = av_packet_get_side_data(avpkt, AV_PKT_DATA_NEW_EXTRADATA, &side_size);
                if(is_extra(side_data, side_size)){
                    if(s->avctx_internal->extradata){
                        av_freep(&s->avctx_internal->extradata);
                        av_bitstream_filter_close(s->bsf);
                        s->bsf = av_bitstream_filter_init("h264_mp4toannexb");
                    }
                    s->avctx_internal->extradata = av_mallocz(side_size + FF_INPUT_BUFFER_PADDING_SIZE);
                    memcpy(s->avctx_internal->extradata, side_data, side_size);
                    s->avctx_internal->extradata_size = side_size;
                }
            }

            /* Copy avctx->extradata to avctx_internal, because bsf will change extradata.*/
            if (!s->avctx_internal->extradata && avctx->extradata) {
                s->avctx_internal->extradata = av_mallocz(avctx->extradata_size + FF_INPUT_BUFFER_PADDING_SIZE);
                memcpy(s->avctx_internal->extradata, avctx->extradata,
                       avctx->extradata_size);
                s->avctx_internal->extradata_size = avctx->extradata_size;
            }
            ret = av_bitstream_filter_filter(s->bsf, s->avctx_internal, "private_spspps_buf",
                                         &p_filtered, &n_filtered,
                                         avpkt->data, avpkt->size, 0);
            if(ret >= 0){
                pkt_filtered.data = p_filtered;
                pkt_filtered.size = n_filtered;
                need_free = (ret == 1);
            }
        }
    }

    ret = ff_qsv_decode(avctx, &s->qsv, frame, got_frame, &pkt_filtered);
    if(need_free)
        av_freep(&p_filtered);

    return ret;
}

static void qsv_decode_flush(AVCodecContext *avctx)
{
    QSVH2645Context *s = avctx->priv_data;
    ff_qsv_decode_reset(avctx, &s->qsv);
}

#define OFFSET(x) offsetof(QSVH2645Context, x)
#define VD AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM

#if CONFIG_HEVC_QSV_DECODER
AVHWAccel ff_hevc_qsv_hwaccel = {
    .name           = "hevc_qsv",
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_HEVC,
    .pix_fmt        = AV_PIX_FMT_QSV,
};

static const AVOption hevc_options[] = {
    { "async_depth", "Internal parallelization depth, the higher the value the higher the latency.", OFFSET(qsv.async_depth), AV_OPT_TYPE_INT, { .i64 = ASYNC_DEPTH_DEFAULT }, 0, INT_MAX, VD },

    { "load_plugin", "A user plugin to load in an internal session", OFFSET(load_plugin), AV_OPT_TYPE_INT, { .i64 = LOAD_PLUGIN_DEFAULT }, LOAD_PLUGIN_NONE, LOAD_PLUGIN_DEFAULT, VD, "load_plugin" },
    { "none",     NULL, 0, AV_OPT_TYPE_CONST, { .i64 = LOAD_PLUGIN_NONE },    0, 0, VD, "load_plugin" },
    { "hevc_sw",  NULL, 0, AV_OPT_TYPE_CONST, { .i64 = LOAD_PLUGIN_HEVC_SW }, 0, 0, VD, "load_plugin" },
    { "hevc_hw",  NULL, 0, AV_OPT_TYPE_CONST, { .i64 = LOAD_PLUGIN_HEVC_HW }, 0, 0, VD, "load_plugin" },
    { "default",  NULL, 0, AV_OPT_TYPE_CONST, { .i64 = LOAD_PLUGIN_DEFAULT }, 0, 0, VD, "load_plugin" },


    { "load_plugins", "A :-separate list of hexadecimal plugin UIDs to load in an internal session",
        OFFSET(qsv.load_plugins), AV_OPT_TYPE_STRING, { .str = "" }, 0, 0, VD },
    { NULL },
};

static const AVClass hevc_class = {
    .class_name = "hevc_qsv",
    .item_name  = av_default_item_name,
    .option     = hevc_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_hevc_qsv_decoder = {
    .name           = "hevc_qsv",
    .long_name      = NULL_IF_CONFIG_SMALL("HEVC (Intel Quick Sync Video acceleration)"),
    .priv_data_size = sizeof(QSVH2645Context),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_HEVC,
    .init           = qsv_decode_init,
    .decode         = qsv_decode_frame,
    .flush          = qsv_decode_flush,
    .close          = qsv_decode_close,
    .capabilities   = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_DR1,
    .priv_class     = &hevc_class,
};
#endif

#if CONFIG_H264_QSV_DECODER
AVHWAccel ff_h264_qsv_hwaccel = {
    .name           = "h264_qsv",
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_H264,
    .pix_fmt        = AV_PIX_FMT_QSV,
};

static const AVOption options[] = {
    { "async_depth", "Internal parallelization depth, the higher the value the higher the latency.", OFFSET(qsv.async_depth), AV_OPT_TYPE_INT, { .i64 = ASYNC_DEPTH_DEFAULT }, 0, INT_MAX, VD },
    { "gpu_copy", "Enable gpu copy in sysmem mode [default = off]", OFFSET(qsv.internal_qs.gpu_copy), AV_OPT_TYPE_INT, { .i64 = MFX_GPUCOPY_OFF }, MFX_GPUCOPY_DEFAULT, MFX_GPUCOPY_OFF, .flags = VD, "gpu_copy" },
    { "default", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_GPUCOPY_DEFAULT }, MFX_GPUCOPY_DEFAULT, MFX_GPUCOPY_OFF, .flags = VD, "gpu_copy" },
    { "on", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_GPUCOPY_ON }, MFX_GPUCOPY_DEFAULT, MFX_GPUCOPY_OFF, .flags = VD, "gpu_copy" },
    { "off", NULL, 0, AV_OPT_TYPE_CONST, { .i64 = MFX_GPUCOPY_OFF }, MFX_GPUCOPY_DEFAULT, MFX_GPUCOPY_OFF, .flags = VD, "gpu_copy" },
    { NULL },
};

static const AVClass class = {
    .class_name = "h264_qsv",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVCodec ff_h264_qsv_decoder = {
    .name           = "h264_qsv",
    .long_name      = NULL_IF_CONFIG_SMALL("H.264 / AVC / MPEG-4 AVC / MPEG-4 part 10 (Intel Quick Sync Video acceleration)"),
    .priv_data_size = sizeof(QSVH2645Context),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_H264,
    .init           = qsv_decode_init,
    .decode         = qsv_decode_frame,
    .flush          = qsv_decode_flush,
    .close          = qsv_decode_close,
    .capabilities   = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_DR1,
    .priv_class     = &class,
};
#endif
