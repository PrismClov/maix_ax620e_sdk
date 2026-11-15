/**
 * Background thumbnail generator (P0-2).
 *
 * Decodes a full size JPEG on a worker thread, scales it to <=256px and
 * stores it as JPEG under <dir>/.thumbs/<name>_<mtime>_<size>_v1.jpg.
 * The UI thread only ever sees a tiny file that LVGL can decode in ~1ms.
 */
#ifndef THUMB_GEN_H
#define THUMB_GEN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define THUMB_GEN_MAX_SIDE 256

/* start/stop the worker thread (idempotent) */
void thumb_gen_start(void);
void thumb_gen_stop(void);

/* Build the expected thumb path for src.
 * Returns 1 if the thumb already exists, 0 otherwise (path still filled). */
int thumb_gen_query(const char *src_jpg, char *out_path, size_t out_len);

/* Queue a job. Duplicate pending paths are ignored. Never blocks. */
void thumb_gen_submit(const char *src_jpg);

/* Queue every *.jpg under dir (non recursive) that has no valid thumb. */
void thumb_gen_sweep(const char *dir);

/* number of queued/待处理 jobs (for logging) */
int thumb_gen_pending(void);

/* ---- full screen preview (decoded to RGB565 by the worker) ------------- */
/* Queue a preview decode. gen is echoed back so the caller can drop stale
 * results (e.g. the user closed the viewer or opened another photo). */
void thumb_preview_request(const char *src_jpg, int gen);

/* Poll a finished preview. Returns 1 and hands over a malloc'ed RGB565 buffer
 * (caller owns it) or 0 when nothing is ready. */
int thumb_preview_poll(char **rgb565, int *w, int *h, int *gen);

/* Drop queued/finished preview results whose generation != gen. */
void thumb_preview_cancel(int gen);

#ifdef __cplusplus
}
#endif

#endif /* THUMB_GEN_H */
