// vita_vtc.c
// HD texture cache: pre-built DXT binary, hardware-decoded by PowerVR GPU
#ifdef TARGET_VITA

#include "pc_texture_pack.h"
#include "pc_gx_internal.h"
#include "pc_settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// VTC file format
#define VTC_MAGIC   0x56544331  // "VTC1"
#define VTC_VERSION 2

// Resolved VTC path from settings (built once at init, shared by both file handles)
static char g_vtc_path[128] = "";
#define VTC_FMT_DXT1    1
#define VTC_FMT_PVRTC4  4   // PVRTC V1 4bpp, native PowerVR, square POT
#define VTC_FMT_DXT5    5
#define VTC_FMT_PVRTCII 6   // PVRTCII V2 4bpp, native PowerVR, non-square POT

#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT  0x83F1
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT  0x83F3
#endif
#ifndef GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG
#define GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG  0x8C02
#endif
#ifndef GL_COMPRESSED_RGBA_PVRTC_4BPPV2_IMG
#define GL_COMPRESSED_RGBA_PVRTC_4BPPV2_IMG  0x9138
#endif

// Map VTC format byte to GL compressed format enum
static GLenum vtc_fmt_to_gl(unsigned char fmt) {
    switch (fmt) {
        case VTC_FMT_DXT1:    return GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
        case VTC_FMT_DXT5:    return GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
        case VTC_FMT_PVRTC4:  return GL_COMPRESSED_RGBA_PVRTC_4BPPV1_IMG;
        case VTC_FMT_PVRTCII: return GL_COMPRESSED_RGBA_PVRTC_4BPPV2_IMG;
        default:              return GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
    }
}

#pragma pack(push, 1)
typedef struct {
    unsigned long long cache_key;
    unsigned int offset;
    unsigned int dxt_size;
    unsigned short hd_width;
    unsigned short hd_height;
    unsigned char format;      // 1=DXT1, 5=DXT5
    unsigned char padding[3];
} VtcIndexEntry;
#pragma pack(pop)

// State
// Worker thread file handle (only file handle, no main-thread I/O)
static FILE* g_vtc_file_worker = NULL;
static VtcIndexEntry* g_vtc_index = NULL;
static int g_vtc_count = 0;
static int g_vtc_active = 0;

// Loaded cache with vRAM budget + LRU eviction
#define VTC_LOADED_CACHE_SIZE 4096
#define VTC_LOADED_CACHE_MASK (VTC_LOADED_CACHE_SIZE - 1)
#define VTC_VRAM_BUDGET (24 * 1024 * 1024)  // 24 MB max for HD textures

typedef struct {
    unsigned long long key;
    GLuint gl_tex;
    int tex_w, tex_h;
    int vram_bytes;      // vRAM consumed by this texture
    unsigned int last_used; // frame counter for LRU
    int occupied;
} VtcLoadedEntry;

static VtcLoadedEntry g_vtc_loaded[VTC_LOADED_CACHE_SIZE];
static int g_vtc_vram_used = 0;       // total vRAM used by loaded HD textures
static unsigned int g_vtc_frame = 0;  // frame counter for LRU
static int g_vtc_evictions = 0;       // stat: total LRU evictions

// Negative cache: skip textures not in the pack
#define VTC_NEG_CACHE_SIZE 2048
#define VTC_NEG_CACHE_MASK (VTC_NEG_CACHE_SIZE - 1)
static unsigned long long g_vtc_neg[VTC_NEG_CACHE_SIZE];
static int g_vtc_neg_valid[VTC_NEG_CACHE_SIZE];

// XXHash64 result cache
// Eliminates repeat XXHash64 computations for textures already seen.
// Keyed by (data_ptr, data_hash_fnv, tlut_hash_fnv, w, h, fmt) which are
// all available from the tex_cache without any hashing. On cache hit, we
// return the pre-computed vtc_cache_key instantly (no XXHash64, no binary search).
// data_hash_fnv detects buffer reuse (same data_ptr, different content).
#define VTC_KEY_CACHE_SIZE 1024
#define VTC_KEY_CACHE_MASK (VTC_KEY_CACHE_SIZE - 1)
typedef struct {
    unsigned int data_ptr;
    unsigned int data_hash_fnv;
    unsigned int tlut_hash_fnv;
    unsigned short w, h;
    unsigned int fmt;
    unsigned long long vtc_key; // result: 0 = not in VTC pack
    int occupied;
} VtcKeyCacheEntry;
static VtcKeyCacheEntry g_vtc_key_cache[VTC_KEY_CACHE_SIZE];

static unsigned int vtc_key_cache_slot(unsigned int data_ptr, unsigned int data_hash_fnv) {
    unsigned int h = data_ptr ^ (data_hash_fnv * 0x9E3779B1u);
    h ^= h >> 16;
    return h & VTC_KEY_CACHE_MASK;
}

// Stats (main + worker combined)
static int g_vtc_stat_lookups = 0;
static int g_vtc_stat_hits = 0;
static int g_vtc_stat_loaded = 0;
static int g_vtc_stat_cache_hits = 0;
static int g_vtc_stat_neg_hits = 0;
static int g_vtc_stat_worker_lookups = 0;
static int g_vtc_stat_worker_hits = 0;
static int g_vtc_stat_worker_neg = 0;
static int g_vtc_compute_calls = 0;
static int g_vtc_compute_hits = 0;
static int g_vtc_keycache_hits = 0;
static int g_vtc_io_preread_hits = 0;  // I/O thread pre-read was ready (main thread found READY slot)
static int g_vtc_sync_reads = 0;       // fell back to sync fread on main thread
static volatile int g_vtc_io_completed = 0;
static volatile int g_vtc_io_queued = 0;

// Diagnostic timing (set by pc_gx_texture.c upgrade scan)
unsigned int vita_vtc_upgrade_us = 0;
int vita_vtc_upgrade_count = 0;
int vita_vtc_requeue_count = 0;
unsigned int vita_vtc_prefetch_us = 0;  // accumulated prefetch I/O time (worker idle)

// Prefetch pending queue
// Populated by vita_vtc_compute_key (worker thread),
// drained by vita_vtc_prefetch (worker thread idle time after signal done)
#define VTC_PREFETCH_MAX 64
static unsigned long long g_vtc_prefetch_keys[VTC_PREFETCH_MAX];
static volatile int g_vtc_prefetch_count = 0;

// I/O slot definitions (used by lookup_by_key and worker prefetch)
#include <psp2/kernel/threadmgr.h>

#define VTC_IO_SLOTS 24
#define VTC_IO_BUF_SIZE (256 * 1024)

enum { VTC_SLOT_FREE = 0, VTC_SLOT_QUEUED, VTC_SLOT_READING, VTC_SLOT_READY };

typedef struct {
    unsigned long long key;
    unsigned char* buffer;
    unsigned char* malloc_buf;
    int dxt_size;
    unsigned short hd_w, hd_h;
    unsigned char dxt_format;
    volatile int state;
    unsigned int file_offset;
} VtcIoSlot;

static VtcIoSlot g_vtc_io_slots[VTC_IO_SLOTS];
static unsigned char* g_vtc_io_pool = NULL; // heap-allocated to avoid BSS bloat
static SceUID g_vtc_io_thread = -1;
static SceUID g_vtc_io_sema = -1;
static volatile int g_vtc_io_shutdown = 0;
static FILE* g_vtc_file_io = NULL;
void vita_vtc_release_slot(int slot); // forward declaration

// Forward declaration
extern void pc_gx_texture_invalidate_gl_tex(GLuint tex);

// XXHash64 (seed=0, matches Dolphin's GetHash64)

typedef unsigned long long xxh_u64;
typedef unsigned int xxh_u32;

#define XXH_PRIME64_1 0x9E3779B185EBCA87ULL
#define XXH_PRIME64_2 0xC2B2AE3D27D4EB4FULL
#define XXH_PRIME64_3 0x165667B19E3779F9ULL
#define XXH_PRIME64_4 0x85EBCA77C2B2AE63ULL
#define XXH_PRIME64_5 0x27D4EB2F165667C5ULL

static inline xxh_u64 xxh_read64(const void* p) { xxh_u64 v; memcpy(&v, p, 8); return v; }
static inline xxh_u32 xxh_read32(const void* p) { xxh_u32 v; memcpy(&v, p, 4); return v; }
static inline xxh_u64 xxh_rotl64(xxh_u64 x, int r) { return (x << r) | (x >> (64 - r)); }

static inline xxh_u64 xxh_round(xxh_u64 acc, xxh_u64 input) {
    acc += input * XXH_PRIME64_2; acc = xxh_rotl64(acc, 31); acc *= XXH_PRIME64_1; return acc;
}
static inline xxh_u64 xxh_merge_round(xxh_u64 acc, xxh_u64 val) {
    val = xxh_round(0, val); acc ^= val; acc = acc * XXH_PRIME64_1 + XXH_PRIME64_4; return acc;
}

static xxh_u64 xxhash64(const void* input, int len) {
    const unsigned char* p = (const unsigned char*)input;
    const unsigned char* end = p + len;
    xxh_u64 h64;
    if (len >= 32) {
        const unsigned char* limit = end - 32;
        xxh_u64 v1 = XXH_PRIME64_1 + XXH_PRIME64_2, v2 = XXH_PRIME64_2, v3 = 0, v4 = 0 - XXH_PRIME64_1;
        do {
            v1 = xxh_round(v1, xxh_read64(p)); p += 8;
            v2 = xxh_round(v2, xxh_read64(p)); p += 8;
            v3 = xxh_round(v3, xxh_read64(p)); p += 8;
            v4 = xxh_round(v4, xxh_read64(p)); p += 8;
        } while (p <= limit);
        h64 = xxh_rotl64(v1,1) + xxh_rotl64(v2,7) + xxh_rotl64(v3,12) + xxh_rotl64(v4,18);
        h64 = xxh_merge_round(h64, v1); h64 = xxh_merge_round(h64, v2);
        h64 = xxh_merge_round(h64, v3); h64 = xxh_merge_round(h64, v4);
    } else {
        h64 = XXH_PRIME64_5;
    }
    h64 += (xxh_u64)len;
    while (p + 8 <= end) { h64 ^= xxh_round(0, xxh_read64(p)); h64 = xxh_rotl64(h64,27) * XXH_PRIME64_1 + XXH_PRIME64_4; p += 8; }
    if (p + 4 <= end) { h64 ^= (xxh_u64)xxh_read32(p) * XXH_PRIME64_1; h64 = xxh_rotl64(h64,23) * XXH_PRIME64_2 + XXH_PRIME64_3; p += 4; }
    while (p < end) { h64 ^= (*p) * XXH_PRIME64_5; h64 = xxh_rotl64(h64,11) * XXH_PRIME64_1; p++; }
    h64 ^= h64 >> 33; h64 *= XXH_PRIME64_2; h64 ^= h64 >> 29; h64 *= XXH_PRIME64_3; h64 ^= h64 >> 32;
    return h64;
}

// GC texture block-aligned data size (for hash input length)
static int gc_texture_data_size(int w, int h, unsigned int fmt) {
    int bw, bh, bb;
    switch (fmt) {
        case 0: case 8:  bw=8; bh=8; bb=32; break;  // I4, C4
        case 14:         bw=8; bh=8; bb=32; break;  // CMPR
        case 1: case 2: case 9: bw=8; bh=4; bb=32; break;  // I8, IA4, C8
        case 3: case 4: case 5: bw=4; bh=4; bb=32; break;  // IA8, RGB565, RGB5A3
        case 6:          bw=4; bh=4; bb=64; break;  // RGBA8
        default:         bw=8; bh=4; bb=32; break;
    }
    return ((w+bw-1)/bw) * ((h+bh-1)/bh) * bb;
}

// Cache key (same formula as pc_texture_pack.c)
static unsigned long long vtc_cache_key(unsigned long long data_hash, unsigned long long tlut_hash,
                                         unsigned int fmt, unsigned int w, unsigned int h) {
    unsigned long long k = data_hash;
    k ^= tlut_hash * 0x517CC1B727220A95ULL;
    k ^= (unsigned long long)fmt * 0x6C62272E07BB0142ULL;
    k ^= (unsigned long long)w * 0x165667B19E3779F9ULL;
    k ^= (unsigned long long)h * 0x85EBCA77C2B2AE63ULL;
    return k;
}

// Loaded cache ops (with LRU eviction + vRAM budget)

// Estimate vRAM for a compressed texture
int vtc_estimate_vram(int w, int h, int fmt) {
    // DXT5 = 8bpp, DXT1/PVRTC4/PVRTCII = 4bpp
    if (fmt == VTC_FMT_DXT5) {
        int blocks_x = (w + 3) / 4;
        int blocks_y = (h + 3) / 4;
        return blocks_x * blocks_y * 16;
    }
    // PVRTC4 and DXT1 are both 4bpp
    int blocks_x = (w + 3) / 4;
    int blocks_y = (h + 3) / 4;
    return blocks_x * blocks_y * 8;
}

static VtcLoadedEntry* vtc_loaded_find(unsigned long long key) {
    unsigned int slot = (unsigned int)(key & VTC_LOADED_CACHE_MASK);
    for (int i = 0; i < VTC_LOADED_CACHE_SIZE; i++) {
        unsigned int idx = (slot + i) & VTC_LOADED_CACHE_MASK;
        if (!g_vtc_loaded[idx].occupied) return NULL;
        if (g_vtc_loaded[idx].key == key) {
            g_vtc_loaded[idx].last_used = g_vtc_frame; // touch for LRU
            return &g_vtc_loaded[idx];
        }
    }
    return NULL;
}

// Evict oldest entry from loaded cache to free vRAM
static void vtc_loaded_evict_oldest(void) {
    int oldest_idx = -1;
    unsigned int oldest_frame = 0xFFFFFFFF;

    for (int i = 0; i < VTC_LOADED_CACHE_SIZE; i++) {
        if (g_vtc_loaded[i].occupied && g_vtc_loaded[i].last_used < oldest_frame) {
            oldest_frame = g_vtc_loaded[i].last_used;
            oldest_idx = i;
        }
    }
    if (oldest_idx < 0) return;

    VtcLoadedEntry* victim = &g_vtc_loaded[oldest_idx];

    pc_gx_texture_invalidate_gl_tex(victim->gl_tex);

    // Defer-delete the GL texture (GPU may still be rendering with it)
    extern void vita_defer_tex_delete(GLuint tex);
    vita_defer_tex_delete(victim->gl_tex);
    g_vtc_vram_used -= victim->vram_bytes;
    g_vtc_evictions++;

    // Clear the slot. We can't just zero it because open-addressing
    // hash table requires tombstone or rehash. Simple approach: mark as
    // unoccupied. This may break chains but is safe since lookups always
    // fall back to VTC file read on miss.
    victim->occupied = 0;
    victim->gl_tex = 0;
    victim->key = 0;
}

void vtc_loaded_insert(unsigned long long key, GLuint tex, int w, int h, int vram_bytes) {
    // Enforce vRAM budget. Evict LRU entries until we fit.
    while (g_vtc_vram_used + vram_bytes > VTC_VRAM_BUDGET) {
        vtc_loaded_evict_oldest();
        if (g_vtc_vram_used <= 0) break; // safety: nothing left to evict
    }

    unsigned int slot = (unsigned int)(key & VTC_LOADED_CACHE_MASK);
    for (int i = 0; i < VTC_LOADED_CACHE_SIZE; i++) {
        unsigned int idx = (slot + i) & VTC_LOADED_CACHE_MASK;
        if (!g_vtc_loaded[idx].occupied) {
            g_vtc_loaded[idx].key = key;
            g_vtc_loaded[idx].gl_tex = tex;
            g_vtc_loaded[idx].tex_w = w;
            g_vtc_loaded[idx].tex_h = h;
            g_vtc_loaded[idx].vram_bytes = vram_bytes;
            g_vtc_loaded[idx].last_used = g_vtc_frame;
            g_vtc_loaded[idx].occupied = 1;
            g_vtc_vram_used += vram_bytes;
            return;
        }
    }
    // Cache completely full with no free slots. Evict one more and retry.
    vtc_loaded_evict_oldest();
    slot = (unsigned int)(key & VTC_LOADED_CACHE_MASK);
    for (int i = 0; i < VTC_LOADED_CACHE_SIZE; i++) {
        unsigned int idx = (slot + i) & VTC_LOADED_CACHE_MASK;
        if (!g_vtc_loaded[idx].occupied) {
            g_vtc_loaded[idx].key = key;
            g_vtc_loaded[idx].gl_tex = tex;
            g_vtc_loaded[idx].tex_w = w;
            g_vtc_loaded[idx].tex_h = h;
            g_vtc_loaded[idx].vram_bytes = vram_bytes;
            g_vtc_loaded[idx].last_used = g_vtc_frame;
            g_vtc_loaded[idx].occupied = 1;
            g_vtc_vram_used += vram_bytes;
            return;
        }
    }
}

// Call once per frame to advance LRU counter
void vita_vtc_tick_frame(void) {
    g_vtc_frame++;
}

// Negative cache ops
static int vtc_neg_check(unsigned long long key) {
    unsigned int slot = (unsigned int)(key & VTC_NEG_CACHE_MASK);
    return g_vtc_neg_valid[slot] && g_vtc_neg[slot] == key;
}

static void vtc_neg_insert(unsigned long long key) {
    unsigned int slot = (unsigned int)(key & VTC_NEG_CACHE_MASK);
    g_vtc_neg[slot] = key;
    g_vtc_neg_valid[slot] = 1;
}

// Binary search VTC index
static VtcIndexEntry* vtc_find(unsigned long long cache_key) {
    int lo = 0, hi = g_vtc_count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (g_vtc_index[mid].cache_key == cache_key) return &g_vtc_index[mid];
        if (g_vtc_index[mid].cache_key < cache_key) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

// TLUT hash computation (Dolphin-compatible)
static unsigned long long vtc_compute_tlut_hash(const void* image_data, int hash_size,
                                                  const void* tlut_data, int tlut_entries,
                                                  int tlut_is_be) {
    if (!tlut_data || tlut_entries <= 0) return 0;

    const unsigned char* tex = (const unsigned char*)image_data;
    unsigned int pal_min = 0xFFFF, pal_max = 0;

    if (tlut_entries <= 16) { // CI4
        for (int i = 0; i < hash_size; i++) {
            unsigned int lo = tex[i] & 0xF, hi = tex[i] >> 4;
            if (lo < pal_min) pal_min = lo; if (hi < pal_min) pal_min = hi;
            if (lo > pal_max) pal_max = lo; if (hi > pal_max) pal_max = hi;
        }
    } else if (tlut_entries <= 256) { // CI8
        for (int i = 0; i < hash_size; i++) {
            unsigned int idx = tex[i];
            if (idx < pal_min) pal_min = idx; if (idx > pal_max) pal_max = idx;
        }
    } else { // CI14x2
        for (int i = 0; i + 1 < hash_size; i += 2) {
            unsigned int idx = ((unsigned int)tex[i] << 8 | tex[i+1]) & 0x3FFF;
            if (idx < pal_min) pal_min = idx; if (idx > pal_max) pal_max = idx;
        }
    }

    if (pal_min > pal_max) return 0;

    int used = (int)(pal_max + 1 - pal_min);
    int toff = (int)(pal_min * 2);
    int tbytes = used * 2;
    if (toff + tbytes > tlut_entries * 2) tbytes = tlut_entries * 2 - toff;
    if (tbytes <= 0) tbytes = tlut_entries * 2;

    const unsigned char* tsrc = (const unsigned char*)tlut_data + toff;
    if (tlut_is_be) {
        return xxhash64(tsrc, tbytes);
    } else {
        unsigned char* tmp = (unsigned char*)malloc(tbytes);
        if (!tmp) return 0;
        for (int i = 0; i < tbytes; i += 2) {
            tmp[i] = tsrc[i + 1]; tmp[i + 1] = tsrc[i];
        }
        unsigned long long h = xxhash64(tmp, tbytes);
        free(tmp);
        return h;
    }
}


void vita_vtc_init(void) {
    g_vtc_count = 0;
    g_vtc_active = 0;
    memset(g_vtc_loaded, 0, sizeof(g_vtc_loaded));
    memset(g_vtc_neg_valid, 0, sizeof(g_vtc_neg_valid));
    memset(g_vtc_key_cache, 0, sizeof(g_vtc_key_cache));

    // Build VTC path from settings. Empty texture_pack = no HD textures.
    if (g_pc_settings.texture_pack[0]) {
        snprintf(g_vtc_path, sizeof(g_vtc_path),
                 "ux0:data/AnimalCrossing/texture_packs/%s.vtc",
                 g_pc_settings.texture_pack);
    } else {
        g_vtc_path[0] = '\0';
        printf("[VTC] No texture pack selected\n");
        return;
    }

    // Use a temporary FILE handle to read header + index at init.
    // Only the worker thread handle is kept open for runtime reads.
    FILE* init_file = fopen(g_vtc_path, "rb");
    if (!init_file) {
        printf("[VTC] No cache file found at %s\n", g_vtc_path);
        return;
    }

    // Read header
    unsigned int header[4];
    if (fread(header, 4, 4, init_file) != 4) {
        printf("[VTC] Invalid header\n");
        fclose(init_file);
        return;
    }
    if (header[0] != VTC_MAGIC || header[1] != VTC_VERSION) {
        printf("[VTC] Wrong magic/version: %08X v%d\n", header[0], header[1]);
        fclose(init_file);
        return;
    }

    int count = (int)header[2];
    if (count <= 0 || count > 100000) {
        printf("[VTC] Invalid count: %d\n", count);
        fclose(init_file);
        return;
    }

    // Load index table
    g_vtc_index = (VtcIndexEntry*)malloc(count * sizeof(VtcIndexEntry));
    if (!g_vtc_index) {
        printf("[VTC] Failed to allocate index (%d entries, %d KB)\n",
               count, (int)(count * sizeof(VtcIndexEntry) / 1024));
        fclose(init_file);
        return;
    }
    if ((int)fread(g_vtc_index, sizeof(VtcIndexEntry), count, init_file) != count) {
        printf("[VTC] Failed to read index table\n");
        free(g_vtc_index); g_vtc_index = NULL;
        fclose(init_file);
        return;
    }
    fclose(init_file); // done reading, no main-thread file handle kept open

    // Open worker thread file handle (the ONLY runtime file handle)
    g_vtc_file_worker = fopen(g_vtc_path, "rb");
    if (!g_vtc_file_worker) {
        printf("[VTC] WARNING: failed to open worker file handle, worker thread HD textures disabled\n");
    }

    g_vtc_count = count;
    g_vtc_active = 1;
    // intentionally no printf, stdout doesn't work on Vita
}

void vita_vtc_shutdown(void) {
    if (g_vtc_active) {
        printf("[VTC] Stats: %d lookups, %d hits, %d loaded, %d cache hits, %d neg skips, %d evictions, %dKB vRAM\n",
               g_vtc_stat_lookups, g_vtc_stat_hits, g_vtc_stat_loaded,
               g_vtc_stat_cache_hits, g_vtc_stat_neg_hits,
               g_vtc_evictions, g_vtc_vram_used / 1024);
    }

    for (int i = 0; i < VTC_LOADED_CACHE_SIZE; i++) {
        if (g_vtc_loaded[i].occupied && g_vtc_loaded[i].gl_tex)
            glDeleteTextures(1, &g_vtc_loaded[i].gl_tex);
    }
    memset(g_vtc_loaded, 0, sizeof(g_vtc_loaded));
    memset(g_vtc_neg_valid, 0, sizeof(g_vtc_neg_valid));

    if (g_vtc_index) { free(g_vtc_index); g_vtc_index = NULL; }
    if (g_vtc_file_worker) { fclose(g_vtc_file_worker); g_vtc_file_worker = NULL; }
    g_vtc_count = 0;
    g_vtc_active = 0;
}

// Return GL texture from loaded_cache if key exists, 0 otherwise.
GLuint vita_vtc_loaded_cache_lookup(unsigned long long key) {
    if (!g_vtc_active || key == 0) return 0;
    VtcLoadedEntry* e = vtc_loaded_find(key);
    return e ? e->gl_tex : 0;
}

// Re-queue a key for prefetch if not already in loaded_cache or an I/O slot.
// Called from upgrade scan for textures that missed the initial prefetch window.
void vita_vtc_requeue_prefetch(unsigned long long key) {
    if (!g_vtc_active || key == 0) return;
    // Skip if already in loaded_cache
    if (vtc_loaded_find(key)) return;
    // Skip if already in an I/O slot
    for (int si = 0; si < VTC_IO_SLOTS; si++) {
        if (g_vtc_io_slots[si].key == key && g_vtc_io_slots[si].state != VTC_SLOT_FREE)
            return;
    }
    // Skip if not in VTC index
    if (!vtc_find(key)) return;
    // Queue for prefetch (worker thread will read during next idle window)
    if (g_vtc_prefetch_count < VTC_PREFETCH_MAX) {
        g_vtc_prefetch_keys[g_vtc_prefetch_count++] = key;
    }
}

int vita_vtc_active(void) {
    return g_vtc_active;
}

// Main thread lookup: check loaded_cache only, NO file I/O.
// Returns HD texture from loaded_cache if available, 0 otherwise.
// File reads happen exclusively on the worker thread via prefetch.
GLuint vita_vtc_lookup(const void* data, int data_size,
                        int w, int h, unsigned int fmt,
                        const void* tlut_data, int tlut_entries, int tlut_is_be,
                        int* out_w, int* out_h) {
    if (!g_vtc_active || !data || data_size <= 0) return 0;

    // Compute hashes
    int hash_size = gc_texture_data_size(w, h, fmt);
    if (hash_size > data_size) hash_size = data_size;
    unsigned long long data_hash = xxhash64(data, hash_size);
    unsigned long long tlut_hash = vtc_compute_tlut_hash(data, hash_size, tlut_data, tlut_entries, tlut_is_be);

    unsigned long long key = vtc_cache_key(data_hash, tlut_hash, fmt, (unsigned int)w, (unsigned int)h);

    // Loaded cache: check exact key then wildcard
    VtcLoadedEntry* loaded = vtc_loaded_find(key);
    if (!loaded && tlut_hash != 0) {
        unsigned long long wc_key = vtc_cache_key(data_hash, 0, fmt, (unsigned int)w, (unsigned int)h);
        loaded = vtc_loaded_find(wc_key);
    }
    if (loaded) {
        g_vtc_stat_cache_hits++;
        if (out_w) *out_w = loaded->tex_w;
        if (out_h) *out_h = loaded->tex_h;
        return loaded->gl_tex;
    }

    // Not in loaded_cache. Return 0, let worker thread handle via prefetch.
    return 0;
}

// Worker thread lookup: reads DXT into buffer for deferred upload (no GL calls).
// Returns malloc'd DXT data. Caller must free. Sets out fields for deferred upload.
unsigned char* vita_vtc_lookup_deferred(const void* data, int data_size,
                                         int w, int h, unsigned int fmt,
                                         const void* tlut_data, int tlut_entries, int tlut_is_be,
                                         int* out_w, int* out_h,
                                         int* out_dxt_size, int* out_dxt_format) {
    if (!g_vtc_active || !data || data_size <= 0) return NULL;

    int hash_size = gc_texture_data_size(w, h, fmt);
    if (hash_size > data_size) hash_size = data_size;
    unsigned long long data_hash = xxhash64(data, hash_size);
    unsigned long long tlut_hash = vtc_compute_tlut_hash(data, hash_size, tlut_data, tlut_entries, tlut_is_be);

    unsigned long long key = vtc_cache_key(data_hash, tlut_hash, fmt, (unsigned int)w, (unsigned int)h);

    // Check loaded cache (exact + wildcard)
    if (vtc_loaded_find(key)) return NULL; // already loaded, main thread will use it
    if (tlut_hash != 0) {
        unsigned long long wc_key = vtc_cache_key(data_hash, 0, fmt, (unsigned int)w, (unsigned int)h);
        if (vtc_loaded_find(wc_key)) return NULL;
    }

    // Binary search VTC index (exact + wildcard, skip neg-cached)
    g_vtc_stat_worker_lookups++;

    VtcIndexEntry* entry = NULL;
    if (!vtc_neg_check(key))
        entry = vtc_find(key);
    if (!entry && tlut_hash != 0) {
        unsigned long long wc_key = vtc_cache_key(data_hash, 0, fmt, (unsigned int)w, (unsigned int)h);
        if (!vtc_neg_check(wc_key)) {
            entry = vtc_find(wc_key);
            if (entry) key = wc_key;
        }
    }
    if (!entry) {
        g_vtc_stat_worker_neg++;
        vtc_neg_insert(key);
        return NULL;
    }
    g_vtc_stat_worker_hits++;

    // Read DXT data from file (worker thread handle, separate from main)
    if (!g_vtc_file_worker) return NULL;
    unsigned char* dxt = (unsigned char*)malloc(entry->dxt_size);
    if (!dxt) return NULL;

    fseek(g_vtc_file_worker, entry->offset, SEEK_SET);
    if (fread(dxt, 1, entry->dxt_size, g_vtc_file_worker) != entry->dxt_size) {
        free(dxt);
        return NULL;
    }

    *out_w = entry->hd_width;
    *out_h = entry->hd_height;
    *out_dxt_size = (int)entry->dxt_size;
    *out_dxt_format = vtc_fmt_to_gl(entry->format);
    return dxt;
}

// Compute VTC cache_key for a texture (worker thread safe, no I/O, no GL).
// Returns non-zero key if VTC has a match, 0 if not found.
//
// data_ptr + data_hash_fnv + tlut_hash_fnv identify the texture content cheaply
// (already computed by tex_cache). On repeat calls for the same content, the
// XXHash64 result cache returns instantly with no re-hashing.
unsigned long long vita_vtc_compute_key(const void* data, int data_size,
                                         int w, int h, unsigned int fmt,
                                         const void* tlut_data, int tlut_entries, int tlut_is_be,
                                         unsigned int data_ptr, unsigned int data_hash_fnv,
                                         unsigned int tlut_hash_fnv) {
    g_vtc_compute_calls++;
    if (!g_vtc_active || !data || data_size <= 0) return 0;

    // Check XXHash64 result cache
    unsigned int kc_slot = vtc_key_cache_slot(data_ptr, data_hash_fnv);
    for (int i = 0; i < 4; i++) { // linear probe up to 4 slots
        unsigned int idx = (kc_slot + i) & VTC_KEY_CACHE_MASK;
        VtcKeyCacheEntry* kc = &g_vtc_key_cache[idx];
        if (!kc->occupied) break;
        if (kc->data_ptr == data_ptr && kc->data_hash_fnv == data_hash_fnv &&
            kc->tlut_hash_fnv == tlut_hash_fnv && kc->w == (unsigned short)w &&
            kc->h == (unsigned short)h && kc->fmt == fmt) {
            g_vtc_keycache_hits++;
            unsigned long long cached_key = kc->vtc_key;
            if (cached_key != 0) {
                // Re-queue for prefetch in case it was evicted from loaded_cache
                if (!vtc_loaded_find(cached_key) && g_vtc_prefetch_count < VTC_PREFETCH_MAX) {
                    g_vtc_prefetch_keys[g_vtc_prefetch_count++] = cached_key;
                }
            }
            return cached_key;
        }
    }

    // Cache miss: compute XXHash64 inline (first encounter only)
    int hash_size = gc_texture_data_size(w, h, fmt);
    if (hash_size > data_size) hash_size = data_size;
    unsigned long long data_hash = xxhash64(data, hash_size);
    unsigned long long tlut_hash = vtc_compute_tlut_hash(data, hash_size, tlut_data, tlut_entries, tlut_is_be);
    unsigned long long key = vtc_cache_key(data_hash, tlut_hash, fmt, (unsigned int)w, (unsigned int)h);

    unsigned long long result_key = 0; // default: not in VTC

    if (vtc_loaded_find(key)) {
        result_key = key;
    } else {
        VtcIndexEntry* entry = NULL;
        if (!vtc_neg_check(key))
            entry = vtc_find(key);

        // Wildcard fallback for CI textures: retry with tlut_hash=0
        if (!entry && tlut_hash != 0) {
            unsigned long long wc_key = vtc_cache_key(data_hash, 0, fmt, (unsigned int)w, (unsigned int)h);
            if (vtc_loaded_find(wc_key)) {
                result_key = wc_key;
            } else if (!vtc_neg_check(wc_key)) {
                entry = vtc_find(wc_key);
                if (entry) result_key = wc_key;
            }
        } else if (entry) {
            result_key = key;
        }

        if (result_key == 0) {
            vtc_neg_insert(key);
        }
    }

    // Store in XXHash64 result cache
    for (int i = 0; i < 4; i++) {
        unsigned int idx = (kc_slot + i) & VTC_KEY_CACHE_MASK;
        VtcKeyCacheEntry* kc = &g_vtc_key_cache[idx];
        if (!kc->occupied) {
            kc->data_ptr = data_ptr;
            kc->data_hash_fnv = data_hash_fnv;
            kc->tlut_hash_fnv = tlut_hash_fnv;
            kc->w = (unsigned short)w;
            kc->h = (unsigned short)h;
            kc->fmt = fmt;
            kc->vtc_key = result_key;
            kc->occupied = 1;
            break;
        }
    }

    if (result_key != 0) {
        g_vtc_compute_hits++;
        // Queue for prefetch. Worker thread will read VTC data during idle time.
        if (g_vtc_prefetch_count < VTC_PREFETCH_MAX) {
            g_vtc_prefetch_keys[g_vtc_prefetch_count++] = result_key;
        }
    }
    return result_key;
}

// Main thread: lookup by pre-computed cache_key, read DXT, upload GL texture.
// Called from deferred upload processing after worker thread provided the key.
GLuint vita_vtc_lookup_by_key(unsigned long long key, int* out_w, int* out_h) {
    if (!g_vtc_active || key == 0) return 0;

    // 1. Check loaded cache (instant, no I/O)
    VtcLoadedEntry* loaded = vtc_loaded_find(key);
    if (loaded) {
        if (out_w) *out_w = loaded->tex_w;
        if (out_h) *out_h = loaded->tex_h;
        return loaded->gl_tex;
    }

    // 2. Check I/O thread pre-read slots (worker thread queued these last frame)
    for (int si = 0; si < VTC_IO_SLOTS; si++) {
        if (g_vtc_io_slots[si].key == key && g_vtc_io_slots[si].state == VTC_SLOT_READY) {
            // I/O thread already read this. Zero file I/O on main thread.
            g_vtc_io_preread_hits++;
            unsigned char* dxt = g_vtc_io_slots[si].buffer;
            int dxt_size = g_vtc_io_slots[si].dxt_size;
            int hd_w = g_vtc_io_slots[si].hd_w;
            int hd_h = g_vtc_io_slots[si].hd_h;
            unsigned char slot_fmt = g_vtc_io_slots[si].dxt_format;
            GLenum gl_fmt = vtc_fmt_to_gl(slot_fmt);

            GLuint tex;
            glGenTextures(1, &tex);
            glActiveTexture(GL_TEXTURE7);
            glBindTexture(GL_TEXTURE_2D, tex);
            glCompressedTexImage2D(GL_TEXTURE_2D, 0, gl_fmt, hd_w, hd_h,
                                   0, dxt_size, dxt);
            // Safe defaults. Caller overrides with correct GX values, but
            // prevent GL_REPEAT/GL_NEAREST_MIPMAP_LINEAR if used before that.
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glActiveTexture(GL_TEXTURE0);

            vita_vtc_release_slot(si);

            if (glGetError() != GL_NO_ERROR) {
                glDeleteTextures(1, &tex);
                return 0;
            }

            int vram = vtc_estimate_vram(hd_w, hd_h, slot_fmt);
            vtc_loaded_insert(key, tex, hd_w, hd_h, vram);

            if (out_w) *out_w = hd_w;
            if (out_h) *out_h = hd_h;
            return tex;
        }
    }

    // 3. Prefetch not ready. Return 0 (use GC texture this frame).
    // Worker thread will read DXT data next idle window via prefetch.
    // When tex_cache evicts the GC entry and the game re-requests it,
    // lookup_by_key will find the HD version in loaded_cache instantly.
    return 0;
}

// I/O thread: reads DXT data from VTC file asynchronously

static int vtc_io_thread_func(SceSize args, void* argp) {
    (void)args; (void)argp;

    while (!g_vtc_io_shutdown) {
        sceKernelWaitSema(g_vtc_io_sema, 1, NULL);
        if (g_vtc_io_shutdown) break;

        // Find a queued slot
        for (int i = 0; i < VTC_IO_SLOTS; i++) {
            if (g_vtc_io_slots[i].state != VTC_SLOT_QUEUED) continue;

            VtcIoSlot* slot = &g_vtc_io_slots[i];
            slot->state = VTC_SLOT_READING;

            // Determine buffer
            unsigned char* buf;
            if (slot->dxt_size <= VTC_IO_BUF_SIZE) {
                buf = slot->buffer; // pre-allocated pool buffer
                slot->malloc_buf = NULL;
            } else {
                buf = (unsigned char*)malloc(slot->dxt_size);
                slot->malloc_buf = buf;
                if (!buf) { slot->state = VTC_SLOT_FREE; continue; }
            }

            // Read from VTC file
            fseek(g_vtc_file_io, slot->file_offset, SEEK_SET);
            if (fread(buf, 1, slot->dxt_size, g_vtc_file_io) == (size_t)slot->dxt_size) {
                slot->buffer = buf;
                slot->state = VTC_SLOT_READY;
                g_vtc_io_completed++;
            } else {
                if (slot->malloc_buf) free(slot->malloc_buf);
                slot->malloc_buf = NULL;
                slot->state = VTC_SLOT_FREE;
            }
            // Don't break. Drain ALL queued slots in one wake cycle.
        }
    }
    return 0;
}

void vita_vtc_io_init(void) {
    if (!g_vtc_active) return;

    // Allocate I/O slot buffer pool on heap
    g_vtc_io_pool = (unsigned char*)malloc(VTC_IO_SLOTS * VTC_IO_BUF_SIZE);
    if (g_vtc_io_pool) {
        for (int i = 0; i < VTC_IO_SLOTS; i++) {
            g_vtc_io_slots[i].buffer = &g_vtc_io_pool[i * VTC_IO_BUF_SIZE];
            g_vtc_io_slots[i].malloc_buf = NULL;
            g_vtc_io_slots[i].state = VTC_SLOT_FREE;
            g_vtc_io_slots[i].key = 0;
        }
    }

    // Open dedicated file handle for I/O thread
    g_vtc_file_io = fopen(g_vtc_path, "rb");

    // Start low-priority I/O thread for VTC reads.
    // Priority 0xA0 (160) = lower than main (0x40) and worker (0x60).
    // Runs on any available core, won't preempt rendering.
    g_vtc_io_sema = sceKernelCreateSema("vtc_io", 0, 0, 64, NULL);
    if (g_vtc_io_sema >= 0 && g_vtc_file_io) {
        g_vtc_io_shutdown = 0;
        g_vtc_io_thread = sceKernelCreateThread("vtc_io", vtc_io_thread_func,
                                                 0xA0, // low priority
                                                 64 * 1024, // 64KB stack
                                                 0, 0, NULL);
        if (g_vtc_io_thread >= 0) {
            sceKernelStartThread(g_vtc_io_thread, 0, NULL);
        }
    }
}

// Called from emu64 worker thread AFTER signal(done), during its idle window.
// Resolves prefetch keys to I/O slots and queues them for the dedicated I/O thread.
// NO file I/O happens here. The I/O thread handles reads on a low-priority thread
// to avoid memory bus contention with main thread rendering.
void vita_vtc_prefetch(void) {
    if (!g_vtc_active || !g_vtc_io_pool) return;

    int count = g_vtc_prefetch_count;
    g_vtc_prefetch_count = 0;

    if (count == 0) return;

    // If I/O thread is running, queue slots for it.
    // If no I/O thread, fall back to inline reads (worker file handle).
    int have_io_thread = (g_vtc_io_thread >= 0 && g_vtc_file_io != NULL);

    // Phase 1: Resolve keys to index entries, filtering duplicates and cache hits
    typedef struct { unsigned long long key; VtcIndexEntry* entry; } PrefetchItem;
    PrefetchItem pending[VTC_PREFETCH_MAX];
    int pending_count = 0;

    for (int qi = 0; qi < count; qi++) {
        unsigned long long key = g_vtc_prefetch_keys[qi];

        int already = 0;
        for (int si = 0; si < VTC_IO_SLOTS; si++) {
            if (g_vtc_io_slots[si].key == key && g_vtc_io_slots[si].state != VTC_SLOT_FREE) {
                already = 1;
                break;
            }
        }
        if (already) continue;
        if (vtc_loaded_find(key)) continue;

        VtcIndexEntry* entry = vtc_find(key);
        if (!entry) continue;

        pending[pending_count].key = key;
        pending[pending_count].entry = entry;
        pending_count++;
    }

    // Phase 2: Sort by file offset for sequential I/O
    for (int i = 1; i < pending_count; i++) {
        PrefetchItem tmp = pending[i];
        int j = i - 1;
        while (j >= 0 && pending[j].entry->offset > tmp.entry->offset) {
            pending[j + 1] = pending[j];
            j--;
        }
        pending[j + 1] = tmp;
    }

    // Phase 3: Queue into I/O slots
    int signaled = 0;
    for (int pi = 0; pi < pending_count; pi++) {
        VtcIndexEntry* entry = pending[pi].entry;

        int slot_idx = -1;
        for (int si = 0; si < VTC_IO_SLOTS; si++) {
            if (g_vtc_io_slots[si].state == VTC_SLOT_FREE) {
                slot_idx = si;
                break;
            }
        }
        if (slot_idx < 0) break;

        VtcIoSlot* slot = &g_vtc_io_slots[slot_idx];
        slot->key = pending[pi].key;
        slot->dxt_size = (int)entry->dxt_size;
        slot->hd_w = entry->hd_width;
        slot->hd_h = entry->hd_height;
        slot->dxt_format = entry->format;
        slot->file_offset = entry->offset;

        if (have_io_thread) {
            // Queue for I/O thread. No file I/O on worker thread.
            slot->state = VTC_SLOT_QUEUED;
            g_vtc_io_queued++;
            signaled++;
        } else {
            // Fallback: inline read on worker thread
            slot->state = VTC_SLOT_READING;
            unsigned char* buf;
            if (slot->dxt_size <= VTC_IO_BUF_SIZE) {
                buf = slot->buffer;
                slot->malloc_buf = NULL;
            } else {
                buf = (unsigned char*)malloc(slot->dxt_size);
                slot->malloc_buf = buf;
                if (!buf) { slot->state = VTC_SLOT_FREE; continue; }
            }
            fseek(g_vtc_file_worker, slot->file_offset, SEEK_SET);
            if (fread(buf, 1, slot->dxt_size, g_vtc_file_worker) == (size_t)slot->dxt_size) {
                slot->buffer = buf;
                slot->state = VTC_SLOT_READY;
                g_vtc_io_completed++;
            } else {
                if (slot->malloc_buf) free(slot->malloc_buf);
                slot->malloc_buf = NULL;
                slot->state = VTC_SLOT_FREE;
            }
        }
    }

    // Wake I/O thread once for all queued slots
    if (signaled > 0 && g_vtc_io_sema >= 0) {
        sceKernelSignalSema(g_vtc_io_sema, signaled);
    }
}

void vita_vtc_io_shutdown(void) {
    if (g_vtc_io_thread >= 0) {
        g_vtc_io_shutdown = 1;
        sceKernelSignalSema(g_vtc_io_sema, 1);
        sceKernelWaitThreadEnd(g_vtc_io_thread, NULL, NULL);
        sceKernelDeleteThread(g_vtc_io_thread);
        g_vtc_io_thread = -1;
    }
    if (g_vtc_io_sema >= 0) {
        sceKernelDeleteSema(g_vtc_io_sema);
        g_vtc_io_sema = -1;
    }
    // Free any overflow buffers
    for (int i = 0; i < VTC_IO_SLOTS; i++) {
        if (g_vtc_io_slots[i].malloc_buf) {
            free(g_vtc_io_slots[i].malloc_buf);
            g_vtc_io_slots[i].malloc_buf = NULL;
        }
    }
    if (g_vtc_file_io) { fclose(g_vtc_file_io); g_vtc_file_io = NULL; }
}

// Queue an async read. Returns slot index (>=0) or -1 if queue full.
// Called from main thread during deferred upload processing.
int vita_vtc_request_read(unsigned long long key) {
    if (g_vtc_io_thread < 0) return -1;

    // Check if already queued or ready
    for (int i = 0; i < VTC_IO_SLOTS; i++) {
        if (g_vtc_io_slots[i].key == key &&
            (g_vtc_io_slots[i].state == VTC_SLOT_QUEUED ||
             g_vtc_io_slots[i].state == VTC_SLOT_READING ||
             g_vtc_io_slots[i].state == VTC_SLOT_READY))
            return i;
    }

    // Find VTC entry for this key
    VtcIndexEntry* entry = vtc_find(key);
    if (!entry) return -1;

    // Find a free slot
    for (int i = 0; i < VTC_IO_SLOTS; i++) {
        if (g_vtc_io_slots[i].state == VTC_SLOT_FREE) {
            g_vtc_io_slots[i].key = key;
            g_vtc_io_slots[i].dxt_size = (int)entry->dxt_size;
            g_vtc_io_slots[i].hd_w = entry->hd_width;
            g_vtc_io_slots[i].hd_h = entry->hd_height;
            g_vtc_io_slots[i].dxt_format = entry->format;
            g_vtc_io_slots[i].file_offset = entry->offset;
            g_vtc_io_slots[i].state = VTC_SLOT_QUEUED;
            g_vtc_io_queued++;
            sceKernelSignalSema(g_vtc_io_sema, 1);
            return i;
        }
    }
    return -1; // queue full
}

// Check if a slot's read is complete. Returns 1 if ready, 0 if still loading.
// Outputs DXT buffer pointer and metadata.
int vita_vtc_check_ready(int slot, unsigned char** out_dxt, int* out_size,
                          int* out_w, int* out_h, int* out_gl_fmt) {
    if (slot < 0 || slot >= VTC_IO_SLOTS) return 0;
    if (g_vtc_io_slots[slot].state != VTC_SLOT_READY) return 0;

    *out_dxt = g_vtc_io_slots[slot].buffer;
    *out_size = g_vtc_io_slots[slot].dxt_size;
    *out_w = g_vtc_io_slots[slot].hd_w;
    *out_h = g_vtc_io_slots[slot].hd_h;
    *out_gl_fmt = vtc_fmt_to_gl(g_vtc_io_slots[slot].dxt_format);
    return 1;
}

// Get the cache key for a slot
unsigned long long vita_vtc_get_slot_key(int slot) {
    if (slot < 0 || slot >= VTC_IO_SLOTS) return 0;
    return g_vtc_io_slots[slot].key;
}

// Release a slot after main thread has uploaded the DXT data.
void vita_vtc_release_slot(int slot) {
    if (slot < 0 || slot >= VTC_IO_SLOTS) return;
    if (g_vtc_io_slots[slot].malloc_buf) {
        free(g_vtc_io_slots[slot].malloc_buf);
        g_vtc_io_slots[slot].malloc_buf = NULL;
    }
    g_vtc_io_slots[slot].buffer = &g_vtc_io_pool[slot * VTC_IO_BUF_SIZE];
    g_vtc_io_slots[slot].state = VTC_SLOT_FREE;
    g_vtc_io_slots[slot].key = 0;
}

#endif // TARGET_VITA
