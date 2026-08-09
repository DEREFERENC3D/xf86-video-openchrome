/*
 * Copyright © 2011 James Simmons, All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include <drm.h>

#include "xf86.h"
#include "xf86drm.h"
#include "via_driver.h"

#ifdef OPENCHROMEDRI

typedef struct via_dri2_buffer_private_rec *via_dri2_buffer_private_ptr;
struct via_dri2_buffer_private_rec {
    struct buffer_object *obj;
    int width;
    int height;
    unsigned int pitch;
    unsigned int cpp;
};

static DRI2BufferPtr
via_dri2_create_buffer(DrawablePtr pDraw, unsigned int attachment,
                       unsigned int format)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pDraw->pScreen);
    VIAPtr pVia = VIAPTR(pScrn);
    DRI2BufferPtr buffer = NULL;
    via_dri2_buffer_private_ptr priv = NULL;
    struct buffer_object *bo = NULL;
    struct drm_gem_flink flink;
    unsigned int cpp, pitch, size;
    int domain;

    /* Only colour buffers are supported. */
    switch (attachment) {
    case DRI2BufferFrontLeft:
    case DRI2BufferFrontRight:
    case DRI2BufferBackLeft:
    case DRI2BufferBackRight:
    case DRI2BufferFakeFrontLeft:
    case DRI2BufferFakeFrontRight:
        domain = TTM_PL_VRAM;
        break;
    case DRI2BufferDepth:
    case DRI2BufferStencil:
    case DRI2BufferDepthStencil:
    default:
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                    "[dri2] attachment type %u not supported\n", attachment);
        return NULL;
    }

    buffer = xnfcalloc(1, sizeof *buffer);
    priv = xnfcalloc(1, sizeof *priv);
    if (!buffer || !priv)
        goto fail;

    cpp = (format != 0) ? (format + 7) >> 3 : 4;
    pitch = ALIGN_TO(pDraw->width * cpp, 16);
    size = pitch * pDraw->height;

    bo = drm_bo_alloc(pScrn, size, 16, domain);
    if (!bo)
        goto fail;

    memset(&flink, 0, sizeof flink);
    flink.handle = bo->handle;
    if (drmIoctl(pVia->drmmode.fd, DRM_IOCTL_GEM_FLINK, &flink) < 0) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                    "[dri2] Failed to name buffer (errno %d)\n", errno);
        goto fail_bo;
    }

    buffer->attachment = attachment;
    buffer->name = flink.name;
    buffer->pitch = pitch;
    buffer->cpp = cpp;
    buffer->flags = 0;
    buffer->format = format;
    buffer->driverPrivate = priv;

    priv->obj = bo;
    priv->width = pDraw->width;
    priv->height = pDraw->height;
    priv->pitch = pitch;
    priv->cpp = cpp;

    return buffer;

fail_bo:
    drm_bo_free(pScrn, bo);
fail:
    free(buffer);
    free(priv);
    return NULL;
}

static void
via_dri2_destroy_buffer(DrawablePtr pDraw, DRI2BufferPtr buffer)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pDraw->pScreen);
    via_dri2_buffer_private_ptr priv;

    if (!buffer)
        return;

    priv = buffer->driverPrivate;
    if (priv) {
        if (priv->obj)
            drm_bo_free(pScrn, priv->obj);
        free(priv);
    }
    free(buffer);
}

static void
via_dri2_copy_region(DrawablePtr pDraw, RegionPtr pRegion,
                     DRI2BufferPtr pDst, DRI2BufferPtr pSrc)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pDraw->pScreen);
    via_dri2_buffer_private_ptr src_priv = pSrc->driverPrivate;
    via_dri2_buffer_private_ptr dst_priv = pDst->driverPrivate;
    uint8_t *src, *dst;
    BoxPtr box;
    int nbox, i;

    if (!src_priv || !src_priv->obj || !dst_priv || !dst_priv->obj)
        return;

    src = drm_bo_map(pScrn, src_priv->obj);
    dst = drm_bo_map(pScrn, dst_priv->obj);
    if (!src || !dst)
        return;

    nbox = REGION_NUM_RECTS(pRegion);
    box = REGION_RECTS(pRegion);
    for (i = 0; i < nbox; i++) {
        int x1 = box[i].x1, y1 = box[i].y1;
        int x2 = box[i].x2, y2 = box[i].y2;
        int j;

        if (x1 < 0) x1 = 0;
        if (y1 < 0) y1 = 0;
        if (x2 > pDraw->width) x2 = pDraw->width;
        if (y2 > pDraw->height) y2 = pDraw->height;

        for (j = y1; j < y2; j++) {
            memcpy(dst + j * dst_priv->pitch + x1 * dst_priv->cpp,
                   src + j * src_priv->pitch + x1 * src_priv->cpp,
                   (x2 - x1) * dst_priv->cpp);
        }
    }
}

static int
via_dri2_get_msc(DrawablePtr pDraw, CARD64 *ust, CARD64 *msc)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    *ust = (CARD64) tv.tv_sec * 1000000 + tv.tv_usec;
    *msc = 0;

    return Success;
}

static int
via_dri2_auth_magic(int fd, uint32_t magic)
{
    return drmAuthMagic(fd, magic);
}

static int
via_dri2_schedule_wait_msc(ClientPtr client, DrawablePtr pDraw,
                           CARD64 target_msc, CARD64 divisor, CARD64 remainder)
{
    /* No vblank sync support: the target MSC is considered reached at
     * once.  The extension expects us to wake the client by completing
     * the DRI2WaitMSC request; failing to do so leaves it blocked forever
     * (this hung the server after running glxgears). */
    DRI2WaitMSCComplete(client, pDraw, target_msc, 0, 0);
    return TRUE;
}

Bool
VIADRI2FinishScreenInit(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    VIAPtr pVia = VIAPTR(pScrn);

    /*
     * Legacy DRI1 did the following here, none of which applies to
     * DRI2 / KMS:
     *
     *  - CAREA (DRM_via_sarea_t / ctxOwner) is a DRI1-only construct;
     *    DRI2 clients negotiate buffers through GetBuffersInstead.
     *  - Off-screen framebuffer space was allocated with drm_bo_alloc()
     *    into pVia->driOffScreenMem.  Under DRI2/KMS the framebuffer
     *    window (visible area + VIA_KMS_EXA_OFFSCREEN_SIZE) is already
     *    a single GEM object created in VIAScreenInit(), and DRI2 draw
     *    buffers are created on demand by via_dri2_create_buffer().
     *  - The AGP DMA ring buffer (VIADRIRingBufferInit) and the
     *    userspace-installed IRQ handler (drmCtlInstHandler) belonged
     *    to DRI1's command submission path.  The kernel DRM module now
     *    owns the ring and the interrupt (via_ring_legacy_init installs
     *    both), so they must not be set up from the DDX.
     *
     * EXA submits 2D commands through the MMIO path, so agpDMA must
     * remain FALSE.  There is nothing left to initialize: the DRI2
     * extension was already registered by VIADRI2ScreenInit().
     */
    if (pVia->directRenderingType != DRI_2)
        return FALSE;

    pVia->agpDMA = FALSE;

    return TRUE;
}

Bool
VIADRI2ScreenInit(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    VIAPtr pVia = VIAPTR(pScrn);
    DRI2InfoRec dri2_info;

    memset(&dri2_info, 0, sizeof(dri2_info));

    /* version 4 enables ScheduleSwap/GetMSC/ScheduleWaitMSC */
    dri2_info.version = 4;
    dri2_info.fd = pVia->drmmode.fd;
    dri2_info.driverName = "openchrome_dri.so";
    dri2_info.deviceName = "/dev/dri/card0";

    dri2_info.CreateBuffer = via_dri2_create_buffer;
    dri2_info.DestroyBuffer = via_dri2_destroy_buffer;
    dri2_info.CopyRegion = via_dri2_copy_region;
    dri2_info.GetMSC = via_dri2_get_msc;
    dri2_info.ScheduleWaitMSC = via_dri2_schedule_wait_msc;
    dri2_info.AuthMagic = via_dri2_auth_magic;

    if (!DRI2ScreenInit(pScreen, &dri2_info)) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                    "DRI2ScreenInit failed\n");
        return FALSE;
    }

    return TRUE;
}

void
VIADRI2CloseScreen(ScreenPtr pScreen)
{
    DRI2CloseScreen(pScreen);
}

#endif /* OPENCHROMEDRI */