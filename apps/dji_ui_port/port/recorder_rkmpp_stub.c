/* RKMPP recorder backend stub for MaixCAM2 (no Rockchip MPP on AX630C).
 * Recording will fall back to the MJPEG backend. */
#include "recorder_backends.h"

recorder_handle_t * recorder_rkmpp_create(const recorder_config_t *cfg) { (void)cfg; return 0; }
void recorder_rkmpp_destroy(recorder_handle_t *rec) { (void)rec; }
int recorder_rkmpp_start(recorder_handle_t *rec) { (void)rec; return -1; }
int recorder_rkmpp_stop(recorder_handle_t *rec) { (void)rec; return -1; }
int recorder_rkmpp_feed_frame(recorder_handle_t *rec, const camera_frame_t *frame) { (void)rec; (void)frame; return -1; }
recorder_state_t recorder_rkmpp_get_state(const recorder_handle_t *rec) { (void)rec; return REC_STATE_IDLE; }
uint64_t recorder_rkmpp_get_elapsed_ms(const recorder_handle_t *rec) { (void)rec; return 0; }
int recorder_rkmpp_is_available(void) { return 0; }
int recorder_rkmpp_get_max_capability(int *w, int *h, int *fps) { (void)w; (void)h; (void)fps; return -1; }
