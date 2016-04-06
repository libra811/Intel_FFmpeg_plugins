LIBAVCODEC_$MAJOR {
        global: av*;
                ff_qsv_*;
                #deprecated, remove after next bump
                audio_resample;
                audio_resample_close;
        local:  *;
};
