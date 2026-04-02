// vita_vtc.h
// HD texture cache (VTC format) API
#ifndef VITA_VTC_H
#define VITA_VTC_H

#ifdef TARGET_VITA

#include "pc_platform.h" // GLuint

#ifdef __cplusplus
extern "C" {
#endif

// lifecycle
void vita_vtc_init(void);
void vita_vtc_shutdown(void);
int  vita_vtc_active(void);

// background I/O thread
void vita_vtc_io_init(void);
void vita_vtc_io_shutdown(void);

// call once per frame to process async results
void vita_vtc_tick_frame(void);

// main-thread texture lookups (no disk I/O, cached only)
GLuint vita_vtc_lookup(const void* data, int data_size, int w, int h,
                       unsigned int fmt, const void* tlut_data, int tlut_entries,
                       int tlut_is_be, int* out_w, int* out_h);

GLuint vita_vtc_loaded_cache_lookup(unsigned long long key);

GLuint vita_vtc_lookup_by_key(unsigned long long key, int* out_w, int* out_h);

// worker-thread lookup
// reads DXT into a buffer for deferred GPU upload
unsigned char* vita_vtc_lookup_deferred(const void* data, int data_size, int w, int h,
                                        unsigned int fmt, const void* tlut_data,
                                        int tlut_entries, int tlut_is_be,
                                        int* out_w, int* out_h,
                                        int* out_dxt_size, int* out_dxt_format);

// hash key computation (safe to call from worker thread)
unsigned long long vita_vtc_compute_key(const void* data, int data_size, int w, int h,
                                        unsigned int fmt, const void* tlut_data,
                                        int tlut_entries, int tlut_is_be,
                                        unsigned int data_ptr, unsigned int data_hash_fnv,
                                        unsigned int tlut_hash_fnv);

// async prefetch / I/O slots
void vita_vtc_prefetch(void);
void vita_vtc_requeue_prefetch(unsigned long long key);
int  vita_vtc_request_read(unsigned long long key);
int  vita_vtc_check_ready(int slot, unsigned char** out_dxt, int* out_size,
                          int* out_w, int* out_h, int* out_gl_fmt);
unsigned long long vita_vtc_get_slot_key(int slot);
void vita_vtc_release_slot(int slot);

// insert into loaded cache after deferred upload finishes
int  vtc_estimate_vram(int w, int h, int fmt);
void vtc_loaded_insert(unsigned long long key, GLuint tex, int w, int h, int vram_bytes);

// perf counters
extern unsigned int vita_vtc_upgrade_us;
extern int          vita_vtc_upgrade_count;
extern int          vita_vtc_requeue_count;
extern unsigned int vita_vtc_prefetch_us;

#ifdef __cplusplus
}
#endif

#endif // TARGET_VITA 
#endif // VITA_VTC_H
