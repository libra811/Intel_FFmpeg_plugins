LIBAVCODEC_$MAJOR {
        global: av*;
                #deprecated, remove after next bump
                audio_resample;
                audio_resample_close;
                ff_qsv_*;
        local:  *;
};
