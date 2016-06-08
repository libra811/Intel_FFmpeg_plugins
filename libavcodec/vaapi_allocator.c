/* ****************************************************************************** *\

INTEL CORPORATION PROPRIETARY INFORMATION
This software is supplied under the terms of a license agreement or nondisclosure
agreement with Intel Corporation and may not be copied or disclosed except in
accordance with the terms of that agreement
Copyright(c) 2011-2014 Intel Corporation. All Rights Reserved.

\* ****************************************************************************** */


#include <stdio.h>
#include <assert.h>
#include <stdbool.h>
#include <va/va.h>
#include "vaapi_allocator.h"
#include "qsvdec.h"


enum {
    MFX_FOURCC_VP8_NV12    = MFX_MAKEFOURCC('V','P','8','N'),
    MFX_FOURCC_VP8_MBDATA  = MFX_MAKEFOURCC('V','P','8','M'),
    MFX_FOURCC_VP8_SEGMAP  = MFX_MAKEFOURCC('V','P','8','S'),
};

mfxStatus va_to_mfx_status(VAStatus va_res)
{
    mfxStatus mfxRes = MFX_ERR_NONE;

    switch (va_res)
    {
    case VA_STATUS_SUCCESS:
        mfxRes = MFX_ERR_NONE;
        break;
    case VA_STATUS_ERROR_ALLOCATION_FAILED:
        mfxRes = MFX_ERR_MEMORY_ALLOC;
        break;
    case VA_STATUS_ERROR_ATTR_NOT_SUPPORTED:
    case VA_STATUS_ERROR_UNSUPPORTED_PROFILE:
    case VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT:
    case VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT:
    case VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE:
    case VA_STATUS_ERROR_FLAG_NOT_SUPPORTED:
    case VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED:
        mfxRes = MFX_ERR_UNSUPPORTED;
        break;
    case VA_STATUS_ERROR_INVALID_DISPLAY:
    case VA_STATUS_ERROR_INVALID_CONFIG:
    case VA_STATUS_ERROR_INVALID_CONTEXT:
    case VA_STATUS_ERROR_INVALID_SURFACE:
    case VA_STATUS_ERROR_INVALID_BUFFER:
    case VA_STATUS_ERROR_INVALID_IMAGE:
    case VA_STATUS_ERROR_INVALID_SUBPICTURE:
        mfxRes = MFX_ERR_NOT_INITIALIZED;
        break;
    case VA_STATUS_ERROR_INVALID_PARAMETER:
        mfxRes = MFX_ERR_INVALID_VIDEO_PARAM;
    default:
        mfxRes = MFX_ERR_UNKNOWN;
        break;
    }
    return mfxRes;
}

unsigned int ConvertMfxFourccToVAFormat(mfxU32 fourcc)
{
    switch (fourcc)
    {
    case MFX_FOURCC_NV12:
        return VA_FOURCC_NV12;
    case MFX_FOURCC_YUY2:
        return VA_FOURCC_YUY2;
    case MFX_FOURCC_YV12:
        return VA_FOURCC_YV12;
    case MFX_FOURCC_RGB4:
        return VA_FOURCC_ARGB;
    case MFX_FOURCC_P8:
        return VA_FOURCC_P208;

    default:
        assert(!"unsupported fourcc");
        return 0;
    }
}

unsigned int ConvertVP8FourccToMfxFourcc(mfxU32 fourcc)
{
    switch (fourcc)
    {
    case MFX_FOURCC_VP8_NV12:
    case MFX_FOURCC_VP8_MBDATA:
        return MFX_FOURCC_NV12;
    case MFX_FOURCC_VP8_SEGMAP:
        return MFX_FOURCC_P8;

    default:
        return fourcc;
    }
}

mfxStatus CheckRequestType(mfxFrameAllocRequest *request)
{
    if ((request->Type & (MFX_MEMTYPE_VIDEO_MEMORY_DECODER_TARGET | MFX_MEMTYPE_VIDEO_MEMORY_PROCESSOR_TARGET)) != 0)
        return MFX_ERR_NONE;
    else
        return MFX_ERR_UNSUPPORTED;
}


mfxStatus frame_alloc(mfxHDL pthis, mfxFrameAllocRequest *request, mfxFrameAllocResponse *response)
{
    int i, format;
    VAStatus va_res = VA_STATUS_SUCCESS;
    VASurfaceID* surfaces;
    vaapiMemId* vaapi_mids = NULL;
    vaapiMemId* vaapi_mid = NULL;
    mfxFrameSurface1 *mfxsurface = NULL;
	QSVFrame* last_frame = NULL;
    QSVFrame* work_frames = NULL;
    VAContextID context_id;
    VABufferType codedbuf_type;

    mfxStatus mfx_res = MFX_ERR_NONE;
    mfxMemId* mids;
    VASurfaceAttrib attrib;
    mfxU16 surface_num;
    unsigned int va_fourcc = 0;
    mfxU32 fourcc = request->Info.FourCC;
    QSVContext *q = (QSVContext*)pthis;
    mfxU16 numAllocated = 0;
    bool bCreateSrfSucceeded = false;
	mfxU32 mfx_fourcc;
    int start_num = 0;
    int codedbuf_size;

    int width32;
    int height32;

	av_log( NULL, AV_LOG_INFO, "=========vaapi alloc frame==============\n");
    if (0==request || 0==response || 0==request->NumFrameSuggested)
	    return MFX_ERR_MEMORY_ALLOC;

	if(( va_fourcc != VA_FOURCC_P208 )&&(request->Type & MFX_MEMTYPE_EXTERNAL_FRAME) && (request->Type & MFX_MEMTYPE_FROM_DECODE) ){
        surface_num = q->request->NumFrameSuggested; //request->NumFrameSuggested*2;
	}else{
        surface_num = request->NumFrameSuggested;
	}
	
    memset( response, 0, sizeof(mfxFrameAllocResponse) );
    av_log(NULL, AV_LOG_INFO, "VAAPI: va_dpy =%p, surface_num=%d\n",q->internal_qs.va_display,surface_num);

    mfx_fourcc = ConvertVP8FourccToMfxFourcc( fourcc );
    va_fourcc = ConvertMfxFourccToVAFormat( mfx_fourcc );

//    if(request->Info.BitDepthLuma!=8||request->Info.BitDepthChroma!=8||
//		    request->Info.Shift||
//		    request->Info.FourCC!=MFX_FOURCC_NV12||
//		    request->Info.ChromaFormat!=MFX_CHROMAFORMAT_YUV420)
//    {
//	    printf("ERROR: unsupported surface properties.\n");
//	    return MFX_ERR_UNSUPPORTED;
//    }

    //request->Type = MFX_MEMTYPE_VIDEO_MEMORY_PROCESSOR_TARGET;
    av_log(NULL, AV_LOG_INFO, "VAAPI: request->Type=%d\n",request->Type);
    surfaces = (VASurfaceID*)calloc(surface_num, sizeof(VASurfaceID));
	mids = (mfxMemId*)calloc(surface_num, sizeof(mfxMemId));
    vaapi_mids = (vaapiMemId*)calloc( surface_num, sizeof(vaapiMemId) );

    if( NULL == surfaces || (NULL == mids) || vaapi_mids==NULL )
    {
	    av_log(NULL, AV_LOG_ERROR, "ERROR: memory allocation failed\n");
	    return MFX_ERR_MEMORY_ALLOC;
    }

    if( va_fourcc != VA_FOURCC_P208 )
    {
        av_log(NULL, AV_LOG_INFO, "VAAPI: va_fourcc != VA_FOURCC_P208\n");
 	    attrib.type = VASurfaceAttribPixelFormat;
    	attrib.flags = VA_SURFACE_ATTRIB_SETTABLE;
    	attrib.value.type = VAGenericValueTypeInteger;
    	//attrib.value.value.i = VA_FOURCC_NV12;
    	attrib.value.value.i = va_fourcc;
	    format = va_fourcc;

	    if (fourcc == MFX_FOURCC_VP8_NV12)
        {
                // special configuration for NV12 surf allocation for VP8 hybrid encoder is required
            attrib.type          = (VASurfaceAttribType)VASurfaceAttribUsageHint;
            attrib.value.value.i = VA_SURFACE_ATTRIB_USAGE_HINT_ENCODER;
        }
        else if (fourcc == MFX_FOURCC_VP8_MBDATA)
        {
                // special configuration for MB data surf allocation for VP8 hybrid encoder is required
            attrib.value.value.i = VA_FOURCC_P208;
            format               = VA_FOURCC_P208;
        }
        else if (va_fourcc == VA_FOURCC_NV12)
        {
            format = VA_RT_FORMAT_YUV420;
        }

        av_log(NULL, AV_LOG_INFO, "frame_alloc: q->internal_qs.va_display = %p, width=%d, height=%d\n",q->internal_qs.va_display, request->Info.Width, request->Info.Height);
        va_res = vaCreateSurfaces(  q->internal_qs.va_display, 
	               	    		format,
                				request->Info.Width, request->Info.Height,
                 				surfaces,
                				surface_num,
                				&attrib, 1);
     	bCreateSrfSucceeded = (va_res==VA_STATUS_SUCCESS);
        if((request->Type & MFX_MEMTYPE_EXTERNAL_FRAME) && (request->Type & MFX_MEMTYPE_FROM_DECODE) ){
			if(q->nb_surfaces > 0 ){
				last_frame = q->work_frames;
				while( NULL != last_frame->next )
					last_frame = last_frame->next;
			}
			start_num = q->nb_surfaces;
            q->nb_surfaces += surface_num;
			mfxsurface = (mfxFrameSurface1*) calloc( surface_num, sizeof(mfxFrameSurface1) );
			if(mfxsurface==NULL){
				av_log(NULL, AV_LOG_ERROR, "ERROR: VA Surfaces allocation failed\n");
				return MFX_ERR_MEMORY_ALLOC;
			} 

			work_frames = (QSVFrame*)calloc( surface_num, sizeof(QSVFrame) );
            i = 0;
			while( i < surface_num ){
                vaapi_mid = &(vaapi_mids[i]);
                vaapi_mid->m_fourcc = fourcc;
                vaapi_mid->m_surface = &(surfaces[i]);
                mids[i] = vaapi_mid;	    
                memcpy( &mfxsurface[i].Info, &(request->Info), sizeof(mfxFrameInfo) );
                mfxsurface[i].Data.MemId = vaapi_mid;
                work_frames[i].frame = av_frame_alloc();
                work_frames[i].frame->width = request->Info.Width;
                work_frames[i].frame->height = request->Info.Height;
                work_frames[i].surface = &(mfxsurface[i]);
                work_frames[i].num = i + start_num;
                if( i < ( surface_num - 1 ) ){
					work_frames[i].next = &(work_frames[i+1]);
				}else{
        			work_frames[i].next = NULL;
				}
				i++;
			} 
			if( NULL == last_frame ){
				q->work_frames = work_frames;
			}else{
				last_frame->next = &(work_frames[0]);
			}
		}
        else{
             av_log(NULL, AV_LOG_INFO, "internal frame\n");
        }
    }
    else
    {
        av_log(NULL, AV_LOG_INFO, "VAAPI: va_fourcc == VA_FOURCC_P208\n");
	    context_id = request->reserved[0];

        width32 = 32 * ((request->Info.Width + 31) >> 5);
        height32 = 32 * ((request->Info.Height + 31) >> 5);

        if (fourcc == MFX_FOURCC_VP8_SEGMAP)
        {
            codedbuf_size = request->Info.Width * request->Info.Height;
            codedbuf_type = (VABufferType)VAEncMacroblockMapBufferType;
        }
        else
        {
            codedbuf_size = ((width32 * height32) * 400LL / (16 * 16));
            codedbuf_type = VAEncCodedBufferType;
        }

        for (numAllocated = 0; numAllocated < surface_num; numAllocated++)
        {
            VABufferID coded_buf;

            va_res = vaCreateBuffer( q->internal_qs.va_display, 
                                      context_id,
                                      codedbuf_type,
                                      codedbuf_size,
                                      1,
                                      NULL,
                                      &coded_buf);
            mfx_res = va_to_mfx_status(va_res);
            if (MFX_ERR_NONE != mfx_res) break;
            surfaces[numAllocated] = coded_buf;
        }
    }

    av_log(NULL, AV_LOG_INFO, "VAAPI: %d VA surfaces have been allocated\n", surface_num);
    if( va_res == VA_STATUS_SUCCESS )
	{

		for( i=0; i<surface_num; i++ ){
			vaapi_mid = &(vaapi_mids[i]);
    	    vaapi_mid->m_fourcc = fourcc;
    	    vaapi_mid->m_surface = &(surfaces[i]);
			mids[i] = vaapi_mid;
		    //av_log(NULL, AV_LOG_INFO, "mids[%d]=%p, surface = %p\n",i,  mids[i], vaapi_mid->m_surface);
        }
		
    	response->mids = mids;
    	response->NumFrameActual = surface_num;

    } else {
    	response->mids = NULL;
    	response->NumFrameActual = 0;

    	if (VA_FOURCC_P208 != va_fourcc || fourcc==MFX_FOURCC_VP8_MBDATA)
    	{
	        if (bCreateSrfSucceeded) vaDestroySurfaces(q->internal_qs.va_display, surfaces, surface_num);	
    	}
     	else
    	{
    	    for (i=0; i<numAllocated; i++)
     		    vaDestroyBuffer(q->internal_qs.va_display, surfaces[i]);
    	}
    	if (mids)
    	{
     	    free(mids);
    	    mids = NULL;
    	}
	
    	if (vaapi_mids) { free(vaapi_mids); vaapi_mids = NULL; }
    	if (surfaces) { free(surfaces); surfaces = NULL; }

    	av_log(NULL, AV_LOG_INFO, "ERROR: VA Surfaces allocation failed\n");
    	return MFX_ERR_MEMORY_ALLOC;
    }

    return MFX_ERR_NONE;
}

mfxStatus frame_free(mfxHDL pthis, mfxFrameAllocResponse *response)
{

    vaapiMemId* vaapi_mids = NULL;
    VASurfaceID* surfaces = NULL;
    QSVContext* q;
    mfxU32 i = 0;
    bool isBitstreamMemory=false;
    //VADisplay va_dpy = (VADisplay)pthis;
	mfxU32 mfx_fourcc;
	
    if (!response) return MFX_ERR_NULL_PTR;

	av_log( NULL, AV_LOG_INFO, "=========vaapi free frame: %d==============\n", response->NumFrameActual);
    q = (QSVContext*) pthis;

    if (response->mids)
    {
        vaapi_mids = (vaapiMemId*)(response->mids[0]);
        surfaces = vaapi_mids->m_surface;
    	mfx_fourcc = ConvertVP8FourccToMfxFourcc(vaapi_mids->m_fourcc);
    	isBitstreamMemory = (MFX_FOURCC_P8==mfx_fourcc)?true:false;

    	for (i = 0; i < response->NumFrameActual; ++i)
        {
            if (MFX_FOURCC_P8 == vaapi_mids[i].m_fourcc) vaDestroyBuffer(q->internal_qs.va_display, surfaces[i]);
            else if (vaapi_mids[i].m_sys_buffer) free(vaapi_mids[i].m_sys_buffer);
        }

    	if (!isBitstreamMemory) {
    	    //vaDestroySurfaces(va_dpy, surfaces, response->NumFrameActual);
    	    vaDestroySurfaces(q->internal_qs.va_display, surfaces, response->NumFrameActual);
    	}
        free(surfaces);
    	free(vaapi_mids);
        free(response->mids);
        response->mids = NULL;
    }

    response->NumFrameActual = 0;
    return MFX_ERR_NONE;
}

mfxStatus frame_lock(mfxHDL pthis, mfxMemId mid, mfxFrameData *ptr)
{
    mfxStatus mfx_res = MFX_ERR_NONE;
    VAStatus  va_res  = VA_STATUS_SUCCESS;
    
    VACodedBufferSegment *coded_buffer_segment;
    vaapiMemId* vaapi_mid = (vaapiMemId*)mid;
    mfxU8* pBuffer = 0;
    QSVContext* q; 
    mfxU32 mfx_fourcc;
    
	if (!mid) return MFX_ERR_INVALID_HANDLE;

    mfx_fourcc = ConvertVP8FourccToMfxFourcc(vaapi_mid->m_fourcc);
    q = (QSVContext*) pthis;

    if (MFX_FOURCC_P8 == mfx_fourcc)   // bitstream processing
    {
        if (vaapi_mid->m_fourcc == MFX_FOURCC_VP8_SEGMAP)
            va_res =  vaMapBuffer(q->internal_qs.va_display, *(vaapi_mid->m_surface), (void **)(&pBuffer));
        else
            va_res =  vaMapBuffer(q->internal_qs.va_display, *(vaapi_mid->m_surface), (void **)(&coded_buffer_segment));
        mfx_res = va_to_mfx_status(va_res);

        if (MFX_ERR_NONE == mfx_res)
        {
            if (vaapi_mid->m_fourcc == MFX_FOURCC_VP8_SEGMAP)
                ptr->Y = pBuffer;
            else
                ptr->Y = (mfxU8*)coded_buffer_segment->buf;

        }
    }
    else
    {
        va_res = vaSyncSurface(q->internal_qs.va_display, *(vaapi_mid->m_surface));
        mfx_res = va_to_mfx_status(va_res);

        if (MFX_ERR_NONE == mfx_res)
        {
            va_res = vaDeriveImage(q->internal_qs.va_display, *(vaapi_mid->m_surface), &(vaapi_mid->m_image));
	    mfx_res = va_to_mfx_status(va_res);
        }

        if (MFX_ERR_NONE == mfx_res)
        {
            va_res = vaMapBuffer(q->internal_qs.va_display, vaapi_mid->m_image.buf, (void **) &pBuffer);
            mfx_res = va_to_mfx_status(va_res);
        }

        if (MFX_ERR_NONE == mfx_res)
        {
        switch (vaapi_mid->m_image.format.fourcc)
        {
            case VA_FOURCC_NV12:
                if (mfx_fourcc == MFX_FOURCC_NV12)
                {
                    ptr->Pitch = (mfxU16)vaapi_mid->m_image.pitches[0];
                    ptr->Y = pBuffer + vaapi_mid->m_image.offsets[0];
                    ptr->U = pBuffer + vaapi_mid->m_image.offsets[1];
                    ptr->V = ptr->U + 1;
                }
                else mfx_res = MFX_ERR_LOCK_MEMORY;
                break;
            case VA_FOURCC_YV12:
                if (mfx_fourcc == MFX_FOURCC_YV12)
                {
                    ptr->Pitch = (mfxU16)vaapi_mid->m_image.pitches[0];
                    ptr->Y = pBuffer + vaapi_mid->m_image.offsets[0];
                    ptr->V = pBuffer + vaapi_mid->m_image.offsets[1];
                    ptr->U = pBuffer + vaapi_mid->m_image.offsets[2];
                }
                else mfx_res = MFX_ERR_LOCK_MEMORY;
                break;
            case VA_FOURCC_YUY2:
                if (mfx_fourcc == MFX_FOURCC_YUY2)
                {
                    ptr->Pitch = (mfxU16)vaapi_mid->m_image.pitches[0];
                    ptr->Y = pBuffer + vaapi_mid->m_image.offsets[0];
                    ptr->U = ptr->Y + 1;
                    ptr->V = ptr->Y + 3;
                }
                else mfx_res = MFX_ERR_LOCK_MEMORY;
                break;
            case VA_FOURCC_ARGB:
                if (mfx_fourcc == MFX_FOURCC_RGB4)
                {
                    ptr->Pitch = (mfxU16)vaapi_mid->m_image.pitches[0];
                    ptr->B = pBuffer + vaapi_mid->m_image.offsets[0];
                    ptr->G = ptr->B + 1;
                    ptr->R = ptr->B + 2;
                    ptr->A = ptr->B + 3;
                }
                else mfx_res = MFX_ERR_LOCK_MEMORY;
                break;
            case VA_FOURCC_P208:
                if (mfx_fourcc == MFX_FOURCC_NV12)
                {
                    ptr->Pitch = (mfxU16)vaapi_mid->m_image.pitches[0];
                    ptr->Y = pBuffer + vaapi_mid->m_image.offsets[0];
                }
                else mfx_res = MFX_ERR_LOCK_MEMORY;
                break;
            default:
                mfx_res = MFX_ERR_LOCK_MEMORY;
                break;
	    }
        }
    }
    return mfx_res;
}

mfxStatus frame_unlock(mfxHDL pthis, mfxMemId mid, mfxFrameData *ptr)
{
    QSVContext* q;
    mfxU32 mfx_fourcc;

    vaapiMemId* vaapi_mid = (vaapiMemId*)mid;
    q = (QSVContext*) pthis;

    if (!vaapi_mid || !(vaapi_mid->m_surface)) return MFX_ERR_INVALID_HANDLE;

    mfx_fourcc = ConvertVP8FourccToMfxFourcc(vaapi_mid->m_fourcc);

    if( mfx_fourcc == MFX_FOURCC_P8 )
    {
	vaUnmapBuffer(q->internal_qs.va_display, *(vaapi_mid->m_surface));
    }
    else
    {

        vaUnmapBuffer(q->internal_qs.va_display, vaapi_mid->m_image.buf);
        vaDestroyImage(q->internal_qs.va_display, vaapi_mid->m_image.image_id);

        if (NULL != ptr)
        {
        ptr->Pitch = 0;
        ptr->Y     = NULL;
        ptr->U     = NULL;
        ptr->V     = NULL;
        ptr->A     = NULL;
        }
    }
    return MFX_ERR_NONE;
}

mfxStatus frame_get_hdl(mfxHDL pthis, mfxMemId mid, mfxHDL *handle)
{
    	vaapiMemId* vaapi_mid = (vaapiMemId*)mid;

	if ( !handle || !mid ) return MFX_ERR_INVALID_HANDLE;

    *handle = (mfxHDL) vaapi_mid->m_surface; //VASurfaceID* <-> mfxHDL
    return MFX_ERR_NONE;
}
