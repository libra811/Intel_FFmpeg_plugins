#include <unistd.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/qsv.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/parseutils.h>
#include <libavutil/frame.h>
#include <libavutil/timestamp.h>
#include <libavutil/avstring.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>

#define NELEMS(a) (sizeof(a)/sizeof(a[0]))
#define find_x_by_codec_id(_x_) \
static const AVCodec *find_ ## _x_ ## _by_codec_id(enum AVCodecID codec_id)\
{\
    const char *codec_name = NULL;\
    switch (codec_id) {\
        case AV_CODEC_ID_H264:\
            codec_name = "h264_qsv";\
            break;\
        case AV_CODEC_ID_MPEG2VIDEO:\
            codec_name = "mpeg2_qsv";\
            break;\
        case AV_CODEC_ID_H265:\
            codec_name = "hevc_qsv";\
            break;\
        default:\
            break;\
    }\
\
    if (codec_name)\
        return avcodec_find_ ## _x_ ## _by_name(codec_name);\
\
    return avcodec_find_##_x_(codec_id);\
}

typedef struct io_context {
    const char *url;
    const char *codec_name;
    const char *size;
    const char *framerate;
    const AVCodec *codec;
    AVCodecContext *vid_ctx;
    AVFormatContext *fmt_ctx;
    AVFilterContext *buffer;
    int vid_idx;
    int eof;
    int64_t frame_count;
} io_context;

static io_context *in_files[2] = {NULL};
static io_context *out_file = NULL;

find_x_by_codec_id(decoder);
find_x_by_codec_id(encoder);

static int parse_opts(int argc, char **argv)
{
    int opt, cur_infile_idx = 0;
    char *codec = NULL, *size = NULL, *framerate = NULL;

    if (argc <= 1) {
        av_log(NULL, AV_LOG_ERROR, "No options specified, use -h for help.\n");
        exit(0);
    }

    while ((opt = getopt(argc, argv, "?hi:o:s:c:r:")) != -1) {
        switch (opt) {
            case 'i':
                if (cur_infile_idx >= NELEMS(in_files)) {
                    av_log(NULL, AV_LOG_ERROR, "Too many inputs.\n");
                    exit(0);
                }
                in_files[cur_infile_idx] = av_mallocz(sizeof(io_context));
                if (!in_files[cur_infile_idx]) {
                    av_log(NULL, AV_LOG_ERROR, "Out of memory.\n");
                    exit(0);
                }
                in_files[cur_infile_idx]->url = optarg;
                in_files[cur_infile_idx]->codec_name = codec;
                in_files[cur_infile_idx]->size = size;
                in_files[cur_infile_idx]->framerate = framerate;
                codec = size = framerate = NULL;
                cur_infile_idx++;
                break;
            case 'o':
                if (out_file) {
                    av_log(NULL, AV_LOG_ERROR, "Too many outputs.\n");
                    exit(0);
                }
                out_file = av_mallocz(sizeof(io_context));
                out_file->url = optarg;
                out_file->codec_name = codec;
                out_file->size = size;
                out_file->framerate = framerate;
                codec = size = framerate = NULL;
                break;
            case 's':
                if (size) {
                    av_log(NULL, AV_LOG_ERROR, "Too many -s options in one group.\n");
                    exit(0);
                }
                size = optarg;
                break;
            case 'c':
                if (size) {
                    av_log(NULL, AV_LOG_ERROR, "Too many -c options in one group.\n");
                    exit(0);
                }
                codec = optarg;
                break;
            case 'r':
                if (framerate) {
                    av_log(NULL, AV_LOG_ERROR, "Too many -r options in one group.\n");
                    exit(0);
                }
                framerate = optarg;
                break;
            default:
                av_log(NULL, AV_LOG_ERROR, "Unknown option -%c.\n", (char)opt);
            case 'h':
            case '?':
                av_log(NULL, AV_LOG_INFO, "%s usage:\n", argv[0]);
                av_log(NULL, AV_LOG_INFO, "\t-h\tShow this help.\n");
                av_log(NULL, AV_LOG_INFO, "\t-i\tInput url.\n");
                av_log(NULL, AV_LOG_INFO, "\t-o\tOutput url\n");
                av_log(NULL, AV_LOG_INFO, "\t-s\tPicture size of I/O url\n");
                av_log(NULL, AV_LOG_INFO, "\t-c\tFormat of I/O url, raw/h264/h264_qsv...\n");
                av_log(NULL, AV_LOG_INFO, "\t-r\tForce framerate for I/O stream\n");
                exit(0);
        }
    }

    if (codec || size) {
        av_log(NULL, AV_LOG_WARNING, "Unused\"%s%s%s%s\" detected.\n", codec ? "" : " -c ",
               codec ? codec : "", size ? "" : " -s ", size ? size : "");
    }

    if (!in_files[0] || !out_file) {
        av_log(NULL, AV_LOG_ERROR, "No input or output files given.\n");
        exit(0);
    }

    return 0;
}

static int open_input_file(io_context *in)
{
    AVInputFormat *iformat = NULL;
    AVDictionary *options = NULL;
    int ret;

    if (in->codec_name && strcmp(in->codec_name, "rawvideo") == 0) {
        if (!in->size) {
            av_log(NULL, AV_LOG_ERROR, "-s must be set for 'rawvideo'.\n");
            return -1;
        }

        iformat = av_find_input_format("rawvideo");
        if (!iformat) {
            av_log(NULL, AV_LOG_ERROR, "rawvideo is not supported by your FFmpeg.\n");
            return -1;
        }
        av_dict_set(&options, "pixel_format", "nv12", 0);
    }

    if (in->size)    av_dict_set(&options, "video_size", in->size, 0);
    if (in->framerate)  av_dict_set(&options, "framerate", in->framerate, 0);

    ret = avformat_open_input(&in->fmt_ctx, in->url, iformat, &options);
    av_dict_free(&options);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Open input file %s failed.\n", in->url);
        return ret;
    }

    ret = avformat_find_stream_info(in->fmt_ctx, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Parse input file %s failed.\n", in->url);
        return ret;
    }
    av_dump_format(in->fmt_ctx, 0, in->url, 0);

    in->vid_idx = av_find_best_stream(in->fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (in->vid_idx < 0) {
        av_log(NULL, AV_LOG_ERROR, "No video found in file %s.\n", in->url);
        return -1;
    }
    in->vid_ctx = in->fmt_ctx->streams[in->vid_idx]->codec;

    if (in->codec_name)
        in->codec = avcodec_find_decoder_by_name(in->codec_name);
    else
        in->codec = find_decoder_by_codec_id(in->vid_ctx->codec_id);
    if (!in->codec) {
        av_log(NULL, AV_LOG_ERROR, "No decoder found for %s.\n", in->url);
        return -1;
    }

    in->vid_ctx->refcounted_frames = 1;
    ret = avcodec_open2(in->vid_ctx, in->codec, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Open decoder failed for %s.\n", in->url);
        return ret;
    }
    if (av_stristr(in->codec->name, "_qsv"))
        in->vid_ctx->pix_fmt = AV_PIX_FMT_NV12;

    if (in->vid_ctx->framerate.num * in->vid_ctx->framerate.den == 0)
        in->vid_ctx->framerate = (AVRational){25,1};

    return 0;
}

static int configure_vidmem_pipeline(io_context *in, AVFilterGraph *graph, io_context *out)
{
    int ret = 0, i;
    AVFilterContext *vpp_ctx;

    if (!in || !out || !graph)
        return 0;

    if (av_stristr(in->codec->name, "_qsv") && av_stristr(out->codec->name, "_qsv")) {
        ret = av_qsv_pipeline_connect_codec(in->vid_ctx, out->vid_ctx, AVFILTER_VPP_ONLY);
        if (ret < 0)
            return ret;

        for (i = 0; i < graph->nb_filters; i++)
            if (0 == strcmp(graph->filters[i]->filter->name, "vpp")) {
                vpp_ctx = graph->filters[i];
                break;
            }
        if (!vpp_ctx)
            return 0;

        ret = av_qsv_pipeline_insert_vpp(in->vid_ctx, vpp_ctx);
        if (ret < 0)
            return ret;
    }

    return 0;
}

static int open_output_file(io_context *out)
{
    AVStream *st = NULL;
    int ret;

    /*Find encoder*/
    if (out->codec_name)
        out->codec = avcodec_find_encoder_by_name(out->codec_name);
    else
        out->codec = find_encoder_by_codec_id(AV_CODEC_ID_H264);
    if (!out->codec)
        return -1;

    ret = avformat_alloc_output_context2(&out->fmt_ctx, NULL, NULL, out->url);
    if (ret < 0)
        return -1;

    if (!(out->fmt_ctx->flags & AVFMT_NOFILE))
        avio_open(&out->fmt_ctx->pb, out->fmt_ctx->filename, AVIO_FLAG_WRITE);

    /*Create a video stream for this output*/
    st = avformat_new_stream(out->fmt_ctx, out->codec);
    if (!st)
        return -2;
    out->vid_ctx = st->codec;
    out->vid_idx = st->index;

    out->vid_ctx->bit_rate = 2*1024*1024;
    out->vid_ctx->width = out->buffer->inputs[0]->w;
    out->vid_ctx->height = out->buffer->inputs[0]->h;
    out->vid_ctx->pix_fmt = AV_PIX_FMT_NV12;
    out->vid_ctx->gop_size = 25;
    out->vid_ctx->framerate = out->buffer->inputs[0]->frame_rate;
    out->vid_ctx->time_base = av_inv_q(out->vid_ctx->framerate);
    if (out->fmt_ctx->oformat->flags &AVFMT_GLOBALHEADER)
        out->vid_ctx->flags |= CODEC_FLAG_GLOBAL_HEADER;

    /*
     * Configure if support video memory.
     * Doesn't matter if it fails.
     */
    configure_vidmem_pipeline(in_files[0], out->buffer->graph, out);

    ret = avcodec_open2(out->vid_ctx, out->codec, NULL);
    if (ret != 0) {
        printf("avcodec_open2() failed.\n");
        return ret;
    }

    st->time_base = out->vid_ctx->time_base;
    st->avg_frame_rate = out->vid_ctx->framerate;
    ret = avformat_write_header(out->fmt_ctx, NULL);
    if (ret < 0)
        return ret;

    av_dump_format(out->fmt_ctx, 0, out->url, 1);

    return 0;
}

static int init_filter(AVFilterGraph *graph)
{
    AVFilterContext *vpp = NULL;
    char arglist[1024];
    int ret, i;
    int w = 0, h = 0;
    AVRational r = {0, 1};

    if (out_file->size) {
        ret = av_parse_video_size(&w, &h, out_file->size);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Invalid parameter -s %s for %s.\n",
                    out_file->size, out_file->url);
            return ret;
        }
    }
    if (w * h <= 0) {
        w = in_files[0]->vid_ctx->width;
        h = in_files[0]->vid_ctx->height;
    }

    if (out_file->framerate) {
        ret = av_parse_video_rate(&r, out_file->framerate);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Invalid parameter -r %s for %s.\n",
                    out_file->framerate, out_file->url);
            return ret;
        }
    }
    if (r.num * r.den <= 0)
        r = in_files[0]->vid_ctx->framerate;

    snprintf(arglist, sizeof(arglist), "w=%d:h=%d:framerate=%d/%d:overlay_type=%d:"
            "overlay_w=128:overlay_h=72:overlay_x=10:overlay_y=10",
            w, h, r.num, r.den, in_files[1] ? 1 : 0);
    av_log(NULL, AV_LOG_INFO, "VPP parameters: %s\n", arglist);
    ret = avfilter_graph_create_filter(&vpp, avfilter_get_by_name("vpp"),
                "vpp", arglist, NULL, graph);
    if (ret < 0 || !vpp)
        return ret;

    ret = avfilter_graph_create_filter(&out_file->buffer,
            avfilter_get_by_name("buffersink"), out_file->url, NULL, NULL, graph);
    if (ret < 0 || !out_file->buffer)
        return ret;

    for (i = 0; i < NELEMS(in_files) && in_files[i]; i++) {
        io_context *in = in_files[i];
        snprintf(arglist, sizeof(arglist),
                "video_size=%dx%d:time_base=%d/%d:frame_rate=%d/%d:sar=%d/%d:pix_fmt=%d",
                in->vid_ctx->width, in->vid_ctx->height,
                in->vid_ctx->time_base.num, in->vid_ctx->time_base.den,
                in->vid_ctx->framerate.num, in->vid_ctx->framerate.den,
                in->vid_ctx->sample_aspect_ratio.num, in->vid_ctx->sample_aspect_ratio.den,
                in->vid_ctx->pix_fmt);
        ret = avfilter_graph_create_filter(&in->buffer, avfilter_get_by_name("buffer"),
                in->url, arglist, NULL, graph);
        if (ret < 0)
            return ret;

        ret = avfilter_link(in->buffer, 0, vpp, i);
        if (ret < 0)
            return ret;
    }

    ret = avfilter_link(vpp, 0, out_file->buffer, 0);
    if (ret < 0)
        return ret;

    ret = avfilter_graph_config(graph, NULL);
    if (ret < 0)
        return ret;

    return 0;
}

static int do_video_out()
{
    int ret;
    AVFrame *frame;
    int got_pkt = 0;
    AVPacket pkt = {.data = NULL};

    if (out_file->buffer) {
        av_init_packet(&pkt);
        frame = av_frame_alloc();
        ret = av_buffersink_get_frame_flags(out_file->buffer, frame,
                AV_BUFFERSINK_FLAG_NO_REQUEST);
        if (ret < 0) {
            av_frame_free(&frame);
            return ret == AVERROR(EAGAIN) ? 0 : ret;
        }

        frame->pts = av_rescale_q(frame->pts, out_file->buffer->inputs[0]->time_base,
                                 out_file->vid_ctx->time_base);
        ret = avcodec_encode_video2(out_file->vid_ctx, &pkt, frame, &got_pkt);
        if (ret < 0) {
            av_frame_free(&frame);
            av_packet_unref(&pkt);
            return ret;
        }
        av_frame_free(&frame);

        if (got_pkt) {
            pkt.stream_index = out_file->vid_idx;
            av_packet_rescale_ts(&pkt, out_file->vid_ctx->time_base,
                    out_file->fmt_ctx->streams[out_file->vid_idx]->time_base);
            ret = av_interleaved_write_frame(out_file->fmt_ctx, &pkt);
            if (ret < 0) {
                av_packet_unref(&pkt);
                return ret;
            }
            av_log(NULL, AV_LOG_INFO, "Output a pkt.\n");
            av_packet_unref(&pkt);
        }
    }

    return 0;
}

static int64_t get_prop_pts(io_context *in)
{
    return av_rescale_q(in->frame_count, av_inv_q(in->vid_ctx->framerate),
                        in->vid_ctx->time_base);
}

int main(int argc, char **argv)
{
    int ret = 0;
    AVFilterGraph *graph = NULL;
    AVFrame *frame = av_frame_alloc();
    int i = 0;
    int got_frame;
    AVPacket pkt;

    parse_opts(argc, argv);
    av_register_all();
    avcodec_register_all();
    avfilter_register_all();

    for (i = 0; i < NELEMS(in_files) && in_files[i]; i++) {
        ret = open_input_file(in_files[i]);
        if (ret < 0)
            goto failed;
    }

    graph = avfilter_graph_alloc();
    if (!graph)
        goto failed;

    ret = init_filter(graph);
    if (ret < 0)
        goto failed;

    ret = open_output_file(out_file);
    if (ret < 0)
        goto failed;

    av_init_packet(&pkt);
    do {
        for (i = 0; i < NELEMS(in_files) && in_files[i]; i++) {
            int flush = 0;
            if (in_files[i]->eof) {
                if (i != 0)
                    continue;
                else
                    goto failed;
            }

            ret = av_read_frame(in_files[i]->fmt_ctx, &pkt);
            if (ret < 0) {
                av_log(NULL, AV_LOG_INFO, "file[%d] reaches EOF.\n", i);
                flush = 1;
                pkt.data = NULL;
                pkt.size = 0;
            }

            if (!flush && pkt.stream_index != in_files[i]->vid_idx) {
                av_packet_unref(&pkt);
                continue;
            }

            if(flush)av_log(NULL, AV_LOG_WARNING, "Flushing decoder[%d].\n", i);
            ret = avcodec_decode_video2(in_files[i]->vid_ctx, frame, &got_frame, &pkt);
            if (ret < 0)
                goto failed;

            av_packet_unref(&pkt);
            if (!got_frame) {
                in_files[i]->eof = flush;
                continue;
            } else {
                if (!in_files[i]->buffer) {
                    ret = init_filter(graph);
                    if (ret < 0)
                        goto failed;

                    ret = open_output_file(out_file);
                    if (ret < 0)
                        goto failed;
                }
            }

            //frame->pts = av_frame_get_best_effort_timestamp(frame);
            frame->pts = get_prop_pts(in_files[i]);
            ret = av_buffersrc_add_frame_flags(in_files[i]->buffer, frame,
                    AV_BUFFERSRC_FLAG_PUSH);
            if (ret < 0)
                goto failed;
            av_frame_unref(frame);
            in_files[i]->frame_count ++;
            av_log(NULL, AV_LOG_INFO, "Decoder[%d] output a frame.\n", i);
        }

        ret = do_video_out();
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Do_video_out() failed with %d.\n", ret);
            goto failed;
        }
    } while (1);

failed:
    av_write_trailer(out_file->fmt_ctx);
    av_packet_unref(&pkt);
    av_frame_free(&frame);
    if (!(out_file->fmt_ctx->flags & AVFMT_NOFILE))
        avio_close(out_file->fmt_ctx->pb);
    avcodec_close(out_file->vid_ctx);
    avformat_free_context(out_file->fmt_ctx);
    av_freep(&out_file);
    avfilter_graph_free(&graph);
    for (i = 0; i < NELEMS(in_files) && in_files[i]; i++) {
        avcodec_close(in_files[i]->vid_ctx);
        avformat_close_input(&in_files[i]->fmt_ctx);
        av_freep(&in_files[i]);
    }

    return 0;
}
