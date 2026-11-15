#include "thumb_gen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <setjmp.h>
#include <stdint.h>
#include <sys/resource.h>
#include <jpeglib.h>

#define THUMB_QUEUE_MAX 128
#define THUMB_VERSION   "v1"

static pthread_t s_thread;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cond = PTHREAD_COND_INITIALIZER;
typedef struct { int type; char *path; int gen; } job_t; /* type: 0=thumb 1=preview */
static job_t s_queue[THUMB_QUEUE_MAX];
static int s_head = 0, s_tail = 0;
static int s_running = 0;
static int s_stop = 0;

typedef struct { unsigned char *data; int w, h, gen; } preview_t;
#define PREVIEW_DONE_MAX 4
static preview_t s_done[PREVIEW_DONE_MAX];
static int s_done_head = 0, s_done_tail = 0;

static void make_dir_p(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, 0755);
}

int thumb_gen_query(const char *src, char *out, size_t out_len)
{
    if (!src || !out || out_len == 0) return 0;
    const char *slash = strrchr(src, '/');
    const char *base = slash ? slash + 1 : src;
    char dir[400];
    if (slash) {
        size_t n = (size_t)(slash - src);
        if (n >= sizeof(dir)) n = sizeof(dir) - 1;
        memcpy(dir, src, n);
        dir[n] = 0;
    } else {
        snprintf(dir, sizeof(dir), ".");
    }

    struct stat st;
    if (stat(src, &st) != 0) { out[0] = 0; return 0; }

    char name[300];
    snprintf(name, sizeof(name), "%s", base);
    char *dot = strrchr(name, '.');
    if (dot) *dot = 0;

    snprintf(out, out_len, "%s/.thumbs/%s_%ld_%ld_%s.jpg", dir, name,
             (long)st.st_mtime, (long)st.st_size, THUMB_VERSION);

    struct stat ts;
    return (stat(out, &ts) == 0 && S_ISREG(ts.st_mode) && ts.st_size > 0);
}

/* ------------------------------------------------------------- jpeg io -- */
struct jerr { struct jpeg_error_mgr pub; jmp_buf jb; };
static void jerr_exit(j_common_ptr c) { struct jerr *e = (struct jerr *)c->err; longjmp(e->jb, 1); }

static int decode_jpeg(const char *path, unsigned char **out, int *w, int *h)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    struct jpeg_decompress_struct cinfo;
    struct jerr jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jerr_exit;
    if (setjmp(jerr.jb)) { jpeg_destroy_decompress(&cinfo); fclose(f); return -1; }
    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, f);
    jpeg_read_header(&cinfo, TRUE);

    while (cinfo.image_width / (cinfo.scale_denom * 2) > THUMB_GEN_MAX_SIDE ||
           cinfo.image_height / (cinfo.scale_denom * 2) > THUMB_GEN_MAX_SIDE) {
        int d = cinfo.scale_denom * 2;
        if (d > 8) break;
        cinfo.scale_denom = d;
    }
    jpeg_start_decompress(&cinfo);
    int cw = cinfo.output_width, ch = cinfo.output_height, comp = cinfo.output_components;
    unsigned char *buf = (unsigned char *)malloc((size_t)cw * ch * comp);
    if (!buf) { jpeg_destroy_decompress(&cinfo); fclose(f); return -1; }
    int row = cw * comp;
    while (cinfo.output_scanline < cinfo.output_height) {
        unsigned char *p = buf + (size_t)cinfo.output_scanline * row;
        jpeg_read_scanlines(&cinfo, &p, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    fclose(f);
    *out = buf; *w = cw; *h = ch;
    return comp;
}

static int encode_jpeg(const char *path, unsigned char *rgb, int w, int h, int comp)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    struct jpeg_compress_struct cinfo;
    struct jerr jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jerr_exit;
    if (setjmp(jerr.jb)) { jpeg_destroy_compress(&cinfo); fclose(f); unlink(path); return -1; }
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, f);
    cinfo.image_width = w;
    cinfo.image_height = h;
    cinfo.input_components = comp;
    cinfo.in_color_space = (comp == 3) ? JCS_RGB : JCS_GRAYSCALE;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, 85, TRUE);
    jpeg_start_compress(&cinfo, TRUE);
    int row = w * comp;
    while (cinfo.next_scanline < cinfo.image_height) {
        unsigned char *p = rgb + (size_t)cinfo.next_scanline * row;
        jpeg_write_scanlines(&cinfo, &p, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    fclose(f);
    return 0;
}

/* nearest-neighbour scale, worker thread only */
static unsigned char *scale_rgb(unsigned char *src, int sw, int sh, int comp, int *dw, int *dh)
{
    int tw = sw, th = sh;
    if (sw >= sh && sw > THUMB_GEN_MAX_SIDE) {
        tw = THUMB_GEN_MAX_SIDE; th = sh * THUMB_GEN_MAX_SIDE / sw;
    } else if (sh > sw && sh > THUMB_GEN_MAX_SIDE) {
        th = THUMB_GEN_MAX_SIDE; tw = sw * THUMB_GEN_MAX_SIDE / sh;
    }
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    *dw = tw; *dh = th;
    if (tw == sw && th == sh) return src;

    unsigned char *dst = (unsigned char *)malloc((size_t)tw * th * comp);
    if (!dst) return NULL;
    for (int y = 0; y < th; y++) {
        int sy = y * sh / th;
        const unsigned char *srow = src + (size_t)sy * sw * comp;
        unsigned char *drow = dst + (size_t)y * tw * comp;
        for (int x = 0; x < tw; x++) {
            const unsigned char *p = srow + (size_t)(x * sw / tw) * comp;
            memcpy(drow + (size_t)x * comp, p, comp);
        }
    }
    return dst;
}

static void generate_one(const char *src)
{
    char dst[512];
    if (thumb_gen_query(src, dst, sizeof(dst))) return; /* already ok */

    unsigned char *rgb = NULL;
    int w = 0, h = 0;
    int comp = decode_jpeg(src, &rgb, &w, &h);
    if (comp <= 0 || !rgb) { fprintf(stderr, "[thumb] decode failed: %s\n", src); return; }

    int tw = 0, th = 0;
    unsigned char *scaled = scale_rgb(rgb, w, h, comp, &tw, &th);
    if (!scaled) { free(rgb); return; }

    /* ensure .thumbs dir */
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", dst);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = 0; make_dir_p(dir); }

    char tmp[560];
    snprintf(tmp, sizeof(tmp), "%s.tmp", dst);
    int ret = encode_jpeg(tmp, scaled, tw, th, comp);
    if (ret == 0 && rename(tmp, dst) == 0) {
        fprintf(stderr, "[thumb] %s -> %s (%dx%d)\n", src, dst, tw, th);
    } else {
        unlink(tmp);
        fprintf(stderr, "[thumb] encode failed: %s\n", src);
    }

    if (scaled != rgb) free(scaled);
    free(rgb);
}

static void push_preview(unsigned char *data, int w, int h, int gen)
{
    pthread_mutex_lock(&s_lock);
    int next = (s_done_tail + 1) % PREVIEW_DONE_MAX;
    if (next == s_done_head) { /* drop oldest */
        free(s_done[s_done_head].data);
        s_done_head = (s_done_head + 1) % PREVIEW_DONE_MAX;
    }
    s_done[s_done_tail].data = data;
    s_done[s_done_tail].w = w;
    s_done[s_done_tail].h = h;
    s_done[s_done_tail].gen = gen;
    s_done_tail = next;
    pthread_mutex_unlock(&s_lock);
}

static void generate_preview(const char *src, int gen)
{
    unsigned char *rgb = NULL;
    int w = 0, h = 0;
    int comp = decode_jpeg(src, &rgb, &w, &h);
    if (comp <= 0 || !rgb) { fprintf(stderr, "[thumb] preview decode failed: %s\n", src); return; }

    int tw, th;
    if ((int64_t)w * 480 > (int64_t)h * 640) { tw = 640; th = h * 640 / w; }
    else { th = 480; tw = w * 480 / h; }
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;

    uint16_t *out = (uint16_t *)malloc((size_t)tw * th * 2);
    if (!out) { free(rgb); return; }
    for (int y = 0; y < th; y++) {
        int sy = y * h / th;
        const unsigned char *srow = rgb + (size_t)sy * w * comp;
        for (int x = 0; x < tw; x++) {
            const unsigned char *p = srow + (size_t)(x * w / tw) * comp;
            if (comp == 3) out[(size_t)y * tw + x] = (uint16_t)(((p[0] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | (p[2] >> 3));
            else out[(size_t)y * tw + x] = (uint16_t)(((p[0] & 0xF8) << 8) | ((p[0] & 0xFC) << 3) | (p[0] >> 3));
        }
    }
    free(rgb);
    push_preview((unsigned char *)out, tw, th, gen);
}

static void *worker_main(void *arg)
{
    (void)arg;
    nice(10);
    for (;;) {
        job_t job = {0, NULL, 0};
        pthread_mutex_lock(&s_lock);
        while (!s_stop && s_head == s_tail) pthread_cond_wait(&s_cond, &s_lock);
        if (s_stop) { pthread_mutex_unlock(&s_lock); break; }
        job = s_queue[s_head];
        s_queue[s_head].path = NULL;
        s_head = (s_head + 1) % THUMB_QUEUE_MAX;
        pthread_mutex_unlock(&s_lock);

        if (job.path) {
            if (job.type == 1) generate_preview(job.path, job.gen);
            else generate_one(job.path);
            free(job.path);
            usleep(5000); /* rate limit: never hog both cores while UI is active */
        }
    }
    return NULL;
}

void thumb_gen_start(void)
{
    pthread_mutex_lock(&s_lock);
    if (s_running) { pthread_mutex_unlock(&s_lock); return; }
    s_stop = 0;
    s_running = 1;
    pthread_mutex_unlock(&s_lock);
    if (pthread_create(&s_thread, NULL, worker_main, NULL) != 0) {
        pthread_mutex_lock(&s_lock); s_running = 0; pthread_mutex_unlock(&s_lock);
        fprintf(stderr, "[thumb] worker create failed\n");
    } else {
        fprintf(stderr, "[thumb] worker started\n");
    }
}

void thumb_gen_stop(void)
{
    pthread_mutex_lock(&s_lock);
    if (!s_running) { pthread_mutex_unlock(&s_lock); return; }
    s_stop = 1;
    pthread_cond_broadcast(&s_cond);
    pthread_mutex_unlock(&s_lock);
    pthread_join(s_thread, NULL);
    s_running = 0;
}

static int queued_locked(const char *p, int type)
{
    for (int i = s_head; i != s_tail; i = (i + 1) % THUMB_QUEUE_MAX) {
        if (s_queue[i].path && s_queue[i].type == type && strcmp(s_queue[i].path, p) == 0) return 1;
    }
    return 0;
}

static void submit_locked(const char *src, int type, int gen)
{
    if (!src || !src[0]) return;
    if (!s_running) thumb_gen_start();
    int next = (s_tail + 1) % THUMB_QUEUE_MAX;
    if (next == s_head) return;
    if (queued_locked(src, type)) return;
    s_queue[s_tail].path = strdup(src);
    if (!s_queue[s_tail].path) return;
    s_queue[s_tail].type = type;
    s_queue[s_tail].gen = gen;
    s_tail = next;
    pthread_cond_signal(&s_cond);
}

void thumb_gen_submit(const char *src)
{
    if (!src || !src[0]) return;
    char dst[512];
    if (thumb_gen_query(src, dst, sizeof(dst))) return;
    pthread_mutex_lock(&s_lock);
    submit_locked(src, 0, 0);
    pthread_mutex_unlock(&s_lock);
}

void thumb_preview_request(const char *src, int gen)
{
    if (!src || !src[0]) return;
    pthread_mutex_lock(&s_lock);
    submit_locked(src, 1, gen);
    pthread_mutex_unlock(&s_lock);
}

int thumb_preview_poll(char **rgb565, int *w, int *h, int *gen)
{
    int got = 0;
    pthread_mutex_lock(&s_lock);
    if (s_done_head != s_done_tail) {
        preview_t r = s_done[s_done_head];
        s_done[s_done_head].data = NULL;
        s_done_head = (s_done_head + 1) % PREVIEW_DONE_MAX;
        if (rgb565) *rgb565 = (char *)r.data; else free(r.data);
        if (w) *w = r.w;
        if (h) *h = r.h;
        if (gen) *gen = r.gen;
        got = 1;
    }
    pthread_mutex_unlock(&s_lock);
    return got;
}

void thumb_preview_cancel(int gen)
{
    pthread_mutex_lock(&s_lock);
    for (int i = s_done_head; i != s_done_tail; i = (i + 1) % PREVIEW_DONE_MAX) {
        if (s_done[i].gen != gen && s_done[i].data) { free(s_done[i].data); s_done[i].data = NULL; }
    }
    pthread_mutex_unlock(&s_lock);
}

int thumb_gen_pending(void)
{
    pthread_mutex_lock(&s_lock);
    int n = (s_tail - s_head + THUMB_QUEUE_MAX) % THUMB_QUEUE_MAX;
    pthread_mutex_unlock(&s_lock);
    return n;
}

void thumb_gen_sweep(const char *dir)
{
    if (!dir) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d))) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot) continue;
        if (strcasecmp(dot, ".jpg") != 0 && strcasecmp(dot, ".jpeg") != 0) continue;
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        thumb_gen_submit(full);
        n++;
    }
    closedir(d);
    fprintf(stderr, "[thumb] sweep %s: %d photos\n", dir, n);
}
