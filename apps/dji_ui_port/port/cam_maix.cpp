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


/* ---------------------------------------------------------------------------
 * High resolution still capture (2K).
 *
 * RGB888 is capped by MaixCDK at <1920x1080 (strict `<`, an off-by-one in
 * maix_camera_maixcam2.cpp), so the still channel uses YVU420SP which accepts
 * the full 2560x1440 sensor mode; the frame is converted to RGB888 only for
 * the JPEG encode.
 * ------------------------------------------------------------------------- */
/* NV21 (Y + interleaved VU) -> packed RGB888, integer BT.601 */
static void nv21_to_rgb888(const uint8_t *src, int w, int h, uint8_t *dst)
{
    const uint8_t *y_plane = src;
    const uint8_t *vu = src + (size_t)w * h;
    for (int j = 0; j < h; j++) {
        const uint8_t *yrow = y_plane + (size_t)j * w;
        const uint8_t *vrow = vu + (size_t)(j / 2) * w;
        uint8_t *drow = dst + (size_t)j * w * 3;
        for (int i = 0; i < w; i++) {
            int y = yrow[i];
            int v = vrow[(i / 2) * 2] - 128;
            int u = vrow[(i / 2) * 2 + 1] - 128;
            int r = y + ((91881 * v) >> 16);
            int g = y - ((22554 * u + 46802 * v) >> 16);
            int b = y + ((116130 * u) >> 16);
            drow[i * 3 + 0] = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
            drow[i * 3 + 1] = (uint8_t)(g < 0 ? 0 : (g > 255 ? 255 : g));
            drow[i * 3 + 2] = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));
        }
    }
}

extern "C" int maixcam2_capture_still_jpeg(const char *path, int quality)
{
    if (!path) return -1;
    uint64_t t0 = time::ticks_ms();
    if (getenv("DJI_STILL_DELAY")) time::sleep_ms(atoi(getenv("DJI_STILL_DELAY")));
    try {
        int cw = getenv("DJI_STILL_W") ? atoi(getenv("DJI_STILL_W")) : 2560;
        int ch = getenv("DJI_STILL_H") ? atoi(getenv("DJI_STILL_H")) : 1440;
        bool use_rgb = getenv("DJI_STILL_RGB") != nullptr;
        int cfps = getenv("DJI_STILL_FPS") ? atoi(getenv("DJI_STILL_FPS")) : -1;
        camera::Camera cam(cw, ch, use_rgb ? image::FMT_RGB888 : image::FMT_YVU420SP, nullptr, cfps);
        uint64_t t1 = time::ticks_ms();
        /* 切换 sensor 模式后 AE/AWB 从默认值重新收敛，首帧必然过曝/偏色。
         * 丢掉前几帧，只保留收敛后的最后一帧。 */
        image::Image *yuv = nullptr;
        const int warmup = 12;
        for (int i = 0; i < warmup; i++) {
            image::Image *f = cam.read(true, 3000);
            if (!f) break;
            if (yuv) delete yuv;
            yuv = f;
        }
        uint64_t t2 = time::ticks_ms();
        if (!yuv) {
            log::warn("still: read timeout");
            return -2;
        }
        int w = yuv->width(), h = yuv->height();
        if (use_rgb) {
            err::Err re = yuv->save(path, quality);
            log::info("still(RGB): %dx%d saved=%d open=%llums read=%llums save=%llums", w, h, (int)re,
                      (unsigned long long)(t1 - t0), (unsigned long long)(t2 - t1),
                      (unsigned long long)(time::ticks_ms() - t2));
            delete yuv;
            return re == err::ERR_NONE ? 0 : -4;
        }
        if (getenv("DJI_STILL_DUMP")) {
            size_t n = yuv->data_size();
            FILE *f = fopen(getenv("DJI_STILL_DUMP") ? getenv("DJI_STILL_DUMP") : "/root/photos/test.yuv", "wb");
            if (f) { fwrite(yuv->data(), 1, n, f); fclose(f); }
            log::info("still: dumped %zu bytes for %dx%d (w*h*1.5=%d)", n, w, h, w * h * 3 / 2);
        }
        image::Image *rgb = new image::Image(w, h, image::FMT_RGB888);
        if (!rgb) { delete yuv; return -3; }
        uint64_t t3 = time::ticks_ms();
        nv21_to_rgb888((const uint8_t *)yuv->data(), w, h, (uint8_t *)rgb->data());
        uint64_t t4 = time::ticks_ms();
        delete yuv;
        err::Err e = rgb->save(path, quality);
        log::info("still: %dx%d saved=%d open=%llums read=%llums cvt=%llums save=%llums",
                  w, h, (int)e,
                  (unsigned long long)(t1 - t0), (unsigned long long)(t2 - t1),
                  (unsigned long long)(t4 - t3), (unsigned long long)(time::ticks_ms() - t4));
        delete rgb;
        return e == err::ERR_NONE ? 0 : -4;
    } catch (std::exception &e) {
        log::warn("still: %s", e.what());
        return -5;
    }
}
