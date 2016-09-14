#ifndef __VAAPI_ALLOCATOR__
#define __VAPPI_ALLOCATOR__

#include <mfx/mfxvideo.h>
#include <va/va.h>


typedef struct __vaapiMemId__
{
    VASurfaceID* m_surface;
    VAImage	 m_image;
    unsigned int m_fourcc;
    mfxU8*	 m_sys_buffer;
    mfxU8*	 m_va_buffer;
} vaapiMemId;

mfxStatus va_to_mfx_status(VAStatus va_res);
unsigned int ConvertMfxFourccToVAFormat(mfxU32 fourcc);
unsigned int ConvertVP8FourccToMfxFourcc(mfxU32 fourcc);
mfxStatus CheckRequestType(mfxFrameAllocRequest *request);
mfxStatus ff_qsv_frame_alloc(mfxHDL pthis, mfxFrameAllocRequest *request, mfxFrameAllocResponse *response);
mfxStatus ff_qsv_frame_free(mfxHDL pthis, mfxFrameAllocResponse *response);
mfxStatus ff_qsv_frame_lock(mfxHDL pthis, mfxMemId mid, mfxFrameData *ptr);
mfxStatus ff_qsv_frame_unlock(mfxHDL pthis, mfxMemId mid, mfxFrameData *ptr);
mfxStatus ff_qsv_frame_get_hdl(mfxHDL pthis, mfxMemId mid, mfxHDL *handle);

#endif
