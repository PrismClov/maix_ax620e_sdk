#include "maix_basic.hpp"
#include <execinfo.h>
#include <cstdio>
#include <cstdlib>
#include <exception>

extern "C" {
#include "lvgl/lvgl.h"
}

extern "C" int maix_port_init(int rotate);
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

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    std::set_terminate(on_terminate);
    if (maix_port_init(0) != 0) {
        return -1;
    }
    app_ui_create();
    /* frame-time statistics: p50/p95/p99/max of lv_timer_handler() */
    static uint32_t samples[600];
    static int n = 0;
    static uint32_t win_start = (uint32_t)maix::time::ticks_ms();
    while (1) {
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
