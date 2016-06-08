
#include <mfx/mfxvideo.h>

#include "libavutil/common.h"
#include "libavutil/fifo.h"
#include "libavutil/opt.h"

#include "avcodec.h"
#include "internal.h"
#include "qsvdec.h"
typedef struct QSVH2645Context {
    AVClass *class;
    QSVContext qsv;

    int load_plugin;

    // the filter for converting to Annex B
    AVBitStreamFilterContext *bsf;

} QSVH2645Context;

