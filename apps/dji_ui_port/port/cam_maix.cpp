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
#include <string>
#include <atomic>
#include <thread>
#include <vector>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <unistd.h>
#include <sys/wait.h>

extern "C" {
#include "camera_hal.h"
}

using namespace maix;

struct camera_hal {
    camera::Camera *cam = nullptr;
    camera::Camera *still_cam = nullptr;
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
    try {
        /* Keep a full-resolution YVU channel alive alongside preview. AX VI
         * supports both outputs from the same sensor; this avoids the
         * stop-preview / reconfigure / restart-preview stall on every shot. */
        h->still_cam = h->cam->add_channel(2560, 1440, image::FMT_YVU420SP);
    } catch (std::exception &e) {
        log::warn("camera_hal: 2K live snapshot channel unavailable: %s", e.what());
        h->still_cam = nullptr;
    }
    log::info("camera_hal: opened %dx%d, live still=%s", h->w, h->h,
              h->still_cam ? "2560x1440 YVU420SP" : "disabled");
    return h;
}

extern "C" void camera_close(camera_hal_t *cam)
{
    if (!cam) return;
    delete cam->still_cam;
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

/* ISP 切换 sensor mode 后，AE/AWB 会重新收敛。只看固定帧数不可靠：
 * 在 2K 启动阶段，前几帧可能过曝，也可能因为场景亮度而很快发生跳变。
 * 从 Y 平面抽样计算平均亮度，等亮度连续稳定后再保存最后一帧。 */
static int y_plane_luma(const uint8_t *src, int w, int h)
{
    if (!src || w <= 0 || h <= 0) return -1;
    const size_t pixels = (size_t)w * h;
    const size_t step = pixels / 4096 > 1 ? pixels / 4096 : 1;
    uint64_t sum = 0;
    size_t count = 0;
    for (size_t i = 0; i < pixels; i += step) {
        sum += src[i];
        count++;
    }
    return count ? (int)(sum / count) : -1;
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
        image::Format still_fmt = use_rgb ? image::FMT_RGB888 : image::FMT_YVU420SP;
        /* MaixCDK/AX VI 在预览相机 close 后仍可能保留上一次的窗口状态。
         * 显式指定 sensor 全幅，避免 2560x1440 输出实际只取到预览阶段的
         * 小 ROI；构造时先不 open，使 windowing 在 VIN 配置前生效。 */
        camera::Camera cam(cw, ch, still_fmt, nullptr, cfps, 3, false);
        err::Err window_ret = cam.set_windowing({0, 0, cw, ch});
        if (window_ret != err::ERR_NONE) return -5;
        err::Err open_ret = cam.open(cw, ch, still_fmt, cfps, 3);
        if (open_ret != err::ERR_NONE) return -5;
        uint64_t t1 = time::ticks_ms();
        /* 切换 sensor 模式后 AE/AWB 从默认值重新收敛。至少丢弃 12 帧，
         * 再要求相邻帧 Y 平面平均亮度连续稳定，避免固定 sleep 或固定帧数
         * 在特定光照下仍保存到过曝帧。 */
        image::Image *yuv = nullptr;
        const int warmup_min = getenv("DJI_STILL_WARMUP")
                                   ? atoi(getenv("DJI_STILL_WARMUP")) : 12;
        const int warmup_max = getenv("DJI_STILL_WARMUP_MAX")
                                   ? atoi(getenv("DJI_STILL_WARMUP_MAX")) : 60;
        const int stable_need = getenv("DJI_STILL_STABLE_FRAMES")
                                    ? atoi(getenv("DJI_STILL_STABLE_FRAMES")) : 3;
        const int luma_tol = getenv("DJI_STILL_LUMA_TOL")
                                 ? atoi(getenv("DJI_STILL_LUMA_TOL")) : 3;
        int warmup_used = 0;
        int luma_start = -1;
        int luma_prev = -1;
        int luma_last = -1;
        int stable_frames = 0;
        for (int i = 0; i < warmup_max; i++) {
            image::Image *f = cam.read(true, 3000);
            if (!f) break;
            if (yuv) delete yuv;
            yuv = f;
            warmup_used = i + 1;

            const int luma = y_plane_luma((const uint8_t *)yuv->data(),
                                          yuv->width(), yuv->height());
            if (luma_start < 0) luma_start = luma;
            if (luma >= 0 && luma_prev >= 0 && abs(luma - luma_prev) <= luma_tol) {
                stable_frames++;
            } else {
                stable_frames = 0;
            }
            luma_prev = luma;
            luma_last = luma;
            if (warmup_used >= warmup_min && stable_frames >= stable_need) break;
        }
        uint64_t t2 = time::ticks_ms();
        if (!yuv) {
            log::warn("still: read timeout");
            return -2;
        }
        log::info("still: warmup=%d frames luma %d -> %d stable=%d/%d",
                  warmup_used, luma_start, luma_last, stable_frames, stable_need);
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

/* 预览阶段会在 MaixCDK/AX VI 驱动中留下进程级窗口状态。即使 close()
 * 后用完整 sensor 分辨率重新 open，后续 2K 帧仍可能只包含预览阶段的小 ROI。
 * 因此快门在一个全新的本进程副本中调用独立抓帧函数；子进程不初始化 LVGL，
 * 抓到照片后立即退出，父进程负责等待结果并恢复预览。 */
extern "C" int maixcam2_capture_still_jpeg_isolated(const char *path, int quality)
{
    if (!path) return -1;

    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0 || n >= (ssize_t)sizeof(exe)) return -1;
    exe[n] = '\0';

    std::vector<std::string> env_storage;
    extern char **environ;
    for (char **e = environ; e && *e; e++) env_storage.emplace_back(*e);
    env_storage.emplace_back("DJI_STILL_HELPER=1");
    env_storage.emplace_back(std::string("DJI_STILL_HELPER_PATH=") + path);
    env_storage.emplace_back(std::string("DJI_STILL_HELPER_QUALITY=") +
                             std::to_string(quality));
    std::vector<char *> envp;
    envp.reserve(env_storage.size() + 1);
    for (auto &s : env_storage) envp.push_back(const_cast<char *>(s.c_str()));
    envp.push_back(nullptr);
    char *argv[] = {exe, nullptr};

    pid_t pid = -1;
    int spawn_ret = posix_spawn(&pid, exe, nullptr, nullptr, argv, envp.data());
    if (spawn_ret != 0) {
        log::warn("still(helper): posix_spawn failed: %d", spawn_ret);
        return -1;
    }

    const uint64_t deadline = time::ticks_ms() + 20000;
    int status = 0;
    while (time::ticks_ms() < deadline) {
        pid_t ret = waitpid(pid, &status, WNOHANG);
        if (ret == pid) {
            if (WIFEXITED(status)) return WEXITSTATUS(status);
            if (WIFSIGNALED(status)) return -128 - WTERMSIG(status);
            return -1;
        }
        if (ret < 0) return -1;
        time::sleep_ms(20);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    log::warn("still(helper): timeout");
    return -6;
}

static std::atomic_bool g_live_still_busy{false};

typedef void (*live_still_done_cb_t)(const char *path, int result, void *ctx);

/* Capture one already-converged frame from the persistent 2K channel, then
 * encode it on a worker thread. Preview continues throughout the capture. */
extern "C" int maixcam2_capture_still_jpeg_from_preview_async(
    camera_hal_t *h, const char *path, int quality,
    live_still_done_cb_t done_cb, void *ctx)
{
    if (!h || !path || !h->still_cam) return -1;
    bool expected = false;
    if (!g_live_still_busy.compare_exchange_strong(expected, true)) return -2;

    image::Image *yuv = nullptr;
    try {
        yuv = h->still_cam->read(true, 1000);
    } catch (std::exception &e) {
        log::warn("still(live): read failed: %s", e.what());
    }
    if (!yuv) {
        g_live_still_busy.store(false);
        return -3;
    }

    std::string path_copy(path);
    std::thread([yuv, path_copy, quality, done_cb, ctx]() {
        int ret = -1;
        const int w = yuv->width(), h = yuv->height();
        uint64_t t0 = time::ticks_ms();
        try {
            image::Image *rgb = new image::Image(w, h, image::FMT_RGB888);
            if (!rgb) {
                ret = -3;
            } else {
                nv21_to_rgb888((const uint8_t *)yuv->data(), w, h,
                               (uint8_t *)rgb->data());
                err::Err e = rgb->save(path_copy.c_str(), quality);
                ret = e == err::ERR_NONE ? 0 : -4;
                delete rgb;
            }
        } catch (std::exception &e) {
            log::warn("still(live): save failed: %s", e.what());
        }
        delete yuv;
        log::info("still(live): %dx%d result=%d save=%llums",
                  w, h, ret, (unsigned long long)(time::ticks_ms() - t0));
        g_live_still_busy.store(false);
        if (done_cb) done_cb(path_copy.c_str(), ret, ctx);
    }).detach();
    return 0;
}
