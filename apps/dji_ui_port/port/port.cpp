/**
 * MaixCAM2 port layer for SanKuai action camera UI (LVGL 9.4, RGB565)
 *
 * Provides:
 *   - LVGL display driver that blits RGB565 into a BGRA8888 image and
 *     pushes it to the AX VO through MaixCDK's maix::display
 *   - LVGL pointer indev backed by /dev/input/event* (evdev, ABS + BTN_TOUCH)
 *
 * Must be initialized before app_ui_create().
 */

#include "maix_basic.hpp"
#include "maix_display.hpp"
#include "maix_image.hpp"

extern "C" {
#include "lvgl/lvgl.h"
#include "../app/common/thumb_gen.h"
}

#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <dirent.h>
#include <cstring>
#include <cstdio>
#include <vector>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

using namespace maix;

static display::Display *g_disp = nullptr;
static image::Image *g_img = nullptr;
static int g_w = 0, g_h = 0;

static int g_touch_fd = -1;
static int g_touch_x = 0, g_touch_y = 0;
static bool g_touch_pressed = false;
static int g_max_x = 0, g_max_y = 0;

/* ---------------------------------------------------------------- flush -- */
static uint32_t g_frame_count = 0;
static uint32_t g_last_stat_ms = 0;

/* Official MaixCDK integration style (maix_lvgl.cpp + monitor.cpp):
 * LVGL renders ARGB8888 straight into the image that maix::display shows,
 * each flush only memcpy()'s the dirty rows. No pixel conversion at all. */
static void fb_flush(lv_display_t *drv, const lv_area_t *area, uint8_t *px_map)
{
    if (!g_img) { lv_display_flush_ready(drv); return; }

    const int32_t w = lv_area_get_width(area);
    uint32_t *src = (uint32_t *)px_map;
    uint32_t *fb = (uint32_t *)g_img->data();

    for (int32_t y = area->y1; y <= area->y2; y++) {
        if (y < 0 || y >= g_h) continue;
        int32_t x0 = area->x1 < 0 ? 0 : area->x1;
        int32_t x1 = area->x2 >= g_w ? g_w - 1 : area->x2;
        if (x1 < x0) continue;
        memcpy(fb + (size_t)y * g_w + x0,
               src + (x0 - area->x1),
               (size_t)(x1 - x0 + 1) * 4);
        src += w;
    }

    if (lv_display_flush_is_last(drv)) {
        try {
            g_disp->show(*g_img);
        } catch (...) {
        }
        g_frame_count++;
        uint32_t now = (uint32_t)time::ticks_ms();
        if (now - g_last_stat_ms >= 5000) {
            log::info("render fps: %.1f", g_frame_count * 1000.0 / (now - g_last_stat_ms));
            g_frame_count = 0;
            g_last_stat_ms = now;
        }
    }
    lv_display_flush_ready(drv);
}

/* ---------------------------------------------------------------- touch -- */
static int open_touch()
{
    DIR *d = opendir("/dev/input");
    if (!d) return -1;
    struct dirent *e;
    int fd = -1;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5) != 0) continue;
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
        int f = open(path, O_RDONLY | O_NONBLOCK);
        if (f < 0) { log::warn("open %s failed: %s", path, strerror(errno)); continue; }
        char name[64] = {0};
        ioctl(f, EVIOCGNAME(sizeof(name) - 1), name);
        unsigned long absbits[(ABS_MAX / 8) + 1] = {0};
        ioctl(f, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits);
        bool has_mt_x = (absbits[ABS_MT_POSITION_X / 8] >> (ABS_MT_POSITION_X % 8)) & 1;
        bool has_x = (absbits[ABS_X / 8] >> (ABS_X % 8)) & 1;
        log::info("input probe %s: name=%s x=%d mt_x=%d", path, name, (int)has_x, (int)has_mt_x);

        bool name_hit = strstr(name, "ts") || strstr(name, "touch") || strstr(name, "Touch") ||
                        strstr(name, "hyn") || strstr(name, "Hyn");
        if (has_mt_x || has_x || name_hit) {
            struct input_absinfo ai;
            if (ioctl(f, EVIOCGABS(ABS_MT_POSITION_X), &ai) == 0 && ai.maximum > 0) g_max_x = ai.maximum;
            else if (ioctl(f, EVIOCGABS(ABS_X), &ai) == 0) g_max_x = ai.maximum;
            if (ioctl(f, EVIOCGABS(ABS_MT_POSITION_Y), &ai) == 0 && ai.maximum > 0) g_max_y = ai.maximum;
            else if (ioctl(f, EVIOCGABS(ABS_Y), &ai) == 0) g_max_y = ai.maximum;
            log::info("touch device: %s (%s) max %d x %d", path, name, g_max_x, g_max_y);
            fd = f;
            break;
        }
        close(f);
    }
    closedir(d);
    return fd;
}

struct TouchSample { int x, y; bool pressed; };
static TouchSample g_q[32];
static int g_qhead = 0, g_qtail = 0;
static int g_last_x = 0, g_last_y = 0;
static bool g_last_pressed = false;

static void queue_push(int x, int y, bool pressed)
{
    int next = (g_qtail + 1) % 32;
    if (next == g_qhead) return; /* full: drop oldest semantics not needed */
    g_q[g_qtail].x = x;
    g_q[g_qtail].y = y;
    g_q[g_qtail].pressed = pressed;
    g_qtail = next;
}

static void read_touch()
{
    if (g_touch_fd < 0) return;
    struct input_event ev;
    bool dirty = false;
    while (read(g_touch_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        if (ev.type == EV_ABS) {
            if (ev.code == ABS_X || ev.code == ABS_MT_POSITION_X) { g_touch_x = ev.value; dirty = true; }
            else if (ev.code == ABS_Y || ev.code == ABS_MT_POSITION_Y) { g_touch_y = ev.value; dirty = true; }
            else if (ev.code == ABS_MT_TRACKING_ID) {
                if (ev.value >= 0) { g_touch_pressed = true; dirty = true; }
                else { g_touch_pressed = false; dirty = true; }
            }
        } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            g_touch_pressed = (ev.value != 0);
            dirty = true;
        }
    }
    if (dirty && (g_touch_pressed != g_last_pressed || g_touch_x != g_last_x || g_touch_y != g_last_y)) {
        queue_push(g_touch_x, g_touch_y, g_touch_pressed);
        g_last_x = g_touch_x;
        g_last_y = g_touch_y;
        g_last_pressed = g_touch_pressed;
    }
}

static void map_touch(int raw_x, int raw_y, int *out_x, int *out_y)
{
    int max_x = g_max_x > 0 ? g_max_x : 479;
    int max_y = g_max_y > 0 ? g_max_y : 639;
    int x = (int)((int64_t)raw_y * (g_w - 1) / (max_y > 0 ? max_y : 1));
    int y = (int)((int64_t)(max_x - raw_x) * (g_h - 1) / (max_x > 0 ? max_x : 1));
    if (x < 0) x = 0;
    if (x >= g_w) x = g_w - 1;
    if (y < 0) y = 0;
    if (y >= g_h) y = g_h - 1;
    *out_x = x;
    *out_y = y;
}

static void indev_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    read_touch();
    if (g_qhead != g_qtail) {
        TouchSample s = g_q[g_qhead];
        g_qhead = (g_qhead + 1) % 32;
        int x, y;
        map_touch(s.x, s.y, &x, &y);
        data->point.x = x;
        data->point.y = y;
        data->state = s.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        data->continue_reading = (g_qhead != g_qtail);
    } else {
        data->point.x = g_last_x;
        data->point.y = g_last_y;
        data->state = LV_INDEV_STATE_RELEASED;
        data->continue_reading = false;
    }
}

/* ----------------------------------------------------------------- init -- */
extern "C" int maix_port_init(int rotate)
{
    try {
        g_disp = new display::Display(-1, -1, image::FMT_BGRA8888);
    } catch (std::exception &e) {
        log::error("display init failed: %s", e.what());
        return -1;
    }
    g_w = g_disp->width();
    g_h = g_disp->height();
    g_img = new image::Image(g_w, g_h, image::FMT_BGRA8888, image::COLOR_BLACK);
    log::info("port: display %dx%d", g_w, g_h);

    lv_init();
    lv_tick_set_cb([]() -> uint32_t { return (uint32_t)time::ticks_ms(); });

    lv_display_t *disp = lv_display_create(g_w, g_h);
    size_t buf_size = (size_t)g_w * 100 * 4;
    void *buf1 = malloc(buf_size);
    void *buf2 = malloc(buf_size);
    lv_display_set_buffers(disp, buf1, buf2, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, fb_flush);
    lv_display_set_default(disp);
    if (rotate) lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90);

    /* P0-2: start the thumbnail worker and backfill existing photos once */
    thumb_gen_start();
    thumb_gen_sweep("/root/photos");

    g_touch_fd = open_touch();
    if (g_touch_fd >= 0) {
        lv_indev_t *indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, indev_read);
    }

    return 0;
}

extern "C" int maix_port_width(void) { return g_w; }
extern "C" int maix_port_height(void) { return g_h; }
