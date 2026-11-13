/**
 * camera_hal backend for MaixCAM2 (AX630C) using MaixCDK maix::camera.
 *
 * Frames are delivered as RGB565 (CAM_FOURCC_RGB565) because the SanKuai UI
 * preview pipeline expects RGB565 (LV_COLOR_DEPTH 16).
 */

#include "maix_basic.hpp"
#include "maix_camera.hpp"
#include "maix_image.hpp"

#include <cstring>
#include <cstdlib>
#include <vector>

extern "C" {
#include "camera_hal.h"
}

using namespace maix;

struct camera_hal {
    camera::Camera *cam = nullptr;
    int w = 0, h = 0, fps = 30;
    image::Image *cur = nullptr;
    uint16_t *rgb565_buf = nullptr;
    int rgb565_w = 0, rgb565_h = 0;
};

extern "C" camera_hal_t *camera_open_default(int width, int height, int fps)
{
    /* The UI asks for 1080p; on this 512MB board the ISP init for 1080p RGB888
     * can block forever, so clamp to a resolution we know is stable. */
    if (width > 1280 || height > 720 || width <= 0 || height <= 0) {
        width = 640;
        height = 480;
    }
    camera_hal_t *h = (camera_hal_t *)calloc(1, sizeof(camera_hal_t));
    if (!h) return nullptr;
    try {
        h->cam = new camera::Camera(width, height, image::FMT_RGB888, nullptr, fps > 0 ? fps : -1);
    } catch (std::exception &e) {
        log::warn("camera %dx%d failed (%s), retry 640x480", width, height, e.what());
        try {
            h->cam = new camera::Camera(640, 480, image::FMT_RGB888);
        } catch (...) {
            free(h);
            return nullptr;
        }
    }
    h->w = h->cam->width();
    h->h = h->cam->height();
    h->fps = fps > 0 ? fps : 30;
    log::info("camera_hal: opened %dx%d", h->w, h->h);
    return h;
}

extern "C" void camera_close(camera_hal_t *cam)
{
    if (!cam) return;
    delete cam->cam;
    free(cam->rgb565_buf);
    free(cam);
}

extern "C" int camera_get_format(camera_hal_t *cam, camera_format_t *fmt)
{
    if (!cam || !fmt) return -1;
    fmt->width = cam->w;
    fmt->height = cam->h;
    fmt->fourcc = CAM_FOURCC_RGB565;
    fmt->fps = cam->fps;
    return 0;
}

extern "C" int camera_start(camera_hal_t *cam) { return cam ? 0 : -1; }
extern "C" int camera_stop(camera_hal_t *cam) { return cam ? 0 : -1; }

extern "C" int camera_acquire_frame(camera_hal_t *cam, camera_frame_t *frame)
{
    if (!cam || !frame) return -1;
    image::Image *img = nullptr;
    try {
        img = cam->cam->read(true, 1000);
    } catch (...) {
        return -1;
    }
    if (!img) return -1;

    /* MaixCDK image conversion does not implement RGB565, do it manually. */
    int w = img->width(), h = img->height();
    if (!cam->rgb565_buf) {
        cam->rgb565_buf = (uint16_t *)malloc((size_t)w * h * 2);
        cam->rgb565_w = w;
        cam->rgb565_h = h;
    }
    if (cam->rgb565_w != w || cam->rgb565_h != h) {
        free(cam->rgb565_buf);
        cam->rgb565_buf = (uint16_t *)malloc((size_t)w * h * 2);
        cam->rgb565_w = w;
        cam->rgb565_h = h;
    }
    const uint8_t *src = (const uint8_t *)img->data();
    uint16_t *dst = cam->rgb565_buf;
    for (int i = 0; i < w * h; i++) {
        uint8_t r = src[i * 3 + 0], g = src[i * 3 + 1], b = src[i * 3 + 2];
        dst[i] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    }
    delete img;

    memset(frame, 0, sizeof(*frame));
    frame->data = (const uint8_t *)cam->rgb565_buf;
    frame->size = (size_t)cam->rgb565_w * cam->rgb565_h * 2;
    frame->fourcc = CAM_FOURCC_RGB565;
    frame->width = cam->rgb565_w;
    frame->height = cam->rgb565_h;
    frame->timestamp_us = time::ticks_us();
    frame->internal = nullptr;
    return 0;
}

extern "C" int camera_release_frame(camera_hal_t *cam, camera_frame_t *frame)
{
    (void)cam;
    (void)frame;
    return 0; /* keep last frame alive, released on next acquire */
}

extern "C" int camera_set_controls(camera_hal_t *cam, const cam_control_t *ctrl)
{
    (void)cam;
    (void)ctrl;
    return 0; /* ISP auto exposure by default */
}

/* ------------------------------------------------------------ converters -- */
static uint16_t *crop_src_rgb565(const camera_frame_t *f, int ratio, int *ow, int *oh)
{
    /* ratio: 0=4:3, 1=16:9, 2=1:1, <0 = no crop */
    if (ratio < 0) {
        *ow = f->width;
        *oh = f->height;
        return nullptr;
    }
    int tw = f->width, th = f->height;
    if (ratio == 1) tw = f->height * 16 / 9;
    else if (ratio == 2) tw = f->height;
    if (tw > f->width) tw = f->width;
    *ow = tw;
    *oh = th;
    return nullptr; /* handled by caller via offsets */
}

extern "C" int camera_frame_to_rgb565(const camera_frame_t *frame, uint16_t *dst,
                                      int dst_w, int dst_h, camera_rotate_t rotate)
{
    return camera_frame_to_rgb565_crop(frame, dst, dst_w, dst_h, rotate, -1);
}

extern "C" int camera_frame_to_rgb565_crop(const camera_frame_t *frame, uint16_t *dst,
                                           int dst_w, int dst_h, camera_rotate_t rotate,
                                           int crop_ratio)
{
    if (!frame || !dst || frame->fourcc != CAM_FOURCC_RGB565) return -1;

    int sw = frame->width, sh = frame->height;
    const uint16_t *src = (const uint16_t *)frame->data;

    int cw = sw, ch = sh;
    int cx = 0, cy = 0;
    if (crop_ratio >= 0) {
        if (crop_ratio == 1) cw = sh * 16 / 9;      /* 16:9 */
        else if (crop_ratio == 2) cw = sh;          /* 1:1 */
        if (cw > sw) cw = sw;
        ch = sh;
        cx = (sw - cw) / 2;
        cy = 0;
    }

    bool rot90 = (rotate == CAM_ROTATE_90 || rotate == CAM_ROTATE_270);

    for (int y = 0; y < dst_h; y++) {
        for (int x = 0; x < dst_w; x++) {
            int sx, sy;
            if (!rot90) {
                sx = cx + x * cw / dst_w;
                sy = cy + y * ch / dst_h;
            } else {
                /* portrait output: sample transposed */
                sx = cx + y * cw / dst_h;
                sy = cy + x * ch / dst_w;
            }
            if (sx < 0) sx = 0;
            if (sx >= sw) sx = sw - 1;
            if (sy < 0) sy = 0;
            if (sy >= sh) sy = sh - 1;
            uint16_t c = src[sy * sw + sx];
            if (rotate == CAM_ROTATE_180) { /* not used, placeholder */ }
            dst[y * dst_w + x] = c;
        }
    }
    return 0;
}

extern "C" const char *camera_fourcc_name(uint32_t fourcc)
{
    switch (fourcc) {
        case CAM_FOURCC_RGB565: return "RGB565";
        case CAM_FOURCC_NV12: return "NV12";
        case CAM_FOURCC_NV21: return "NV21";
        case CAM_FOURCC_YUYV: return "YUYV";
        case CAM_FOURCC_MJPEG: return "MJPEG";
        default: return "UNKNOWN";
    }
}
