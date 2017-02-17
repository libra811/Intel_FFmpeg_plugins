#!/bin/sh
OUTDIR=$(pwd)
APPNAME=$0
EXEC=$(which ffmpeg)
EXEC_TC=${EXEC}
FILTER="vpp"
FILTER_OVERLAY="vpp=overlay_type=1"
LOG=output-$(date +%Y%m%d%H%M).log
STRADDR=http://10.239.173.81
MP2STR=()
AVCSTR=(1080p_h264_short.flv brazil_25full.264 \
        720p-1.2m-h265.mp4 \
         sango.h264 \
         test.mp4 \
         UCloud_test.mp4 \
         big_buck_bunny_1080p_H264_AAC_25fps_7200K.MP4 \
         high_20.mp4 \
         live.544444rxJdLU8x31.flv \
         _LC_QN0_non_2693756114745321291614751_SX.flv)
HEVCSTR=()
CODEC="mpeg2_qsv h264_qsv"
DRYRUN=

ECHO()
{
    echo -e $*
}

print_help()
{
    ECHO "${APPNAME} usage:"
    ECHO "    -h|--help                show this help"
    ECHO "    -o|--output [outdir]     set output direcotry [default=${OUTDIR}]"
    ECHO "    -e|--executive [file]    set the path to the executive bin [default=${EXEC}]"
    ECHO "    -l|--logfile [logfile]   set the log file [default=${LOG}]"
    ECHO "    -s:mpeg2/h264/hevc [url] set the stream url"
    ECHO "    -u|--url [server_addr]   set the stream server[default=${STRADDR}]"
    ECHO "    --enable-hevc            enable hevc encoder test"
    ECHO "    --dry-run                Run test cases without external streams"
}

err_exit()
{
    ECHO $1
    exit 0
}

check_hwaccel()
{
    version=$(${EXEC} -version | grep "ffmpeg version" | awk '{ print $3 }')
    case ${version} in
        3.2.2)
            EXEC_TC="${EXEC} -hwaccel qsv"
            FILTER="vpp_qsv"
            FILTER_OVERLAY="overlay_qsv"
            ;;
        *)
            FILTER="vpp"
            EXEC_TC=${EXEC}
            FILTER_OVERLAY="vpp=overlay_type=1"
            ;;
    esac
}

run_cmd()
{
    cmd=$1
    ECHO "===========================================================" >> ${LOG}
    ECHO "Command: ${cmd}" >> ${LOG}
    ECHO "-----------------------------------------------------------" >> ${LOG}
    eval "${cmd}" >> ${LOG} 2>&1
    [ $? -ne 0 ] && ECHO "!!!!!! Failed.!!!!!!!!" >> ${LOG}
    ECHO "===========================================================\n" >> ${LOG}
}

run_decoder()
{
    for codec in ${CODEC}
    do
        run_cmd "${EXEC} -f lavfi -i testsrc=hd720:d=4 -vf format=nv12 -f $(echo ${codec%_*} | sed 's:mpeg2:mpeg2video:g') - | ${EXEC} -c:v ${codec} -i - -f rawvideo -y ${OUTDIR}/dec_${codec}.nv12"
    done
}

run_encoder()
{
    for codec in ${CODEC}
    do
        run_cmd "${EXEC} -f lavfi -i testsrc=1280x720:d=4 -c:v ${codec} -b:v 2M -look_ahead 0 -y ${OUTDIR}/enc_${codec}.mp4"
    done
}

run_vpp()
{
    run_cmd "${EXEC} -f lavfi -i testsrc=1280x720:d=4 -vf ${FILTER}=w=960 -f rawvideo -y ${OUTDIR}/vpp_downscale_960.nv12"

    run_cmd "${EXEC} -f lavfi -i testsrc=1280x720:d=4 -vf ${FILTER}=w=1080 -f rawvideo -y ${OUTDIR}/vpp_upscale_1080.nv12"

    run_cmd "${EXEC} -f lavfi -i testsrc=1280x720:d=4 -vf ${FILTER}=framerate=23 -y ${OUTDIR}/vpp_downfrc_23.mp4"

    run_cmd "${EXEC} -f lavfi -i testsrc=1280x720:d=4 -vf ${FILTER}=framerate=30 -y ${OUTDIR}/vpp_upfrc_30.mp4"

    run_cmd "${EXEC} -f lavfi -i testsrc=1280x720:d=4 -vf ${FILTER}=cw=iw/2 -y ${OUTDIR}/vpp_crop.mp4"

    run_cmd "${EXEC} -f lavfi -i testsrc=1280x720:d=4 -vf \"testsrc=128x128[wm];[0:v]format=nv12[main];[main][wm]${FILTER_OVERLAY}\" -y ${OUTDIR}/vpp_composite.mp4"
}

run_transcoder()
{
    src_codec=$1 && shift
    str_list=($*)
    if [ ${#str_list[@]} -gt 0 ]
    then
        for codec in ${CODEC}
        do
            i=0
            for str in ${str_list[@]}
            do
                [ ! -f ${str} ] && str=${STRADDR}/${str}
                run_cmd "${EXEC_TC} -c:v ${src_codec} -i ${str} -c:v ${codec} -b:v 2M -vframes 100 -an -y ${OUTDIR}/transcode_${src_codec}_${codec}_${i}.mp4"
                run_cmd "${EXEC_TC} -c:v ${src_codec} -i ${str} -vf ${FILTER}=w=iw/2 -c:v ${codec} -b:v 2M -an -vframes 100 -y ${OUTDIR}/transcode_${src_codec}_vpp_${codec}_${i}.mp4"
                let i=${i}+1
            done
        done
    fi
}

###############################################
# Parse options
###############################################
while [ $# -gt 0 ]
do
    case $1 in
        -h|--help)
            print_help
            exit 0
            ;;
        -o|--output)
            OUTDIR=$2
            shift 2
            ;;
        -e|--executive)
            EXEC=$2
            shift 2
            ;;
        -l|--logfile)
            LOG=$2
            shift 2
            ;;
        -s*)
            case ${1#*:} in
                mpeg2) MP2STR=($2);;
                h264) AVCSTR=($2);;
                hevc) HEVCSTR=($2);;
                *) err_exit "Unknown stream spec $1"
            esac
            shift 2
            ;;
        --enable-hevc)
            CODEC=${CODEC}" hevc_qsv"
            shift 1
            ;;
        -u|--url)
            STRADDR=$2
            shift 2
            ;;
        --dry-run)
            DRYRUN="YES"
            shift 1
            ;;
        *)
            print_help
            exit 0
            ;;
    esac
done

###############################################
# Validation
###############################################
[ ! -x ${EXEC} ] && err_exit "${EXEC} is NOT execuable."
[ ! -d ${OUTDIR} ] && (mkdir -p ${OUTDIR} || err_exit "Crate output dir failed.")
[ ! -e ${LOG} ] && (touch ${LOG} || err_exit "Crate ${LOG} failed.")
check_hwaccel

ECHO "Going to run validation for ${EXEC}..."
ECHO "log file is ${LOG}, generated streams in ${OUTDIR}."

###############################################
# main loop
###############################################
if [ "x"${DRYRUN} = "xYES" ]
then
    ECHO "Testing decoder ... "
    run_decoder

    ECHO "Testing encoder ... "
    run_encoder

    ECHO "Testing vpp ..."
    run_vpp
else
    [ ${#AVCSTR[@]} -gt 0 ] && ECHO "Source h264 stream: ${AVCSTR[@]}"
    [ ${#MP2STR[@]} -gt 0 ] && ECHO "Source mpeg2 stream: ${MP2STR[@]}"
    [ ${#HEVCSTR[@]} -gt 0 ] && ECHO "Source hevc stream: ${HEVCSTR[@]}"
    ECHO "Testing video-memory pipeline ..."
    run_transcoder h264_qsv "${AVCSTR[@]}"
fi
