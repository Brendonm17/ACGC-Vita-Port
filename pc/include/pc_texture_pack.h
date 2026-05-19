#ifndef PC_TEXTURE_PACK_H
#define PC_TEXTURE_PACK_H

#include "pc_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

void pc_texture_pack_init(void);
void pc_texture_pack_preload_all(void);
void pc_texture_pack_shutdown(void);

/* Returns GL texture ID for an HD replacement, or 0 if none found.
 * On hit, bumps the loaded_cache ref_count and returns the cache_key
 * via out_key. Caller must release the ref on eviction so LRU can't
 * free a still-referenced HD tex (long-session wrong-texture swaps). */
GLuint pc_texture_pack_lookup(const void* data, int data_size,
                              int w, int h, unsigned int fmt,
                              const void* tlut_data, int tlut_entries, int tlut_is_be,
                              int* out_w, int* out_h,
                              unsigned long long* out_key);

/* Returns malloc'd RGBA buffer for HD replacement (no GL calls, thread-safe).
 * Caller must free() the returned buffer. Returns NULL if no match. */
unsigned char* pc_texture_pack_lookup_rgba(const void* data, int data_size,
                                           int w, int h, unsigned int fmt,
                                           const void* tlut_data, int tlut_entries, int tlut_is_be,
                                           int* out_w, int* out_h);

int pc_texture_pack_active(void);

/* Async HD texture loading (Vita: background thread, others: no-op) */
void pc_texture_pack_start_async(void);
void pc_texture_pack_stop_async(void);
int pc_texture_pack_queue_async(unsigned long long data_hash, unsigned long long tlut_hash,
                                 unsigned int gc_fmt, int orig_w, int orig_h,
                                 unsigned int data_ptr, unsigned int cache_data_hash,
                                 unsigned int tlut_key, unsigned int tlut_ptr_key,
                                 unsigned int tlut_hash_key,
                                 unsigned int wrap_s, unsigned int wrap_t,
                                 unsigned int min_filter);
int pc_texture_pack_process_async(void);  /* call once per frame from main thread */

#ifdef __cplusplus
}
#endif

#endif /* PC_TEXTURE_PACK_H */
