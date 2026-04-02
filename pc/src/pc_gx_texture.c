// pc_gx_texture.c - GC texture format decoders + texture cache
#include "pc_gx_internal.h"
#include "pc_gx_backend.h"
#include "pc_texture_pack.h"
#include <dolphin/gx/GXEnum.h>
#include <stdlib.h>

static int pc_gx_tlut_force_be(void);
static void decode_rgb5a3_entry(u16 val, u8* r, u8* g, u8* b, u8* a);
static u32 tlut_content_hash(const void* data, int tlut_fmt, int n_entries, int is_be);

#ifdef TARGET_VITA
unsigned int vita_texload_us = 0;
#endif

// TLUT stale-data detection
// the game reuses memory buffers, same address can hold different palette data.
// we track the first u16 of each TLUT slot to detect content changes and force reloads.
u16 s_tlut_first_word[16];

/* --- texture cache --- */
#ifdef TARGET_VITA
#define TEX_CACHE_SIZE 512   // smaller cache for Vita; FIFO eviction handles overflow
#else
#define TEX_CACHE_SIZE 2048
#endif

typedef struct {
    u32 data_ptr;
    u16 width;
    u16 height;
    u32 format;
    u32 tlut_name;   /* TLUT slot for CI4/CI8, 0xFFFFFFFF for non-indexed */
    u32 tlut_ptr;
    u32 tlut_hash;   /* FNV-1a of TLUT data + metadata */
    u32 data_hash;   /* FNV-1a of first N bytes */
    GLuint gl_tex;
    u32 wrap_s;
    u32 wrap_t;
    u32 min_filter;
    u8 external;     /* owned by texture pack, don't delete on eviction */
#ifdef TARGET_VITA
    unsigned long long vtc_cache_key;
#endif
} TexCacheEntry;

/* Bits-per-pixel for each GC texture format (8 for unknown as safe default) */
static int gc_format_bpp(u32 format) {
    switch (format) {
        case GX_TF_I4: case GX_TF_C4: case GX_TF_CMPR: return 4;
        case GX_TF_I8: case GX_TF_IA4: case GX_TF_C8: return 8;
        case GX_TF_IA8: case GX_TF_RGB565: case GX_TF_RGB5A3: return 16;
        case GX_TF_RGBA8: return 32;
        default: return 8;
    }
}

/* FNV-1a hash of texture data to detect buffer reuse with different content.
 * Hashes first 256 + last 256 bytes (or all if <= 512). */
static u32 tex_content_hash(const void* data, int width, int height, u32 format) {
    if (!data) return 0;
    int bpp = gc_format_bpp(format);
    int data_size = (width * height * bpp) / 8;
    const u8* p = (const u8*)data;
    u32 h = 0x811c9dc5u;
    if (data_size <= 512) {
        /* small texture: hash everything */
        for (int i = 0; i < data_size; i++) {
            h ^= p[i];
            h *= 0x01000193u;
        }
    } else {
        /* large texture: sample head + tail */
        for (int i = 0; i < 256; i++) {
            h ^= p[i];
            h *= 0x01000193u;
        }
        for (int i = data_size - 256; i < data_size; i++) {
            h ^= p[i];
            h *= 0x01000193u;
        }
    }
    return h;
}

/* FNV-1a hash of TLUT data + metadata. CI textures need palette identity
 * in the cache key since the same image is often reused with different TLUTs. */
static u32 tlut_content_hash(const void* data, int tlut_fmt, int n_entries, int is_be) {
    if (!data || n_entries <= 0) return 0;
    int bytes = n_entries * 2;
    if (bytes > 512) bytes = 512;
    const u8* p = (const u8*)data;
    u32 h = 0x811c9dc5u;
    for (int i = 0; i < bytes; i++) {
        h ^= p[i];
        h *= 0x01000193u;
    }
    /* mix in metadata to distinguish different TLUT modes */
    h ^= (u32)(tlut_fmt & 0xFF); h *= 0x01000193u;
    h ^= (u32)(n_entries & 0xFFFF); h *= 0x01000193u;
    h ^= (u32)(is_be & 1); h *= 0x01000193u;
    return h;
}

static TexCacheEntry tex_cache[TEX_CACHE_SIZE];
static int tex_cache_count = 0;
int tex_cache_hits = 0;
int tex_cache_misses = 0;

#ifdef TARGET_VITA
// Spinlock protecting texture cache from concurrent access by worker + main thread.
// process_deferred_uploads (main) and GXLoadTexObj (worker) both do cache inserts.
static volatile int tex_cache_spinlock = 0;
static inline void tex_cache_lock(void) {
    while (__sync_lock_test_and_set(&tex_cache_spinlock, 1)) { }
}
static inline void tex_cache_unlock(void) {
    __sync_lock_release(&tex_cache_spinlock);
}
#endif
#ifndef TARGET_VITA
static inline void tex_cache_lock(void) {}
static inline void tex_cache_unlock(void) {}
#endif

#ifdef TARGET_VITA
// Hash index for O(1) texture cache lookup.
// Maps data_ptr to tex_cache index via open addressing with linear probing.
// Table size must be power of 2.
#define TEX_HASH_SIZE 1024  // 2x cache size for low collision rate
#define TEX_HASH_MASK (TEX_HASH_SIZE - 1)
#define TEX_HASH_EMPTY (-1)
static int tex_hash_table[TEX_HASH_SIZE];

static void tex_hash_clear(void) {
    for (int i = 0; i < TEX_HASH_SIZE; i++)
        tex_hash_table[i] = TEX_HASH_EMPTY;
}

static u32 tex_hash_key(u32 data_ptr) {
    // mix bits for better distribution
    u32 h = data_ptr;
    h ^= h >> 16;
    h *= 0x45d9f3bu;
    h ^= h >> 16;
    return h & TEX_HASH_MASK;
}

static void tex_hash_insert(u32 data_ptr, int cache_idx) {
    u32 slot = tex_hash_key(data_ptr);
    for (int i = 0; i < TEX_HASH_SIZE; i++) {
        u32 s = (slot + i) & TEX_HASH_MASK;
        if (tex_hash_table[s] == TEX_HASH_EMPTY) {
            tex_hash_table[s] = cache_idx;
            return;
        }
    }
    // table full, shouldn't happen with 2x overprovisioning
}

static void tex_hash_rebuild(void) {
    tex_hash_clear();
    for (int i = 0; i < tex_cache_count; i++)
        tex_hash_insert(tex_cache[i].data_ptr, i);
}
#endif

#ifdef TARGET_VITA
extern volatile int vita_on_worker_thread;

// Deferred texture deletion.
// VitaGL/GXM crashes if a texture is deleted while the GPU still references it.
// Collect and flush at start of next frame after vglSwapBuffers.
#define VITA_TEX_DELETE_MAX 4096
static GLuint vita_tex_delete_queue[VITA_TEX_DELETE_MAX];
static int vita_tex_delete_count = 0;

void vita_defer_tex_delete(GLuint tex) {
    if (tex == 0) return;
    if (vita_tex_delete_count >= VITA_TEX_DELETE_MAX) {
        // Queue full, sync GPU then flush
        glFinish();
        glDeleteTextures(vita_tex_delete_count, vita_tex_delete_queue);
        vita_tex_delete_count = 0;
    }
    vita_tex_delete_queue[vita_tex_delete_count++] = tex;
}

void pc_gx_texture_flush_deferred_deletes(void) {
    if (vita_tex_delete_count > 0) {
        // delete in batches of 64 to avoid GPU stall from mass deletion
        int to_delete = vita_tex_delete_count < 64 ? vita_tex_delete_count : 64;
        glDeleteTextures(to_delete, vita_tex_delete_queue);
        if (to_delete < vita_tex_delete_count) {
            memmove(vita_tex_delete_queue, &vita_tex_delete_queue[to_delete],
                    (vita_tex_delete_count - to_delete) * sizeof(GLuint));
        }
        vita_tex_delete_count -= to_delete;
    }
}
#else
void pc_gx_texture_flush_deferred_deletes(void) { }
#endif

static TexCacheEntry* tex_cache_find(u32 data_ptr, int w, int h, u32 fmt, u32 tlut_name,
                                     u32 tlut_ptr, u32 tlut_hash, u32 data_hash) {
#ifdef TARGET_VITA
    tex_cache_lock();
    // Hash-accelerated lookup: probe hash table for data_ptr matches,
    // then verify remaining fields.
    u32 slot = tex_hash_key(data_ptr);
    for (int i = 0; i < TEX_HASH_SIZE; i++) {
        u32 s = (slot + i) & TEX_HASH_MASK;
        int idx = tex_hash_table[s];
        if (idx == TEX_HASH_EMPTY) { tex_cache_unlock(); return NULL; }
        if (idx < tex_cache_count) {
            TexCacheEntry* e = &tex_cache[idx];
            if (e->data_ptr == data_ptr && e->width == w && e->height == h &&
                e->format == fmt && e->tlut_name == tlut_name && e->tlut_ptr == tlut_ptr &&
                e->tlut_hash == tlut_hash && e->data_hash == data_hash) {
                tex_cache_unlock();
                return e;
            }
        }
    }
    tex_cache_unlock();
    return NULL;
#else
    /* Linear scan. Fine for <=2048 entries at ~100% hit rate on PC. */
    for (int i = 0; i < tex_cache_count; i++) {
        TexCacheEntry* e = &tex_cache[i];
        if (e->data_ptr == data_ptr && e->width == w && e->height == h &&
            e->format == fmt && e->tlut_name == tlut_name && e->tlut_ptr == tlut_ptr &&
            e->tlut_hash == tlut_hash && e->data_hash == data_hash) {
            return e;
        }
    }
    return NULL;
#endif
}

static TexCacheEntry* tex_cache_insert(u32 data_ptr, int w, int h, u32 fmt, u32 tlut_name,
                                       u32 tlut_ptr, u32 tlut_hash, u32 data_hash, GLuint gl_tex) {
    tex_cache_lock();
    if (tex_cache_count >= TEX_CACHE_SIZE - 32) {
        // Incremental eviction: evict oldest 32 entries instead of half.
        // Triggers earlier (at 480) to avoid sudden bursts at 512.
        int evict_n = 32;
        for (int i = 0; i < evict_n; i++) {
            if (tex_cache[i].gl_tex) {
                for (int s = 0; s < 8; s++) {
                    if (g_gx.gl_textures[s] == tex_cache[i].gl_tex)
                        g_gx.gl_textures[s] = 0;
                }
                if (!tex_cache[i].external)
                    PC_DELETE_TEXTURE(tex_cache[i].gl_tex);
            }
        }
        memmove(&tex_cache[0], &tex_cache[evict_n], (tex_cache_count - evict_n) * sizeof(TexCacheEntry));
        tex_cache_count -= evict_n;
#ifdef TARGET_VITA
        tex_hash_rebuild();
#endif
    }
    TexCacheEntry* e = &tex_cache[tex_cache_count];
    e->data_ptr = data_ptr;
    e->width = (u16)w;
    e->height = (u16)h;
    e->format = fmt;
    e->tlut_name = tlut_name;
    e->tlut_ptr = tlut_ptr;
    e->tlut_hash = tlut_hash;
    e->data_hash = data_hash;
    e->gl_tex = gl_tex;
    e->wrap_s = 0xFFFFFFFF;
    e->wrap_t = 0xFFFFFFFF;
    e->min_filter = 0xFFFFFFFF;
    e->external = 0;
#ifdef TARGET_VITA
    tex_hash_insert(data_ptr, tex_cache_count);
#endif
    tex_cache_count++;
    tex_cache_unlock();
    return e;
}

#ifdef TARGET_VITA
// Deferred texture upload for worker thread.
// When emu64 runs on the worker thread and encounters a texture cache miss,
// the GL upload is deferred to the main thread via this queue.
// Processed in pc_gx_submit_frame() before any draws.
// Current frame shows untextured (tex_id=0); next frame the cache hits.
#define VITA_TEX_UPLOAD_MAX 128
typedef struct {
    u8* rgba;           // decoded RGBA data (heap-allocated, freed after upload)
    int width, height;
    int compressed;         // 1 = DXT data from VTC, 0 = decoded RGBA
    int compressed_size;    // byte size of compressed DXT payload
    GLenum gl_internal;     // GL_COMPRESSED_RGBA_S3TC_DXT1_EXT or DXT5
    GLenum gl_filter;
    GLenum gl_wrap_s, gl_wrap_t;
    // cache key fields for insertion after upload
    u32 data_ptr;
    u16 cache_w, cache_h;
    u32 format;
    u32 tlut_name, tlut_ptr, tlut_hash, data_hash;
    u32 wrap_s, wrap_t, min_filter;
    int gl_tex_slot;        // g_gx.gl_textures[] index to update after upload
    unsigned long long vtc_cache_key; // pre-computed VTC key from worker thread
    int external;           // 1 if HD texture replaced GC texture
} VitaDeferredTexUpload;

// Double-buffered: worker writes to [cmd_write], main thread reads [1-cmd_write].
// Prevents races between worker appending and main thread processing.
extern int cmd_write;
static VitaDeferredTexUpload vita_tex_upload_db[2][VITA_TEX_UPLOAD_MAX];
static int vita_tex_upload_count_db[2] = {0, 0};
#define vita_tex_upload_queue  vita_tex_upload_db[cmd_write]
#define vita_tex_upload_count  vita_tex_upload_count_db[cmd_write]

// Deferred texture parameter updates: glTexParameteri can't run on the
// worker thread (VitaGL not thread-safe). Queue for main thread.
#define VITA_TEX_PARAM_MAX 128
typedef struct {
    GLuint tex_id;
    GLenum wrap_s, wrap_t, min_filter;
} VitaDeferredTexParam;
static VitaDeferredTexParam vita_tex_param_db[2][VITA_TEX_PARAM_MAX];
static int vita_tex_param_count_db[2] = {0, 0};
#define vita_tex_param_queue  vita_tex_param_db[cmd_write]
#define vita_tex_param_count  vita_tex_param_count_db[cmd_write]

void pc_gx_texture_process_deferred_params(void) {
    int rd = 1 - cmd_write;
    int cnt = vita_tex_param_count_db[rd];
    for (int i = 0; i < cnt; i++) {
        VitaDeferredTexParam* p = &vita_tex_param_db[rd][i];
        glActiveTexture(GL_TEXTURE7);
        glBindTexture(GL_TEXTURE_2D, p->tex_id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, p->wrap_s);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, p->wrap_t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, p->min_filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, p->min_filter);
    }
    if (cnt > 0) glActiveTexture(GL_TEXTURE0);
    vita_tex_param_count_db[rd] = 0;
}

int pc_gx_deferred_tex_uploads = 0;

// Uploaded GL IDs indexed by deferred upload queue position.
// Draw commands store their upload index (tex_deferred_idx) which maps 1:1 here.
GLuint vita_deferred_uploaded[VITA_TEX_UPLOAD_MAX] = {0};
int vita_deferred_uploaded_count = 0;
// legacy slot-based array (kept for compatibility)
GLuint vita_just_uploaded_tex[8] = {0};

// Invalidate tex_cache entries referencing a specific GL texture.
// Called when VTC loaded_cache evicts an HD texture.
void pc_gx_texture_invalidate_gl_tex(GLuint tex) {
    if (!tex) return;
    tex_cache_lock();
    for (int i = 0; i < tex_cache_count; i++) {
        if (tex_cache[i].gl_tex == tex) {
            tex_cache[i].gl_tex = 0;
            tex_cache[i].data_ptr = 0;
            tex_cache[i].external = 0;
        }
    }
    tex_cache_unlock();
}

// Per-frame timing for stutter diagnosis
#ifdef VITA_PERF_LOG
static unsigned int vtc_perf_total_us = 0;
static unsigned int vtc_perf_peak_total_us = 0;
static unsigned int vtc_perf_peak_drain_us = 0;
static unsigned int vtc_perf_peak_upgrade_us = 0;
static unsigned int vtc_perf_peak_upload_us = 0;
static int vtc_perf_peak_drain_kb = 0;
static int vtc_perf_peak_upload_cnt = 0;
static int vtc_perf_spike_count = 0;
static int vtc_perf_frames = 0;
#endif

void pc_gx_texture_process_deferred_uploads(void) {
    unsigned int _frame_t0 = sceKernelGetProcessTimeLow();
    unsigned int _section_drain_us = 0, _section_upgrade_us = 0, _section_upload_us = 0;
    int _drain_kb = 0;
    int rd = 1 - cmd_write;
    int cnt = vita_tex_upload_count_db[rd];
    pc_gx_deferred_tex_uploads = cnt;
    vita_deferred_uploaded_count = cnt;
    memset(vita_deferred_uploaded, 0, sizeof(vita_deferred_uploaded));
    memset(vita_just_uploaded_tex, 0, sizeof(vita_just_uploaded_tex));

    // Drain READY I/O slots into loaded_cache before processing deferred uploads
    {
        extern int vita_vtc_check_ready(int, unsigned char**, int*, int*, int*, int*);
        extern void vita_vtc_release_slot(int);
        extern void vtc_loaded_insert(unsigned long long, GLuint, int, int, int);
        extern int vtc_estimate_vram(int, int, int);
        extern unsigned long long vita_vtc_get_slot_key(int);

        unsigned int _drain_t0 = sceKernelGetProcessTimeLow();
        int slot_uploads_this_frame = 0;
        for (int si = 0; si < 24 && slot_uploads_this_frame < 1; si++) {
            unsigned char* dxt_data = NULL;
            int dxt_size = 0, hd_w = 0, hd_h = 0, gl_fmt = 0;
            if (!vita_vtc_check_ready(si, &dxt_data, &dxt_size, &hd_w, &hd_h, &gl_fmt))
                continue;

            unsigned long long slot_key = vita_vtc_get_slot_key(si);

            GLuint hd_tex;
            glGenTextures(1, &hd_tex);
            glActiveTexture(GL_TEXTURE7);
            glBindTexture(GL_TEXTURE_2D, hd_tex);
            glCompressedTexImage2D(GL_TEXTURE_2D, 0, (GLenum)gl_fmt,
                                   hd_w, hd_h, 0, dxt_size, dxt_data);
            glActiveTexture(GL_TEXTURE0);
            vita_vtc_release_slot(si);

            if (glGetError() != GL_NO_ERROR) {
                glDeleteTextures(1, &hd_tex);
                continue;
            }

            // DXT5=8bpp, DXT1/PVRTC4=4bpp
            int vram_fmt = (gl_fmt == 0x83F3) ? 5 : 1;
            vtc_loaded_insert(slot_key, hd_tex, hd_w, hd_h,
                              vtc_estimate_vram(hd_w, hd_h, vram_fmt));
            _drain_kb = dxt_size / 1024;
            slot_uploads_this_frame++;
        }
        _section_drain_us = sceKernelGetProcessTimeLow() - _drain_t0;
    }

    // Standalone upgrade scan
    {
        unsigned int _upgrade_t0 = sceKernelGetProcessTimeLow();
        extern GLuint vita_vtc_loaded_cache_lookup(unsigned long long key);
        extern void vita_vtc_requeue_prefetch(unsigned long long key);
        static int scan_pos = 0;
        unsigned int _t0 = sceKernelGetProcessTimeLow();
        int upgrades = 0, requeues = 0, checked = 0;

        tex_cache_lock();
        for (int n = 0; n < 32 && n < tex_cache_count; n++) {
            int ci = (scan_pos + n) % tex_cache_count;
            if (!tex_cache[ci].vtc_cache_key || tex_cache[ci].external || !tex_cache[ci].gl_tex)
                continue;
            checked++;
            GLuint hd_tex = vita_vtc_loaded_cache_lookup(tex_cache[ci].vtc_cache_key);
            if (!hd_tex) {
                if (requeues < 4) {
                    vita_vtc_requeue_prefetch(tex_cache[ci].vtc_cache_key);
                    requeues++;
                }
                continue;
            }

            vita_defer_tex_delete(tex_cache[ci].gl_tex);
            tex_cache[ci].gl_tex = hd_tex;
            tex_cache[ci].external = 1;
            tex_cache[ci].vtc_cache_key = 0;
            tex_cache[ci].wrap_s = 0xFFFFFFFF;
            tex_cache[ci].wrap_t = 0xFFFFFFFF;
            tex_cache[ci].min_filter = 0xFFFFFFFF;
            upgrades++;
        }
        scan_pos = (scan_pos + 32) % (tex_cache_count > 0 ? tex_cache_count : 1);
        tex_cache_unlock();

        _section_upgrade_us = sceKernelGetProcessTimeLow() - _upgrade_t0;
        {
            extern unsigned int vita_vtc_upgrade_us;
            extern int vita_vtc_upgrade_count, vita_vtc_requeue_count;
            vita_vtc_upgrade_us += _section_upgrade_us;
            vita_vtc_upgrade_count += upgrades;
            vita_vtc_requeue_count += requeues;
        }
    }

    for (int i = 0; i < cnt; i++) {
        VitaDeferredTexUpload* up = &vita_tex_upload_db[rd][i];
        GLuint tex;
        glGenTextures(1, &tex);
        glActiveTexture(GL_TEXTURE7);
        glBindTexture(GL_TEXTURE_2D, tex);
        if (up->rgba && up->width > 0 && up->height > 0) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, up->width, up->height, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, up->rgba);
        } else {
            u8 white[4] = {255, 255, 255, 255};
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, up->gl_filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, up->gl_filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, up->gl_wrap_s);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, up->gl_wrap_t);
        glActiveTexture(GL_TEXTURE0);

        // Try VTC HD replacement via pre-computed key from worker thread
        if (!up->compressed && up->vtc_cache_key != 0) {
            extern GLuint vita_vtc_lookup_by_key(unsigned long long key, int* out_w, int* out_h);
            int hd_w = 0, hd_h = 0;
            GLuint hd_tex = vita_vtc_lookup_by_key(up->vtc_cache_key, &hd_w, &hd_h);
            if (hd_tex) {
                glActiveTexture(GL_TEXTURE7);
                glBindTexture(GL_TEXTURE_2D, hd_tex);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, up->gl_filter);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, up->gl_filter);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, up->gl_wrap_s);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, up->gl_wrap_t);
                glActiveTexture(GL_TEXTURE0);
                vita_defer_tex_delete(tex);
                tex = hd_tex;
                up->external = 1;
            }
            // If HD not ready yet, fall through to cache as GC.
            // HD version loads on natural tex_cache eviction + re-request.
        }

        TexCacheEntry* entry = tex_cache_insert(up->data_ptr, up->cache_w, up->cache_h,
                                                 up->format, up->tlut_name, up->tlut_ptr,
                                                 up->tlut_hash, up->data_hash, tex);
        entry->wrap_s = up->wrap_s;
        entry->wrap_t = up->wrap_t;
        entry->min_filter = up->min_filter;
        if (up->external) entry->external = 1;
        entry->vtc_cache_key = up->vtc_cache_key;

        // record for same-frame re-resolve
        vita_deferred_uploaded[i] = tex;
        if (up->gl_tex_slot >= 0 && up->gl_tex_slot < 8)
            vita_just_uploaded_tex[up->gl_tex_slot] = tex;

        if (up->rgba) free(up->rgba);
    }
    _section_upload_us = sceKernelGetProcessTimeLow() - _frame_t0
                         - _section_drain_us - _section_upgrade_us;
    vita_tex_upload_count_db[rd] = 0;

    extern void vita_vtc_tick_frame(void);
    vita_vtc_tick_frame();
}
#endif // TARGET_VITA deferred upload

void pc_gx_texture_cache_invalidate(void) {
    tex_cache_lock();
    for (int i = 0; i < tex_cache_count; i++) {
        if (tex_cache[i].gl_tex && !tex_cache[i].external)
            PC_DELETE_TEXTURE(tex_cache[i].gl_tex);
    }
    tex_cache_count = 0;
#ifdef TARGET_VITA
    tex_hash_clear();
#endif
    tex_cache_unlock();
}

void pc_gx_texture_init(void) {
    tex_cache_count = 0;
    tex_cache_hits = 0;
    tex_cache_misses = 0;
#ifdef TARGET_VITA
    tex_hash_clear();
#endif
    (void)pc_gx_tlut_force_be();
}

void pc_gx_texture_shutdown(void) {
    pc_gx_texture_cache_invalidate();
    memset(g_gx.gl_textures, 0, sizeof(g_gx.gl_textures));
}

/* --- texture object API --- */

/* GXTexObj layout for PC: 22 u32s (88 bytes) */
#define TEXOBJ_IMAGE_PTR   0
#define TEXOBJ_WIDTH       1
#define TEXOBJ_HEIGHT      2
#define TEXOBJ_FORMAT      3
#define TEXOBJ_WRAP_S      4
#define TEXOBJ_WRAP_T      5
#define TEXOBJ_MIPMAP      6
#define TEXOBJ_MIN_FILTER  7
#define TEXOBJ_MAG_FILTER  8
#define TEXOBJ_MIN_LOD     9
#define TEXOBJ_MAX_LOD     10
#define TEXOBJ_LOD_BIAS    11
#define TEXOBJ_BIAS_CLAMP  12
#define TEXOBJ_EDGE_LOD    13
#define TEXOBJ_MAX_ANISO   14
#define TEXOBJ_GL_TEX      15
#define TEXOBJ_CI_FORMAT   16
#define TEXOBJ_TLUT_NAME   17
#define TEXOBJ_SIZE        22  /* total u32 count */

/* GXTlutObj layout (4 u32s) */
#define TLUTOBJ_DATA       0
#define TLUTOBJ_FORMAT     1
#define TLUTOBJ_N_ENTRIES  2

void GXInitTexObj(void* obj, void* image_ptr, u16 width, u16 height, u32 format,
                  u32 wrap_s, u32 wrap_t, u8 mipmap) {
    u32* o = (u32*)obj;
    memset(o, 0, TEXOBJ_SIZE * sizeof(u32));
    o[TEXOBJ_IMAGE_PTR] = (u32)(uintptr_t)image_ptr;
    o[TEXOBJ_WIDTH] = width;
    o[TEXOBJ_HEIGHT] = height;
    o[TEXOBJ_FORMAT] = format;
    o[TEXOBJ_WRAP_S] = wrap_s;
    o[TEXOBJ_WRAP_T] = wrap_t;
    o[TEXOBJ_MIPMAP] = mipmap;
    o[TEXOBJ_MIN_FILTER] = 1; /* GX_LINEAR */
    o[TEXOBJ_MAG_FILTER] = 1; /* GX_LINEAR */
}

void GXInitTexObjCI(void* obj, void* image_ptr, u16 width, u16 height, u32 format,
                    u32 wrap_s, u32 wrap_t, u8 mipmap, u32 tlut_name) {
    GXInitTexObj(obj, image_ptr, width, height, format, wrap_s, wrap_t, mipmap);
    u32* o = (u32*)obj;
    o[TEXOBJ_CI_FORMAT] = format;
    o[TEXOBJ_TLUT_NAME] = tlut_name;
}

void GXInitTexObjData(void* obj, void* image_ptr) {
    u32* o = (u32*)obj;
    o[TEXOBJ_IMAGE_PTR] = (u32)(uintptr_t)image_ptr;
}

void GXInitTexObjLOD(void* obj, u32 min_filt, u32 mag_filt, f32 min_lod, f32 max_lod,
                     f32 lod_bias, GXBool bias_clamp, GXBool edge_lod, u32 max_aniso) {
    u32* o = (u32*)obj;
    o[TEXOBJ_MIN_FILTER] = min_filt;
    o[TEXOBJ_MAG_FILTER] = mag_filt;
    /* store floats as bits */
    memcpy(&o[TEXOBJ_MIN_LOD], &min_lod, sizeof(f32));
    memcpy(&o[TEXOBJ_MAX_LOD], &max_lod, sizeof(f32));
    memcpy(&o[TEXOBJ_LOD_BIAS], &lod_bias, sizeof(f32));
    o[TEXOBJ_BIAS_CLAMP] = bias_clamp;
    o[TEXOBJ_EDGE_LOD] = edge_lod;
    o[TEXOBJ_MAX_ANISO] = max_aniso;
}

void GXInitTexObjWrapMode(void* obj, u32 s, u32 t) {
    u32* o = (u32*)obj;
    o[TEXOBJ_WRAP_S] = s;
    o[TEXOBJ_WRAP_T] = t;
}

/* --- GC texture format decoders (tile layout -> linear RGBA8) --- */

/* Read a big-endian u16 (GC textures store 16-bit pixels in BE order) */
static inline u16 read_be16(const u8* p) {
    return (u16)((p[0] << 8) | p[1]);
}

/* I4: 8x8 blocks, 4bpp, each byte = 2 pixels */
static void decode_I4(const u8* src, u8* dst, int w, int h) {
    int bw = (w + 7) / 8, bh = (h + 7) / 8;
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            for (int y = 0; y < 8; y++) {
                for (int x = 0; x < 8; x += 2) {
                    u8 val = *src++;
                    int px0 = bx * 8 + x, py = by * 8 + y;
                    int px1 = px0 + 1;
                    u8 i0 = (val >> 4) | (val & 0xF0);
                    u8 i1 = (val & 0x0F) | ((val & 0x0F) << 4);
                    if (px0 < w && py < h) {
                        int idx = (py * w + px0) * 4;
                        dst[idx] = dst[idx+1] = dst[idx+2] = i0;
                        dst[idx+3] = i0;
                    }
                    if (px1 < w && py < h) {
                        int idx = (py * w + px1) * 4;
                        dst[idx] = dst[idx+1] = dst[idx+2] = i1;
                        dst[idx+3] = i1;
                    }
                }
            }
        }
    }
}

/* I8: 8x4 blocks, 8bpp */
static void decode_I8(const u8* src, u8* dst, int w, int h) {
    int bw = (w + 7) / 8, bh = (h + 3) / 4;
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            for (int y = 0; y < 4; y++) {
                for (int x = 0; x < 8; x++) {
                    u8 val = *src++;
                    int px = bx * 8 + x, py = by * 4 + y;
                    if (px < w && py < h) {
                        int idx = (py * w + px) * 4;
                        dst[idx] = dst[idx+1] = dst[idx+2] = val;
                        dst[idx+3] = val;
                    }
                }
            }
        }
    }
}

/* IA4: 8x4 blocks, high nibble = alpha, low nibble = intensity */
static void decode_IA4(const u8* src, u8* dst, int w, int h) {
    int bw = (w + 7) / 8, bh = (h + 3) / 4;
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            for (int y = 0; y < 4; y++) {
                for (int x = 0; x < 8; x++) {
                    u8 val = *src++;
                    int px = bx * 8 + x, py = by * 4 + y;
                    if (px < w && py < h) {
                        u8 a = (val >> 4) | (val & 0xF0);
                        u8 i = (val & 0x0F) | ((val & 0x0F) << 4);
                        int idx = (py * w + px) * 4;
                        dst[idx] = dst[idx+1] = dst[idx+2] = i; dst[idx+3] = a;
                    }
                }
            }
        }
    }
}

/* IA8: 4x4 blocks, 16bpp, alpha byte + intensity byte per pixel */
static void decode_IA8(const u8* src, u8* dst, int w, int h) {
    int bw = (w + 3) / 4, bh = (h + 3) / 4;
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            for (int y = 0; y < 4; y++) {
                for (int x = 0; x < 4; x++) {
                    u8 a = *src++;
                    u8 i = *src++;
                    int px = bx * 4 + x, py = by * 4 + y;
                    if (px < w && py < h) {
                        int idx = (py * w + px) * 4;
                        dst[idx] = dst[idx+1] = dst[idx+2] = i; dst[idx+3] = a;
                    }
                }
            }
        }
    }
}

/* RGB565: 4x4 blocks, 16bpp */
static void decode_RGB565(const u8* src, u8* dst, int w, int h) {
    int bw = (w + 3) / 4, bh = (h + 3) / 4;
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            for (int y = 0; y < 4; y++) {
                for (int x = 0; x < 4; x++) {
                    u16 val = (src[0] << 8) | src[1]; src += 2;
                    int px = bx * 4 + x, py = by * 4 + y;
                    if (px < w && py < h) {
                        u8 r = ((val >> 11) & 0x1F) * 255 / 31;
                        u8 g = ((val >> 5) & 0x3F) * 255 / 63;
                        u8 b = (val & 0x1F) * 255 / 31;
                        int idx = (py * w + px) * 4;
                        dst[idx] = r; dst[idx+1] = g; dst[idx+2] = b; dst[idx+3] = 255;
                    }
                }
            }
        }
    }
}

/* RGB5A3: 4x4 blocks, 16bpp. Bit 15=1: RGB555 opaque, bit 15=0: ARGB3444 */
static void decode_RGB5A3(const u8* src, u8* dst, int w, int h) {
    int bw = (w + 3) / 4, bh = (h + 3) / 4;
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            for (int y = 0; y < 4; y++) {
                for (int x = 0; x < 4; x++) {
                    u16 val = (src[0] << 8) | src[1]; src += 2;
                    int px = bx * 4 + x, py = by * 4 + y;
                    if (px < w && py < h) {
                        int idx = (py * w + px) * 4;
                        decode_rgb5a3_entry(val, &dst[idx], &dst[idx+1], &dst[idx+2], &dst[idx+3]);
                    }
                }
            }
        }
    }
}

/* RGBA8: 4x4 blocks, 32bpp. Two passes per block: AR then GB (64 bytes total) */
static void decode_RGBA8(const u8* src, u8* dst, int w, int h) {
    int bw = (w + 3) / 4, bh = (h + 3) / 4;
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            u8 ar[16][2]; /* AR pass */
            for (int i = 0; i < 16; i++) {
                ar[i][0] = *src++;
                ar[i][1] = *src++;
            }
            for (int i = 0; i < 16; i++) { /* GB pass */
                int x = i % 4, y = i / 4;
                int px = bx * 4 + x, py = by * 4 + y;
                u8 g = *src++;
                u8 b = *src++;
                if (px < w && py < h) {
                    int idx = (py * w + px) * 4;
                    dst[idx] = ar[i][1];
                    dst[idx+1] = g;
                    dst[idx+2] = b;
                    dst[idx+3] = ar[i][0];
                }
            }
        }
    }
}

/* CMPR (S3TC/DXT1): 8x8 super-blocks of 2x2 sub-blocks, each sub is 4x4 DXT1 */
static void decode_CMPR(const u8* src, u8* dst, int w, int h) {
    int bw = (w + 7) / 8, bh = (h + 7) / 8;
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            for (int sub = 0; sub < 4; sub++) {
                int sx = (sub & 1) * 4, sy = (sub >> 1) * 4;
                u16 c0 = (src[0] << 8) | src[1]; /* DXT1 block (BE) */
                u16 c1 = (src[2] << 8) | src[3];
                src += 4;
                u8 palette[4][4];
                palette[0][0] = ((c0 >> 11) & 0x1F) * 255 / 31;
                palette[0][1] = ((c0 >> 5) & 0x3F) * 255 / 63;
                palette[0][2] = (c0 & 0x1F) * 255 / 31;
                palette[0][3] = 255;
                palette[1][0] = ((c1 >> 11) & 0x1F) * 255 / 31;
                palette[1][1] = ((c1 >> 5) & 0x3F) * 255 / 63;
                palette[1][2] = (c1 & 0x1F) * 255 / 31;
                palette[1][3] = 255;
                /* interpolated colors */
                if (c0 > c1) {
                    for (int c = 0; c < 3; c++) {
                        palette[2][c] = (2 * palette[0][c] + palette[1][c]) / 3;
                        palette[3][c] = (palette[0][c] + 2 * palette[1][c]) / 3;
                    }
                    palette[2][3] = palette[3][3] = 255;
                } else {
                    for (int c = 0; c < 3; c++)
                        palette[2][c] = (palette[0][c] + palette[1][c]) / 2;
                    palette[2][3] = 255;
                    palette[3][0] = palette[3][1] = palette[3][2] = 0;
                    palette[3][3] = 0;
                }
                for (int y = 0; y < 4; y++) {
                    u8 row = *src++;
                    for (int x = 0; x < 4; x++) {
                        int ci = (row >> (6 - x * 2)) & 3;
                        int px = bx * 8 + sx + x, py = by * 8 + sy + y;
                        if (px < w && py < h) {
                            int idx = (py * w + px) * 4;
                            dst[idx] = palette[ci][0];
                            dst[idx+1] = palette[ci][1];
                            dst[idx+2] = palette[ci][2];
                            dst[idx+3] = palette[ci][3];
                        }
                    }
                }
            }
        }
    }
}

/* Decode a single RGB5A3 value to RGBA8 */
static void decode_rgb5a3_entry(u16 val, u8* r, u8* g, u8* b, u8* a) {
    if (val & 0x8000) { /* RGB555 opaque */
        *r = ((val >> 10) & 0x1F) * 255 / 31;
        *g = ((val >> 5) & 0x1F) * 255 / 31;
        *b = (val & 0x1F) * 255 / 31;
        *a = 255;
    } else { /* ARGB3444 */
        *a = ((val >> 12) & 0x07) * 255 / 7;
        *r = ((val >> 8) & 0x0F) * 255 / 15;
        *g = ((val >> 4) & 0x0F) * 255 / 15;
        *b = (val & 0x0F) * 255 / 15;
    }
}

/* Decode a single RGB565 value to RGBA8 (always opaque) */
static void decode_rgb565_entry(u16 val, u8* r, u8* g, u8* b, u8* a) {
    *r = (u8)(((val >> 11) & 0x1F) * 255 / 31);
    *g = (u8)(((val >> 5) & 0x3F) * 255 / 63);
    *b = (u8)((val & 0x1F) * 255 / 31);
    *a = 255;
}

/* Build a 256-entry RGBA8 palette from TLUT data.
 * is_be=1 for ROM/JSystem data (BE), 0 for emu64 tlutconv output (native LE). */
static void build_palette(const void* tlut_data, int tlut_fmt, int n_entries,
                          u8 palette[256][4], int is_be) {
    /* default: grayscale ramp for missing palette */
    for (int i = 0; i < 256; i++) {
        palette[i][0] = palette[i][1] = palette[i][2] = (u8)i;
        palette[i][3] = 255;
    }
    if (!tlut_data || n_entries <= 0) return;
    if (n_entries > 256) n_entries = 256;

    const u16* pal16 = (const u16*)tlut_data;
    const u8* pal_bytes = (const u8*)tlut_data;
    for (int i = 0; i < n_entries; i++) {
        u16 val;
        if (is_be) {
            val = read_be16(pal_bytes + i * 2);
        } else {
            val = pal16[i];
        }
        if (tlut_fmt == GX_TL_RGB5A3) {
            decode_rgb5a3_entry(val, &palette[i][0], &palette[i][1],
                               &palette[i][2], &palette[i][3]);
        } else if (tlut_fmt == GX_TL_RGB565) {
            decode_rgb565_entry(val, &palette[i][0], &palette[i][1],
                                &palette[i][2], &palette[i][3]);
        } else { /* GX_TL_IA8: high byte = intensity, low byte = alpha */
            if (is_be) {
                palette[i][0] = palette[i][1] = palette[i][2] = (u8)(val >> 8);
                palette[i][3] = (u8)(val & 0xFF);
            } else {
                /* LE read of BE bytes [I,A] swaps them */
                palette[i][0] = palette[i][1] = palette[i][2] = (u8)(val & 0xFF);
                palette[i][3] = (u8)(val >> 8);
            }
        }
    }
}

/* CI4: 8x8 blocks, 4bpp indexed */
static void decode_CI4(const u8* src, u8* dst, int w, int h, const u8 palette[256][4]) {
    int bw = (w + 7) / 8, bh = (h + 7) / 8;
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            for (int y = 0; y < 8; y++) {
                for (int x = 0; x < 8; x += 2) {
                    u8 val = *src++;
                    int px0 = bx * 8 + x, py = by * 8 + y;
                    int px1 = px0 + 1;
                    int ci0 = (val >> 4) & 0xF;
                    int ci1 = val & 0xF;
                    if (px0 < w && py < h) {
                        int idx = (py * w + px0) * 4;
                        dst[idx] = palette[ci0][0]; dst[idx+1] = palette[ci0][1];
                        dst[idx+2] = palette[ci0][2]; dst[idx+3] = palette[ci0][3];
                    }
                    if (px1 < w && py < h) {
                        int idx = (py * w + px1) * 4;
                        dst[idx] = palette[ci1][0]; dst[idx+1] = palette[ci1][1];
                        dst[idx+2] = palette[ci1][2]; dst[idx+3] = palette[ci1][3];
                    }
                }
            }
        }
    }
}

/* CI8: 8x4 blocks, 8bpp indexed */
static void decode_CI8(const u8* src, u8* dst, int w, int h, const u8 palette[256][4]) {
    int bw = (w + 7) / 8, bh = (h + 3) / 4;
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            for (int y = 0; y < 4; y++) {
                for (int x = 0; x < 8; x++) {
                    u8 val = *src++;
                    int px = bx * 8 + x, py = by * 4 + y;
                    if (px < w && py < h) {
                        int idx = (py * w + px) * 4;
                        dst[idx] = palette[val][0]; dst[idx+1] = palette[val][1];
                        dst[idx+2] = palette[val][2]; dst[idx+3] = palette[val][3];
                    }
                }
            }
        }
    }
}

static void decode_gc_texture(const void* src, u8* dst_rgba, int w, int h, u32 fmt,
                              const u8 palette[256][4]) {
    /* transparent fallback for unhandled formats */
    memset(dst_rgba, 0, w * h * 4);

    switch (fmt) {
        case GX_TF_I4:     decode_I4((const u8*)src, dst_rgba, w, h); break;
        case GX_TF_I8:     decode_I8((const u8*)src, dst_rgba, w, h); break;
        case GX_TF_IA4:    decode_IA4((const u8*)src, dst_rgba, w, h); break;
        case GX_TF_IA8:    decode_IA8((const u8*)src, dst_rgba, w, h); break;
        case GX_TF_RGB565: decode_RGB565((const u8*)src, dst_rgba, w, h); break;
        case GX_TF_RGB5A3: decode_RGB5A3((const u8*)src, dst_rgba, w, h); break;
        case GX_TF_RGBA8:  decode_RGBA8((const u8*)src, dst_rgba, w, h); break;
        case GX_TF_C4:     decode_CI4((const u8*)src, dst_rgba, w, h, palette); break;
        case GX_TF_C8:     decode_CI8((const u8*)src, dst_rgba, w, h, palette); break;
        case GX_TF_CMPR:   decode_CMPR((const u8*)src, dst_rgba, w, h); break;
        default:
            break;
    }
}

void GXLoadTexObj(void* obj, u32 id) {
#ifdef TARGET_VITA
    extern unsigned int vita_texload_us;
    unsigned int _texload_t0 = sceKernelGetProcessTimeLow();
#endif
    pc_gx_flush_if_begin_complete();

    if (id >= 8 && id != 0xFF && id < 0x100) {
#ifdef TARGET_VITA
        vita_texload_us += sceKernelGetProcessTimeLow() - _texload_t0;
#endif
        return;
    }
    if (id >= 8) {
#ifdef TARGET_VITA
        vita_texload_us += sceKernelGetProcessTimeLow() - _texload_t0;
#endif
        return;
    }

    u32* o = (u32*)obj;
    void* image_ptr = (void*)(uintptr_t)o[TEXOBJ_IMAGE_PTR];
    int width = (int)o[TEXOBJ_WIDTH];
    int height = (int)o[TEXOBJ_HEIGHT];
    u32 format = o[TEXOBJ_FORMAT];
    u32 wrap_s = o[TEXOBJ_WRAP_S], wrap_t = o[TEXOBJ_WRAP_T];
    // Track per-slot wrap mode for command buffer per-draw wrap replay.
    // Must be set BEFORE any early return so the snapshot always has valid wrap data.
    g_gx.tex_obj_wrap_s[id] = wrap_s;
    g_gx.tex_obj_wrap_t[id] = wrap_t;
    u32 tlut_key = (format == GX_TF_C4 || format == GX_TF_C8) ? o[TEXOBJ_TLUT_NAME] : 0xFFFFFFFF;
    u32 tlut_ptr_key = 0;
    u32 tlut_hash_key = 0;
    u32 filter_mode = o[TEXOBJ_MIN_FILTER];

    if (format == GX_TF_C4 || format == GX_TF_C8) {
        int tlut_name = (int)o[TEXOBJ_TLUT_NAME];
        if (tlut_name >= 0 && tlut_name < 16 && g_gx.tlut[tlut_name].data) {
            tlut_ptr_key = (u32)(uintptr_t)g_gx.tlut[tlut_name].data;
            tlut_hash_key = tlut_content_hash(g_gx.tlut[tlut_name].data,
                                              g_gx.tlut[tlut_name].format,
                                              g_gx.tlut[tlut_name].n_entries,
                                              g_gx.tlut[tlut_name].is_be);
        }
    }

#ifdef PC_ENHANCEMENTS
    /* EFB capture bypass: use full-res FBO texture instead of re-decoding */
    {
        GLuint efb_tex = pc_gx_efb_capture_find(o[TEXOBJ_IMAGE_PTR]);
        if (efb_tex) {
#ifdef TARGET_VITA
            if (!vita_on_worker_thread) {
                glActiveTexture(GL_TEXTURE7);
                glBindTexture(GL_TEXTURE_2D, efb_tex);
                GLenum gl_filter = filter_mode ? GL_LINEAR : GL_NEAREST;
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, gl_filter);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, gl_filter);
                glActiveTexture(GL_TEXTURE0);
            }
#else
            glBindTexture(GL_TEXTURE_2D, efb_tex);
            GLenum gl_filter = filter_mode ? GL_LINEAR : GL_NEAREST;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, gl_filter);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, gl_filter);
#endif
            o[TEXOBJ_GL_TEX] = efb_tex;
            g_gx.gl_textures[id] = efb_tex; g_gx.gl_tex_deferred[id] = -1;
            g_gx.tex_obj_w[id] = width;
            g_gx.tex_obj_h[id] = height;
            g_gx.tex_obj_fmt[id] = (int)format;
#ifdef TARGET_VITA
            g_gx.efb_v_flip = 1; // VitaGL glCopyTexImage2D is V-flipped
#endif
            DIRTY(PC_GX_DIRTY_TEXTURES);
            return;
        }
    }
#endif

#ifdef TARGET_VITA
    g_gx.efb_v_flip = 0;
#endif

    /* detect when emu64 reuses the same buffer with different data */
    u32 hash = tex_content_hash(image_ptr, width, height, format);

    /* cache lookup */
    TexCacheEntry* cached = tex_cache_find(o[TEXOBJ_IMAGE_PTR], width, height, format, tlut_key,
                                           tlut_ptr_key, tlut_hash_key, hash);
    if (cached) {
        tex_cache_hits++;
        GLuint tex = cached->gl_tex;
#ifdef TARGET_VITA
        // Update wrap/filter if changed. Main thread: immediate GL calls.
        // Worker thread: queue for deferred application.
        if (cached->wrap_s != wrap_s || cached->wrap_t != wrap_t ||
            cached->min_filter != filter_mode) {
            GLenum gl_ws = (wrap_s == 2) ? GL_MIRRORED_REPEAT :
                           (wrap_s == 0) ? GL_CLAMP_TO_EDGE : GL_REPEAT;
            GLenum gl_wt = (wrap_t == 2) ? GL_MIRRORED_REPEAT :
                           (wrap_t == 0) ? GL_CLAMP_TO_EDGE : GL_REPEAT;
            GLenum gl_filter = filter_mode ? GL_LINEAR : GL_NEAREST;

            if (!vita_on_worker_thread) {
                glActiveTexture(GL_TEXTURE7);
                glBindTexture(GL_TEXTURE_2D, tex);
                if (cached->wrap_s != wrap_s || cached->wrap_t != wrap_t) {
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gl_ws);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gl_wt);
                }
                if (cached->min_filter != filter_mode) {
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, gl_filter);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, gl_filter);
                }
                glActiveTexture(GL_TEXTURE0);
            } else {
                // worker thread: queue for main thread
                if (vita_tex_param_count < VITA_TEX_PARAM_MAX) {
                    VitaDeferredTexParam* p = &vita_tex_param_queue[vita_tex_param_count++];
                    p->tex_id = tex;
                    p->wrap_s = gl_ws;
                    p->wrap_t = gl_wt;
                    p->min_filter = gl_filter;
                }
            }
            cached->wrap_s = wrap_s;
            cached->wrap_t = wrap_t;
            cached->min_filter = filter_mode;
        }
#else
        glBindTexture(GL_TEXTURE_2D, tex);

        /* update wrap/filter if changed */
        if (cached->wrap_s != wrap_s || cached->wrap_t != wrap_t) {
            GLenum gl_ws = (wrap_s == 2) ? GL_MIRRORED_REPEAT :
                           (wrap_s == 0) ? GL_CLAMP_TO_EDGE : GL_REPEAT;
            GLenum gl_wt = (wrap_t == 2) ? GL_MIRRORED_REPEAT :
                           (wrap_t == 0) ? GL_CLAMP_TO_EDGE : GL_REPEAT;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gl_ws);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gl_wt);
            cached->wrap_s = wrap_s;
            cached->wrap_t = wrap_t;
        }
        if (cached->min_filter != filter_mode) {
            GLenum gl_filter = filter_mode ? GL_LINEAR : GL_NEAREST;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, gl_filter);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, gl_filter);
            cached->min_filter = filter_mode;
        }
#endif

        o[TEXOBJ_GL_TEX] = tex;
        g_gx.gl_textures[id] = tex; g_gx.gl_tex_deferred[id] = -1;
        g_gx.tex_obj_w[id] = width;
        g_gx.tex_obj_h[id] = height;
        g_gx.tex_obj_fmt[id] = (int)format;
        DIRTY(PC_GX_DIRTY_TEXTURES);
        return;
    }

    /* cache miss */
    tex_cache_misses++;

    /* try texture pack replacement before decoding */
    if (pc_texture_pack_active()
#ifdef TARGET_VITA
        && !vita_on_worker_thread  // texture pack GL calls not safe on worker
#endif
    ) {
        const void* tp_tlut = NULL;
        int tp_tlut_entries = 0;
        int tp_tlut_is_be = 1;
        if ((format == GX_TF_C4 || format == GX_TF_C8)) {
            int tlut_name = (int)o[TEXOBJ_TLUT_NAME];
            if (tlut_name >= 0 && tlut_name < 16 && g_gx.tlut[tlut_name].data) {
                tp_tlut = g_gx.tlut[tlut_name].data;
                tp_tlut_entries = g_gx.tlut[tlut_name].n_entries;
                tp_tlut_is_be = g_gx.tlut[tlut_name].is_be;
            }
        }
        int data_size = (width * height * gc_format_bpp(format)) / 8;
        int hd_w = 0, hd_h = 0;
        GLuint hd_tex = pc_texture_pack_lookup(image_ptr, data_size,
                                               width, height, format,
                                               tp_tlut, tp_tlut_entries, tp_tlut_is_be,
                                               &hd_w, &hd_h);
        if (hd_tex) {
            glBindTexture(GL_TEXTURE_2D, hd_tex);
            GLenum gl_ws = (wrap_s == 2) ? GL_MIRRORED_REPEAT :
                           (wrap_s == 0) ? GL_CLAMP_TO_EDGE : GL_REPEAT;
            GLenum gl_wt = (wrap_t == 2) ? GL_MIRRORED_REPEAT :
                           (wrap_t == 0) ? GL_CLAMP_TO_EDGE : GL_REPEAT;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gl_ws);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gl_wt);

            TexCacheEntry* entry = tex_cache_insert(o[TEXOBJ_IMAGE_PTR], width, height, format,
                                                    tlut_key, tlut_ptr_key, tlut_hash_key, hash, hd_tex);
            entry->wrap_s = wrap_s;
            entry->wrap_t = wrap_t;
            entry->min_filter = filter_mode;
            entry->external = 1;

            o[TEXOBJ_GL_TEX] = hd_tex;
            g_gx.gl_textures[id] = hd_tex; g_gx.gl_tex_deferred[id] = -1;
            g_gx.tex_obj_w[id] = width;
            g_gx.tex_obj_h[id] = height;
            g_gx.tex_obj_fmt[id] = (int)format;
            DIRTY(PC_GX_DIRTY_TEXTURES);
            return;
        }
    }

    // decode texture data (safe on any thread)
    u8* rgba = NULL;
    if (image_ptr && width > 0 && height > 0 && width <= 1024 && height <= 1024) {
        rgba = (u8*)malloc(width * height * 4);
        if (rgba) {
            u8 palette[256][4];
            if (format == GX_TF_C4 || format == GX_TF_C8) {
                int tlut_name = (int)o[TEXOBJ_TLUT_NAME];
                if (tlut_name >= 0 && tlut_name < 16 && g_gx.tlut[tlut_name].data) {
                    build_palette(g_gx.tlut[tlut_name].data,
                                  g_gx.tlut[tlut_name].format,
                                  g_gx.tlut[tlut_name].n_entries,
                                  palette,
                                  g_gx.tlut[tlut_name].is_be);
                } else {
                    build_palette(NULL, 0, 0, palette, 0);
                }
            } else {
                build_palette(NULL, 0, 0, palette, 0);
            }
            decode_gc_texture(image_ptr, rgba, width, height, format, palette);
        }
    }

#ifdef TARGET_VITA
    if (vita_on_worker_thread) {
        // Worker thread: queue upload for main thread, return tex_id=0 this frame.
        // Deduplicate: same data_ptr already queued this frame gets reused to
        // avoid N cache misses burning through the upload queue.
        int existing_idx = -1;
        {
            u32 dp = o[TEXOBJ_IMAGE_PTR];
            int cnt = vita_tex_upload_count_db[cmd_write];
            for (int qi = 0; qi < cnt; qi++) {
                if (vita_tex_upload_db[cmd_write][qi].data_ptr == dp &&
                    vita_tex_upload_db[cmd_write][qi].data_hash == hash) {
                    existing_idx = qi;
                    break;
                }
            }
        }
        if (existing_idx >= 0) {
            if (rgba) free(rgba);
            g_gx.gl_tex_deferred[id] = existing_idx;
            o[TEXOBJ_GL_TEX] = 0;
            g_gx.gl_textures[id] = 0;
            g_gx.tex_obj_w[id] = width;
            g_gx.tex_obj_h[id] = height;
            g_gx.tex_obj_fmt[id] = (int)format;
            DIRTY(PC_GX_DIRTY_TEXTURES);
            vita_texload_us += sceKernelGetProcessTimeLow() - _texload_t0;
            return;
        }
        if (vita_tex_upload_count < VITA_TEX_UPLOAD_MAX) {
            VitaDeferredTexUpload* up = &vita_tex_upload_queue[vita_tex_upload_count++];
            up->rgba = rgba;
            up->width = width;
            up->height = height;
            up->gl_filter = filter_mode ? GL_LINEAR : GL_NEAREST;
            up->gl_wrap_s = (wrap_s == 2) ? GL_MIRRORED_REPEAT :
                            (wrap_s == 0) ? GL_CLAMP_TO_EDGE : GL_REPEAT;
            up->gl_wrap_t = (wrap_t == 2) ? GL_MIRRORED_REPEAT :
                            (wrap_t == 0) ? GL_CLAMP_TO_EDGE : GL_REPEAT;
            up->data_ptr = o[TEXOBJ_IMAGE_PTR];
            up->cache_w = (u16)width;
            up->cache_h = (u16)height;
            up->format = format;
            up->tlut_name = tlut_key;
            up->tlut_ptr = tlut_ptr_key;
            up->tlut_hash = tlut_hash_key;
            up->data_hash = hash;
            up->wrap_s = wrap_s;
            up->wrap_t = wrap_t;
            up->min_filter = filter_mode;
            up->gl_tex_slot = id;
            up->compressed = 0;
            up->external = 0;
            // compute VTC cache key (no I/O, no GL -- safe on worker)
            {
                extern unsigned long long vita_vtc_compute_key(const void*, int, int, int,
                                                               unsigned int, const void*, int, int,
                                                               unsigned int, unsigned int, unsigned int);
                const void* tp_tlut = NULL;
                int tp_tlut_entries = 0;
                int tp_tlut_is_be = 1;
                if ((format == GX_TF_C4 || format == GX_TF_C8)) {
                    int tn = (int)o[TEXOBJ_TLUT_NAME];
                    if (tn >= 0 && tn < 16 && g_gx.tlut[tn].data) {
                        tp_tlut = g_gx.tlut[tn].data;
                        tp_tlut_entries = g_gx.tlut[tn].n_entries;
                        tp_tlut_is_be = g_gx.tlut[tn].is_be;
                    }
                }
                int dsz = (width * height * gc_format_bpp(format)) / 8;
                up->vtc_cache_key = vita_vtc_compute_key(image_ptr, dsz,
                                                          width, height, format,
                                                          tp_tlut, tp_tlut_entries, tp_tlut_is_be,
                                                          o[TEXOBJ_IMAGE_PTR], hash, tlut_hash_key);
            }
            // store deferred upload index for same-frame re-resolve
            g_gx.gl_tex_deferred[id] = vita_tex_upload_count_db[cmd_write] - 1;
        } else {
            if (rgba) free(rgba);
            g_gx.gl_tex_deferred[id] = -1;
        }
        o[TEXOBJ_GL_TEX] = 0;
        g_gx.gl_textures[id] = 0;
        g_gx.tex_obj_w[id] = width;
        g_gx.tex_obj_h[id] = height;
        g_gx.tex_obj_fmt[id] = (int)format;
        DIRTY(PC_GX_DIRTY_TEXTURES);
        vita_texload_us += sceKernelGetProcessTimeLow() - _texload_t0;
        return;
    }
#endif

    /* Main thread path: upload immediately */
    GLuint tex;
    glGenTextures(1, &tex);
#ifdef TARGET_VITA
    glActiveTexture(GL_TEXTURE7);
#endif
    glBindTexture(GL_TEXTURE_2D, tex);

    if (rgba) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        PC_GL_CHECK("glTexImage2D");
        free(rgba);
    } else {
        u8 white[4] = {255, 255, 255, 255};
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    }

    {
        GLenum gl_filter = filter_mode ? GL_LINEAR : GL_NEAREST;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, gl_filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, gl_filter);
    }

    {
        GLenum gl_ws = (wrap_s == 2) ? GL_MIRRORED_REPEAT :
                       (wrap_s == 0) ? GL_CLAMP_TO_EDGE : GL_REPEAT;
        GLenum gl_wt = (wrap_t == 2) ? GL_MIRRORED_REPEAT :
                       (wrap_t == 0) ? GL_CLAMP_TO_EDGE : GL_REPEAT;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gl_ws);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gl_wt);
    }

    /* insert into cache */
    TexCacheEntry* entry = tex_cache_insert(o[TEXOBJ_IMAGE_PTR], width, height, format, tlut_key,
                                            tlut_ptr_key, tlut_hash_key, hash, tex);
    entry->wrap_s = wrap_s;
    entry->wrap_t = wrap_t;
    entry->min_filter = filter_mode;

#ifdef TARGET_VITA
    // compute VTC cache key for background HD upgrade (same as worker path)
    {
        extern unsigned long long vita_vtc_compute_key(const void*, int, int, int,
                                                       unsigned int, const void*, int, int,
                                                       unsigned int, unsigned int, unsigned int);
        const void* tp_tlut = NULL;
        int tp_tlut_entries = 0;
        int tp_tlut_is_be = 1;
        if ((format == GX_TF_C4 || format == GX_TF_C8)) {
            int tn = (int)o[TEXOBJ_TLUT_NAME];
            if (tn >= 0 && tn < 16 && g_gx.tlut[tn].data) {
                tp_tlut = g_gx.tlut[tn].data;
                tp_tlut_entries = g_gx.tlut[tn].n_entries;
                tp_tlut_is_be = g_gx.tlut[tn].is_be;
            }
        }
        int dsz = (width * height * gc_format_bpp(format)) / 8;
        entry->vtc_cache_key = vita_vtc_compute_key(image_ptr, dsz,
                                                     width, height, format,
                                                     tp_tlut, tp_tlut_entries, tp_tlut_is_be,
                                                     o[TEXOBJ_IMAGE_PTR], hash, tlut_hash_key);
    }
    glActiveTexture(GL_TEXTURE0);
#endif
    o[TEXOBJ_GL_TEX] = tex;
    g_gx.gl_textures[id] = tex; g_gx.gl_tex_deferred[id] = -1;
    g_gx.tex_obj_w[id] = width;
    g_gx.tex_obj_h[id] = height;
    g_gx.tex_obj_fmt[id] = (int)format;
    DIRTY(PC_GX_DIRTY_TEXTURES);
#ifdef TARGET_VITA
    {
        extern unsigned int vita_texload_us;
        vita_texload_us += sceKernelGetProcessTimeLow() - _texload_t0;
    }
#endif
}

u32 GXGetTexBufferSize(u16 width, u16 height, u32 format, GXBool mipmap, u8 max_lod) {
    u32 bpp = (u32)gc_format_bpp(format);
    u32 total = (width * height * bpp) / 8;
    if (mipmap) {
        u32 w = width, h = height;
        for (u8 lod = 1; lod <= max_lod && (w > 1 || h > 1); lod++) {
            if (w > 1) w >>= 1;
            if (h > 1) h >>= 1;
            total += (w * h * bpp) / 8;
        }
    }
    return total;
}

void GXInvalidateTexAll(void) {
    /* no-op: PC cache keys on pointer+format, no TMEM to flush */
}
void GXInvalidateTexRegion(void* region) { (void)region; }

/* --- TLUT --- */

void GXInitTlutObj(void* obj, void* lut, u32 fmt, u16 n_entries) {
    u32* o = (u32*)obj;
    memset(o, 0, 4 * sizeof(u32));
    o[TLUTOBJ_DATA] = (u32)(uintptr_t)lut;
    o[TLUTOBJ_FORMAT] = fmt;
    o[TLUTOBJ_N_ENTRIES] = n_entries;
}

void GXLoadTlut(void* obj, u32 idx) {
    pc_gx_flush_if_begin_complete();
    if (idx >= 16) return;
    u32* o = (u32*)obj;
    g_gx.tlut[idx].data = (const void*)(uintptr_t)o[TLUTOBJ_DATA];
    g_gx.tlut[idx].format = (int)o[TLUTOBJ_FORMAT];
    g_gx.tlut[idx].n_entries = (int)o[TLUTOBJ_N_ENTRIES];
    g_gx.tlut[idx].is_be = 1; /* default to BE (ROM/JSystem data) */
}

/* PC_GX_TLUT_MODE env var: "be" forces BE decode for diagnostics */
static int pc_gx_tlut_force_be(void) {
    static int init = 0;
    static int force_be = 0;
    if (!init) {
        const char* mode = getenv("PC_GX_TLUT_MODE");
        if (mode != NULL) {
            if (mode[0] == 'b' || mode[0] == 'B') {
                force_be = 1;
            }
        }
        init = 1;
    }
    return force_be;
}

/* Mark a TLUT slot as native-LE (from emu64 tlutconv) */
void pc_gx_tlut_set_native_le(unsigned int idx) {
    if (idx < 16) {
        if (pc_gx_tlut_force_be()) {
            g_gx.tlut[idx].is_be = 1;
        } else {
            g_gx.tlut[idx].is_be = 0;
        }
    }
}

void GXInitTexCacheRegion(void* region, GXBool is_32b, u32 tmem_even, u32 size_even,
                          u32 tmem_odd, u32 size_odd) {
    (void)region; (void)is_32b; (void)tmem_even; (void)size_even; (void)tmem_odd; (void)size_odd;
}

void* GXSetTexRegionCallback(void* callback) { return NULL; }
void GXInitTlutRegion(void* region, u32 tmem_addr, u32 tlut_size) {
    (void)region; (void)tmem_addr; (void)tlut_size;
}

/* --- accessors --- */
GXBool GXGetTexObjMipMap(const void* obj) { return ((const u32*)obj)[TEXOBJ_MIPMAP] != 0; }
u32    GXGetTexObjFmt(const void* obj)    { return ((const u32*)obj)[TEXOBJ_FORMAT]; }
u16    GXGetTexObjHeight(const void* obj) { return (u16)((const u32*)obj)[TEXOBJ_HEIGHT]; }
u16    GXGetTexObjWidth(const void* obj)  { return (u16)((const u32*)obj)[TEXOBJ_WIDTH]; }
u32    GXGetTexObjWrapS(const void* obj)  { return ((const u32*)obj)[TEXOBJ_WRAP_S]; }
u32    GXGetTexObjWrapT(const void* obj)  { return ((const u32*)obj)[TEXOBJ_WRAP_T]; }
void*  GXGetTexObjData(const void* obj)   { return (void*)(uintptr_t)((const u32*)obj)[TEXOBJ_IMAGE_PTR]; }

void GXDestroyTexObj(void* obj) {
    u32* o = (u32*)obj;
    /* don't delete GL texture here; cache eviction handles that */
    o[TEXOBJ_GL_TEX] = 0;
}

void GXDestroyTlutObj(void* obj) { (void)obj; }
