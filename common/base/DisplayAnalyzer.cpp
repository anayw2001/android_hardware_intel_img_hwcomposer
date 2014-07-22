/*
 * Copyright © 2012 Intel Corporation
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Jackie Li <yaodong.li@intel.com>
 *
 */

#include <HwcTrace.h>
#include <IDisplayDevice.h>
#include <DisplayQuery.h>
#include <BufferManager.h>
#include <DisplayPlaneManager.h>
#include <Hwcomposer.h>
#include <DisplayAnalyzer.h>
#include <cutils/properties.h>


namespace android {
namespace intel {

DisplayAnalyzer::DisplayAnalyzer()
    : mInitialized(false),
      mEnableVideoExtendedMode(true),
      mVideoExtendedMode(false),
      mForceCloneMode(false),
      mBlankDevice(false),
      mVideoPlaying(false),
      mVideoPreparing(false),
      mOverlayAllowed(true),
      mCachedNumDisplays(0),
      mCachedDisplays(0),
      mPendingEvents(),
      mEventMutex()
{
}

DisplayAnalyzer::~DisplayAnalyzer()
{
}

bool DisplayAnalyzer::initialize()
{
    // by default video extended mode is enabled
    char prop[PROPERTY_VALUE_MAX];
    if (property_get("hwc.video.extmode.enable", prop, "1") > 0) {
        mEnableVideoExtendedMode = atoi(prop) ? true : false;
    }
    mVideoExtendedMode = false;
    mForceCloneMode = false;
    mBlankDevice = false;
    mVideoPlaying = false;
    mVideoPreparing = false;
    mOverlayAllowed = true;
    mCachedNumDisplays = 0;
    mCachedDisplays = 0;
    mPendingEvents.clear();
    mInitialized = true;
    return true;
}

void DisplayAnalyzer::deinitialize()
{
    mPendingEvents.clear();
    mInitialized = false;
}

void DisplayAnalyzer::analyzeContents(
        size_t numDisplays, hwc_display_contents_1_t** displays)
{
    // cache and use them only in this context during analysis
    mCachedNumDisplays = numDisplays;
    mCachedDisplays = displays;

    handlePendingEvents();

    if (mBlankDevice) {
        blankSecondaryDevice();
    }

    if (mEnableVideoExtendedMode) {
        detectVideoExtendedMode();
        if (mVideoExtendedMode) {
            detectTrickMode(mCachedDisplays[IDisplayDevice::DEVICE_PRIMARY]);
        }
    }

    mCachedNumDisplays = 0;
    mCachedDisplays = 0;
}

void DisplayAnalyzer::detectTrickMode(hwc_display_contents_1_t *list)
{
    if (list == NULL)
        return;

    bool detected = false;
    for (size_t i = 0; i < list->numHwLayers; i++) {
        hwc_layer_1_t *layer = &list->hwLayers[i];
        if (layer && (layer->flags & HWC_TRICK_MODE)) {
            detected = true;
            // reset the type
            layer->compositionType = HWC_FRAMEBUFFER;
            break;
        }
    }

    if (detected != mForceCloneMode) {
        list->flags |= HWC_GEOMETRY_CHANGED;
        mForceCloneMode = detected;
    }
}

void DisplayAnalyzer::detectVideoExtendedMode()
{
    if (!mVideoPlaying) {
        mVideoExtendedMode = false;
        mForceCloneMode = false;
        return;
    }

    bool geometryChanged = false;
    int activeDisplays = 0;

    hwc_display_contents_1_t *content = NULL;
    for (int i = 0; i < (int)mCachedNumDisplays; i++) {
        content = mCachedDisplays[i];
        if (content == NULL) {
            continue;
        }
        activeDisplays++;
        if (content->flags & HWC_GEOMETRY_CHANGED) {
            geometryChanged = true;
        }
    }

    if (activeDisplays <= 1) {
        mVideoExtendedMode = false;
        return;
    }

    if (geometryChanged == false) {
        // use previous analysis result
        return;
    }

    // reset status of video extended mode
    mVideoExtendedMode = false;

    // check if there is video layer in the primary device
    content = mCachedDisplays[0];
    if (content == NULL) {
        return;
    }

    uint32_t videoHandle = 0;
    bool videoLayerExist = false;
    // exclude the frame buffer target layer
    for (int j = 0; j < (int)content->numHwLayers - 1; j++) {
        videoLayerExist = isVideoLayer(content->hwLayers[j]);
        if (videoLayerExist) {
            videoHandle = (uint32_t)content->hwLayers[j].handle;
            break;
        }
    }

    if (videoLayerExist == false) {
        // no video layer is found in the primary layer
        return;
    }

    // check whether video layer exists in external device or virtual device
    // TODO: video may exist in virtual device but no in external device or vice versa
    // TODO: multiple video layers are not addressed here
    for (int i = 1; i < (int)mCachedNumDisplays; i++) {
        content = mCachedDisplays[i];
        if (content == NULL) {
            continue;
        }

        // exclude the frame buffer target layer
        for (int j = 0; j < (int)content->numHwLayers - 1; j++) {
            if ((uint32_t)content->hwLayers[j].handle == videoHandle) {
                ITRACE("video layer exists in device %d", i);
                if (!isVideoEmbedded(content->hwLayers[j])) {
                    mVideoExtendedMode = true;
                }
                return;
            }
        }
    }
}

bool DisplayAnalyzer::checkVideoExtendedMode()
{
    return mVideoExtendedMode && !mForceCloneMode;
}

bool DisplayAnalyzer::isVideoExtendedModeEnabled()
{
#if 1
    // enable it for run-time debugging purpose.
    char prop[PROPERTY_VALUE_MAX];
    if (property_get("hwc.video.extmode.enable", prop, "1") > 0) {
        mEnableVideoExtendedMode = atoi(prop) ? true : false;
    }
    ITRACE("video extended mode enabled: %d", mEnableVideoExtendedMode);
#endif

    return mEnableVideoExtendedMode;
}

bool DisplayAnalyzer::isVideoLayer(hwc_layer_1_t &layer)
{
    bool ret = false;
    BufferManager *bm = Hwcomposer::getInstance().getBufferManager();
    if (!layer.handle) {
        return false;
    }
    DataBuffer *buffer = bm->lockDataBuffer((uint32_t)layer.handle);
     if (!buffer) {
         ETRACE("failed to get buffer");
     } else {
        ret = DisplayQuery::isVideoFormat(buffer->getFormat());
        bm->unlockDataBuffer(buffer);
    }
    return ret;
}

bool DisplayAnalyzer::isVideoEmbedded(hwc_layer_1_t &layer)
{
    Drm *drm = Hwcomposer::getInstance().getDrm();
    drmModeModeInfo modeInfo;
    if (!drm->getModeInfo(IDisplayDevice::DEVICE_EXTERNAL, modeInfo)) {
        ETRACE("failed to get mode info");
        return false;
    }
    drmModeModeInfoPtr mode = &modeInfo;

    int dstW = layer.displayFrame.right - layer.displayFrame.left;
    int dstH = layer.displayFrame.bottom - layer.displayFrame.top;

    VTRACE("Src[w]:%d[h]:%d Dest[w]:%d[h]:%d Mode[w]:%d[h]:%d Trans:%d",
            layer.sourceCrop.right - layer.sourceCrop.left,
            layer.sourceCrop.bottom - layer.sourceCrop.top,
            dstW, dstH,
            mode->hdisplay, mode->vdisplay,
            layer.transform);

    bool embedded = false;
    if (dstW < mode->hdisplay - 1 &&
        dstH < mode->vdisplay - 1) {
        embedded = true;
    }

    return embedded;
}

bool DisplayAnalyzer::isVideoPlaying()
{
    return mVideoPlaying;
}

bool DisplayAnalyzer::isOverlayAllowed()
{
    return mOverlayAllowed;
}

void DisplayAnalyzer::postHotplugEvent(bool connected)
{
    // TODO: turn on primary display immeidately

    if (!connected) {
        // enable vsync on the primary device immediately
        Hwcomposer::getInstance().getVsyncManager()->resetVsyncSource();
    } else {
        // handle hotplug event (vsync switch) asynchronously
        Event e;
        e.type = HOTPLUG_EVENT;
        e.connected = connected;
        postEvent(e);
        Hwcomposer::getInstance().invalidate();
    }
}

void DisplayAnalyzer::postVideoEvent(bool preparing, bool playing)
{
    Event e;
    e.type = VIDEO_EVENT;
    e.videoEvent.preparing = preparing;
    e.videoEvent.playing = playing;
    postEvent(e);
}

void DisplayAnalyzer::postBlankEvent(bool blank)
{
    Event e;
    e.type = BLANK_EVENT;
    e.blank = blank;
    postEvent(e);
    Hwcomposer::getInstance().invalidate();
}

void DisplayAnalyzer::postEvent(Event& e)
{
    Mutex::Autolock lock(mEventMutex);
    mPendingEvents.add(e);
}

void DisplayAnalyzer::handlePendingEvents()
{
    Mutex::Autolock lock(mEventMutex);
    if (mPendingEvents.size() == 0) {
        return;
    }

    while (mPendingEvents.size() != 0) {
        Event e = mPendingEvents[0];
        mPendingEvents.removeAt(0);
        switch (e.type) {
        case HOTPLUG_EVENT:
            handleHotplugEvent(e.connected);
            break;
        case BLANK_EVENT:
            handleBlankEvent(e.blank);
            break;
        case VIDEO_EVENT:
            handleVideoEvent(e.videoEvent.preparing, e.videoEvent.playing);
            break;
        }
    }
}

void DisplayAnalyzer::handleHotplugEvent(bool connected)
{
    Hwcomposer::getInstance().getVsyncManager()->resetVsyncSource();
}

void DisplayAnalyzer::handleBlankEvent(bool blank)
{
    mBlankDevice = blank;
    // force geometry changed in the secondary device to reset layer composition type
    for (int i = 0; i < (int)mCachedNumDisplays; i++) {
        if (i == IDisplayDevice::DEVICE_PRIMARY) {
            continue;
        }
        if (mCachedDisplays[i]) {
            mCachedDisplays[i]->flags |= HWC_GEOMETRY_CHANGED;
        }
    }
    blankSecondaryDevice();
}

void DisplayAnalyzer::handleVideoEvent(bool preparing, bool playing)
{
// TODO: MDS needs to set MDS_VIDEO_PREPARED
#if 0
    if (preparing != mVideoPreparing) {
        for (int i = 0; i < (int)mCachedNumDisplays; i++) {
            if (mCachedDisplays[i]) {
                mCachedDisplays[i]->flags = HWC_GEOMETRY_CHANGED;
            }
        }
        mVideoPreparing = preparing;
        // if video is in preparing stage, overlay use is temporarily not allowed to avoid
        // scrambed RGB overlay if video is protected.
        mOverlayAllowed = !preparing;
    }
#endif
    mVideoPlaying = playing;
}

void DisplayAnalyzer::blankSecondaryDevice()
{
    hwc_display_contents_1_t *content = NULL;
    hwc_layer_1 *layer = NULL;
    for (int i = 0; i < (int)mCachedNumDisplays; i++) {
        if (i == IDisplayDevice::DEVICE_PRIMARY) {
            continue;
        }
        content = mCachedDisplays[i];
        if (content == NULL) {
            continue;
        }

        for (int j = 0; j < (int)content->numHwLayers - 1; j++) {
            layer = &content->hwLayers[j];
            if (!layer) {
                continue;
            }
            if (mBlankDevice) {
                layer->hints |= HWC_HINT_CLEAR_FB;
                layer->flags &= ~HWC_SKIP_LAYER;
                layer->compositionType = HWC_OVERLAY;
            } else {
                layer->hints &= ~HWC_HINT_CLEAR_FB;
                layer->compositionType = HWC_FRAMEBUFFER;
            }
        }
    }
}

} // namespace intel
} // namespace android

