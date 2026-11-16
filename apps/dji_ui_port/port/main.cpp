#include "maix_basic.hpp"
#include "maix_camera.hpp"
#include "maix_image.hpp"
#include "maix_time.hpp"
#include <execinfo.h>
#include <cstdio>
#include <cstdlib>
#include <exception>

extern "C" {
#include "lvgl/lvgl.h"
}

extern "C" int maixcam2_capture_still_jpeg(const char *path, int quality);
extern "C" int maixcam2_capture_still_jpeg_isolated(const char *path, int quality);
extern "C" int maix_port_init(int rotate);
extern "C" int maix_port_screenshot(const char *path);
extern "C" void page_home_debug_goto_album(void);
extern "C" void page_album_debug_open(int idx);
extern "C" int maix_port_width(void);
extern "C" int maix_port_height(void);
extern "C" void app_ui_create(void);

static void on_terminate()
{
    void *bt[32];
    int n = backtrace(bt, 32);
    fprintf(stderr, "=== std::terminate, backtrace ===\n");
    char **syms = backtrace_symbols(bt, n);
    for (int i = 0; i < n; i++) fprintf(stderr, "%s\n", syms[i]);
    fflush(stderr);
    abort();
}

static int probe_resolutions()
{
    static const int modes[][2] = {{640,480},{1280,720},{1920,1080},{2560,1440}};
    for (auto &m : modes) {
        uint64_t t0 = maix::time::ticks_ms();
        try {
            auto fmt = getenv("DJI_PROBE_YUV") ? maix::image::FMT_YVU420SP : maix::image::FMT_RGB888;
            maix::camera::Camera cam(m[0], m[1], fmt);
            uint64_t t1 = maix::time::ticks_ms();
            maix::image::Image *img = cam.read(true, 4000);
            uint64_t t2 = maix::time::ticks_ms();
            if (img) {
                maix::log::info("[PROBE] %dx%d OK open=%llums read=%llums got=%dx%d",
                    m[0], m[1], (unsigned long long)(t1-t0), (unsigned long long)(t2-t1),
                    img->width(), img->height());
                delete img;
            } else {
                maix::log::info("[PROBE] %dx%d open ok but read timeout", m[0], m[1]);
            }
        } catch (std::exception &e) {
            maix::log::info("[PROBE] %dx%d FAIL after %llums: %s",
                m[0], m[1], (unsigned long long)(maix::time::ticks_ms()-t0), e.what());
        }
        maix::time::sleep_ms(500);
    }
    return 0;
}

int main(int argc, char *argv[])
{
    if (getenv("DJI_STILL_HELPER")) {
        const char *path = getenv("DJI_STILL_HELPER_PATH");
        const char *quality = getenv("DJI_STILL_HELPER_QUALITY");
        return maixcam2_capture_still_jpeg(path, quality ? atoi(quality) : 95);
    }
    if (getenv("DJI_STILL_ISOLATED_TEST")) {
        if (!getenv("DJI_STILL_NOPREVIEW")) {
            maix::camera::Camera prev(640, 480, maix::image::FMT_RGB888);
            maix::image::Image *f = prev.read(true, 2000);
            delete f;
        }
        return maixcam2_capture_still_jpeg_isolated(
            "/root/photos/test_isolated.jpg", 95);
    }
    if (getenv("DJI_PROBE")) return probe_resolutions();
    if (getenv("DJI_STILL_TEST")) {
        if (!getenv("DJI_STILL_NOPREVIEW")) {
            maix::camera::Camera prev(640, 480, maix::image::FMT_RGB888);
            maix::image::Image *f = prev.read(true, 2000);
            maix::log::info("[STILL] preview frame %dx%d", f ? f->width() : -1, f ? f->height() : -1);
            delete f;
        }
        int r = maixcam2_capture_still_jpeg("/root/photos/test_2k.jpg", 95);
        maix::log::info("[STILL] capture result=%d", r);
        return r;
    }
    (void)argc;
    (void)argv;
    std::set_terminate(on_terminate);
    if (maix_port_init(0) != 0) {
        return -1;
    }
    app_ui_create();

    const bool dbg_album = getenv("DJI_DEBUG_ALBUM") != nullptr;
    uint64_t dbg_t0 = (uint64_t)maix::time::ticks_ms();
    int dbg_stage = 0;
    /* frame-time statistics: p50/p95/p99/max of lv_timer_handler() */
    static uint32_t samples[600];
    static int n = 0;
    static uint32_t win_start = (uint32_t)maix::time::ticks_ms();
    while (1) {
        if (dbg_album) {
            uint64_t el = (uint64_t)maix::time::ticks_ms() - dbg_t0;
            if (dbg_stage == 0 && el > 3000) { page_home_debug_goto_album(); dbg_stage = 1; }
            else if (dbg_stage == 1 && el > 7000) { maix_port_screenshot("/tmp/dbg_album.jpg"); dbg_stage = 2; }
            else if (dbg_stage == 2 && el > 9000) { page_album_debug_open(0); dbg_stage = 3; }
            else if (dbg_stage == 3 && el > 13000) { maix_port_screenshot("/tmp/dbg_full.jpg"); dbg_stage = 4; }
        }
        uint32_t t0 = (uint32_t)maix::time::ticks_ms();
        lv_timer_handler();
        uint32_t dt = (uint32_t)maix::time::ticks_ms() - t0;
        if (n < 600) samples[n++] = dt;

        uint32_t now = (uint32_t)maix::time::ticks_ms();
        if (now - win_start >= 5000 && n > 0) {
            uint32_t tmp[600];
            memcpy(tmp, samples, sizeof(uint32_t) * n);
            for (int i = 1; i < n; i++) {
                uint32_t v = tmp[i]; int j = i - 1;
                while (j >= 0 && tmp[j] > v) { tmp[j + 1] = tmp[j]; j--; }
                tmp[j + 1] = v;
            }
            maix::log::info("[Perf] n=%d p50=%ums p95=%ums p99=%ums max=%ums",
                            n, tmp[n / 2], tmp[(int)(n * 0.95)], tmp[(int)(n * 0.99)], tmp[n - 1]);
            n = 0;
            win_start = now;
        }
        maix::time::sleep_ms(2);
    }
    return 0;
}
