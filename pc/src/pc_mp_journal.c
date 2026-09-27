// pc_mp_journal.c
// passport side files: a network traveller's own data while away, so a crash restores the trip
#include "pc_mp.h"

#ifdef VITA_MP

#include "m_common_data.h"
#include "m_card.h"
#include "m_land.h"
#include "m_private.h"

#include <stdio.h>
#include <string.h>
#include <zlib.h>
#ifdef TARGET_VITA
#include <psp2/io/fcntl.h>
#include <psp2/kernel/threadmgr.h>
#endif

#define MP_PASSPORT_MAGIC   0x504D4341u // "ACMP"
#define MP_PASSPORT_VERSION 1
#define MP_PASSPORT_NAME    "mp_passport"

typedef struct {
    u32 magic;
    u16 version;
    u16 body_size;
    u32 seq;
    u32 crc; // header with crc zeroed, then the body
} mp_passport_hdr_t;

typedef struct {
    mp_passport_hdr_t hdr;
    mCD_foreigner_c body;
} mp_passport_file_t;

static mp_passport_file_t s_pp;      // staged on the game thread
static mp_passport_file_t s_pp_out;  // on its way to the card
static mp_passport_file_t s_pp_scratch;
static u32 s_pp_seq;
static int s_pp_player = -1; // the traveller's player slot at home
static int s_pp_good_slot = -1; // file holding the newest good write; the next write takes the other
static volatile u32 s_pp_written; // newest passport written, or tried and failed
static volatile int s_pp_written_ok = TRUE;
static u32 s_pp_mark;              // the host's newest save the staged one has (mp_world_pp_mark)
static u32 s_pp_out_mark;
static volatile u32 s_pp_good_mark; // ...and the newest one on the card
static volatile u32 s_pp_good_seq;  // the newest passport on the card

#ifdef TARGET_VITA
static SceUID s_pp_stage_mtx = -1; // guards s_pp
static SceUID s_pp_io_mtx = -1;    // one file write at a time
static SceUID s_pp_sema = -1;
static int s_pp_threaded;          // 0 untried, 1 the writer runs, -1 writes happen in place

static void mp_pp_lock(SceUID m) {
    if (m >= 0) {
        sceKernelLockMutex(m, 1, NULL);
    }
}

static void mp_pp_unlock(SceUID m) {
    if (m >= 0) {
        sceKernelUnlockMutex(m, 1);
    }
}
#define MP_PP_LOCK(m)   mp_pp_lock(m)
#define MP_PP_UNLOCK(m) mp_pp_unlock(m)
#else
#define MP_PP_LOCK(m)   ((void)0)
#define MP_PP_UNLOCK(m) ((void)0)
#endif

unsigned int mp_crc32(const void* data, int len, unsigned int crc) {
    return (unsigned int)crc32(crc, (const Bytef*)data, (uInt)len);
}

// two files per resident take turns, each written in place and synced: no rename has to survive
static void mp_passport_path(char* out, int size, int player, int slot) {
    snprintf(out, size, "%s/%s_p%d_%d.bin", pc_mc_home_dir(), MP_PASSPORT_NAME, player, slot);
}

static int mp_file_write(const char* path, const void* data, int len) {
#ifdef TARGET_VITA
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);

    if (fd < 0) {
        return FALSE;
    }
    if (sceIoWrite(fd, data, len) != len) {
        sceIoClose(fd);
        sceIoRemove(path);
        return FALSE;
    }
    sceIoSyncByFd(fd, 0);
    sceIoClose(fd);
    return TRUE;
#else
    FILE* fp = fopen(path, "wb");
    int ok;

    if (fp == NULL) {
        return FALSE;
    }
    ok = fwrite(data, 1, len, fp) == (size_t)len;
    fclose(fp);
    if (!ok) {
        remove(path);
    }
    return ok;
#endif
}

static int mp_file_read(const char* path, void* buf, int size) {
    FILE* fp = fopen(path, "rb");
    int n;

    if (fp == NULL) {
        return -1;
    }
    n = (int)fread(buf, 1, size, fp);
    fclose(fp);
    return n;
}


static int mp_passport_valid(const mp_passport_file_t* f) {
    mp_passport_hdr_t hdr = f->hdr;
    unsigned int crc;

    if (hdr.magic != MP_PASSPORT_MAGIC || hdr.version != MP_PASSPORT_VERSION ||
        hdr.body_size != sizeof(mCD_foreigner_c)) {
        return FALSE;
    }
    hdr.crc = 0;
    crc = mp_crc32(&hdr, sizeof(hdr), 0);
    crc = mp_crc32(&f->body, sizeof(f->body), crc);
    return crc == f->hdr.crc;
}

// the newer of the player's two files that reads back whole
static int mp_passport_load(int player, mp_passport_file_t* out) {
    static mp_passport_file_t other;
    char path[324];
    int have = FALSE;
    int i;

    for (i = 0; i < 2; i++) {
        mp_passport_file_t* dst = have ? &other : out;

        mp_passport_path(path, sizeof(path), player, i);
        if (mp_file_read(path, dst, sizeof(*dst)) == (int)sizeof(*dst) && mp_passport_valid(dst)) {
            if (have && other.hdr.seq > out->hdr.seq) {
                memcpy(out, &other, sizeof(*out));
            }
            have = TRUE;
        }
    }
    return have;
}

static void mp_passport_remove(int player) {
    char path[324];
    int i;

    for (i = 0; i < 2; i++) {
        mp_passport_path(path, sizeof(path), player, i);
        remove(path);
    }
}

// the newest staged passport to the card, unless it's already there
static int mp_pp_flush(void) {
    char path[324];
    u32 seq;
    int slot;
    int ok;

    MP_PP_LOCK(s_pp_io_mtx);
    MP_PP_LOCK(s_pp_stage_mtx);
    seq = s_pp.hdr.seq;
    if ((s32)(seq - s_pp_written) <= 0 || s_pp_player < 0) {
        MP_PP_UNLOCK(s_pp_stage_mtx);
        ok = s_pp_written_ok;
        MP_PP_UNLOCK(s_pp_io_mtx);
        return ok;
    }
    memcpy(&s_pp_out, &s_pp, sizeof(s_pp_out));
    s_pp_out_mark = s_pp_mark;
    slot = (s_pp_good_slot == 0) ? 1 : 0;
    mp_passport_path(path, sizeof(path), s_pp_player, slot);
    MP_PP_UNLOCK(s_pp_stage_mtx);

    ok = mp_file_write(path, &s_pp_out, sizeof(s_pp_out));
    if (ok) {
        s_pp_good_slot = slot;
        __atomic_store_n(&s_pp_good_mark, s_pp_out_mark, __ATOMIC_RELEASE);
        __atomic_store_n(&s_pp_good_seq, seq, __ATOMIC_RELEASE);
    }
    s_pp_written_ok = ok;
    __atomic_store_n(&s_pp_written, seq, __ATOMIC_RELEASE);
    MP_PP_UNLOCK(s_pp_io_mtx);
    if (!ok) {
        pc_mp_log("[MP] passport #%u not written", (unsigned)seq);
    }
    return ok;
}

#ifdef TARGET_VITA
static int mp_pp_writer(SceSize args, void* argp) {
    (void)args;
    (void)argp;
    while (sceKernelWaitSema(s_pp_sema, 1, NULL) >= 0) {
        mp_pp_flush();
    }
    return 0;
}
#endif

// the writer thread and its locks, on first use from the game thread
static void mp_pp_init(void) {
#ifdef TARGET_VITA
    SceUID thread;

    if (s_pp_threaded != 0) {
        return;
    }
    s_pp_stage_mtx = sceKernelCreateMutex("ac_pp_stage", 0, 0, NULL);
    s_pp_io_mtx = sceKernelCreateMutex("ac_pp_io", 0, 0, NULL);
    s_pp_sema = sceKernelCreateSema("ac_pp_req", 0, 0, 64, NULL);
    thread = (s_pp_sema >= 0) ? sceKernelCreateThread("ac_pp_writer", mp_pp_writer, 0x10000110, 0x4000, 0, 0, NULL)
                              : -1;
    if (thread >= 0 && sceKernelStartThread(thread, 0, NULL) >= 0) {
        s_pp_threaded = 1;
        return;
    }
    if (thread >= 0) {
        sceKernelDeleteThread(thread);
    }
    s_pp_threaded = -1;
    pc_mp_log("[MP] passport writer unavailable; writing in place");
#endif
}

// the traveller as they are right now, ready for the card; 0 when there's no trip
static u32 mp_pp_stage(void) {
    const mCD_foreigner_c* body;
    unsigned int crc;
    u32 seq;

    if (s_pp_player < 0) {
        pc_mp_log("[MP] passport: no traveller");
        return 0;
    }
    // (a pickup the host hasn't saved was used up: the passport stays as it last was)
    if (mp_world_pp_frozen()) {
        return 0;
    }
    mp_pp_init();
    pc_mc_lock();
    body = (const mCD_foreigner_c*)pc_mc_passport();
    MP_PP_LOCK(s_pp_stage_mtx);
    memset(&s_pp.hdr, 0, sizeof(s_pp.hdr));
    s_pp.hdr.magic = MP_PASSPORT_MAGIC;
    s_pp.hdr.version = MP_PASSPORT_VERSION;
    s_pp.hdr.body_size = sizeof(mCD_foreigner_c);
    s_pp.hdr.seq = seq = ++s_pp_seq;
    memcpy(&s_pp.body, body, sizeof(s_pp.body));
    mp_world_talk_stage(&s_pp.body.priv);
    mp_world_passport_filter(&s_pp.body.priv);
    s_pp_mark = mp_world_pp_mark();
    crc = mp_crc32(&s_pp.hdr, sizeof(s_pp.hdr), 0);
    s_pp.hdr.crc = mp_crc32(&s_pp.body, sizeof(s_pp.body), crc);
    MP_PP_UNLOCK(s_pp_stage_mtx);
    pc_mc_unlock();
    return seq;
}

// departure: the player's files start over, numbered past anything an old trip left behind
void mp_passport_begin(int player_no) {
    mp_pp_init();
    MP_PP_LOCK(s_pp_io_mtx);
    MP_PP_LOCK(s_pp_stage_mtx);
    s_pp_player = player_no;
    s_pp_seq = mp_passport_load(player_no, &s_pp_scratch) ? s_pp_scratch.hdr.seq : 0;
    mp_passport_remove(player_no);
    s_pp.hdr.seq = s_pp_seq; // nothing staged from an earlier trip is left to write
    s_pp_written = s_pp_seq;
    s_pp_written_ok = TRUE;
    s_pp_good_slot = -1;
    s_pp_mark = 0;
    s_pp_good_mark = 0;
    s_pp_good_seq = s_pp_seq;
    MP_PP_UNLOCK(s_pp_stage_mtx);
    MP_PP_UNLOCK(s_pp_io_mtx);
}

// the host's newest save the newest passport on the card has (bit 16: any at all)
unsigned int mp_passport_mark(void) {
    return __atomic_load_n(&s_pp_good_mark, __ATOMIC_ACQUIRE);
}

// the newest passport staged so far, and the newest on the card (one staged later than a change leaves it out)
unsigned int mp_passport_staged_seq(void) {
    u32 seq;

    MP_PP_LOCK(s_pp_stage_mtx);
    seq = s_pp_seq;
    MP_PP_UNLOCK(s_pp_stage_mtx);
    return seq;
}

unsigned int mp_passport_good_seq(void) {
    return __atomic_load_n(&s_pp_good_seq, __ATOMIC_ACQUIRE);
}

// ...and the newest staged, which the card may have by now
unsigned int mp_passport_mark_staged(void) {
    u32 mark;

    MP_PP_LOCK(s_pp_stage_mtx);
    mark = s_pp_mark;
    MP_PP_UNLOCK(s_pp_stage_mtx);
    return mark;
}

// the traveller to the card before returning; the trip's save points use this
int mp_passport_write(void) {
    return mp_pp_stage() != 0 && mp_pp_flush();
}

// the card's newest passport again with the traveller given, its host mark kept (the app closing while it stands)
int mp_passport_rewrite(const void* priv) {
    unsigned int crc;
    int ok;

    if (s_pp_player < 0) {
        return FALSE;
    }
    MP_PP_LOCK(s_pp_io_mtx);
    ok = mp_passport_load(s_pp_player, &s_pp_scratch);
    MP_PP_UNLOCK(s_pp_io_mtx);
    if (!ok) {
        return FALSE;
    }
    MP_PP_LOCK(s_pp_stage_mtx);
    memcpy(&s_pp, &s_pp_scratch, sizeof(s_pp));
    memcpy(&s_pp.body.priv, priv, sizeof(s_pp.body.priv));
    s_pp.hdr.seq = ++s_pp_seq;
    s_pp.hdr.crc = 0;
    s_pp_mark = __atomic_load_n(&s_pp_good_mark, __ATOMIC_ACQUIRE);
    crc = mp_crc32(&s_pp.hdr, sizeof(s_pp.hdr), 0);
    s_pp.hdr.crc = mp_crc32(&s_pp.body, sizeof(s_pp.body), crc);
    MP_PP_UNLOCK(s_pp_stage_mtx);
    return mp_pp_flush();
}

// what's staged goes to the card now if it isn't there yet, so what's read after is what the card has
int mp_passport_settle(void) {
    if (s_pp_player < 0) {
        return FALSE;
    }
    return mp_pp_flush();
}

// the same on the writer thread; wait on the returned number with mp_passport_written
unsigned int mp_passport_write_async(void) {
    u32 seq = mp_pp_stage();

    if (seq == 0) {
        return 0;
    }
#ifdef TARGET_VITA
    if (s_pp_threaded > 0) {
        sceKernelSignalSema(s_pp_sema, 1);
        return seq;
    }
#endif
    mp_pp_flush();
    return seq;
}

// TRUE once that passport (or a newer one) has been written; ok says whether it made it
int mp_passport_written(unsigned int seq, int* ok) {
    if ((s32)(__atomic_load_n(&s_pp_written, __ATOMIC_ACQUIRE) - seq) < 0) {
        return FALSE;
    }
    *ok = s_pp_written_ok;
    return TRUE;
}

void mp_passport_delete(void) {
    MP_PP_LOCK(s_pp_io_mtx);
    MP_PP_LOCK(s_pp_stage_mtx);
    if (s_pp_player >= 0) {
        mp_passport_remove(s_pp_player);
    }
    s_pp_player = -1;
    s_pp_written = s_pp.hdr.seq; // whatever is still staged is dropped with the files
    s_pp_good_slot = -1;
    MP_PP_UNLOCK(s_pp_stage_mtx);
    MP_PP_UNLOCK(s_pp_io_mtx);
}

// the traveller as the newest passport written has them
int mp_passport_last_priv(void* priv) {
    int ok;

    if (s_pp_player < 0) {
        return FALSE;
    }
    MP_PP_LOCK(s_pp_io_mtx);
    ok = mp_passport_load(s_pp_player, &s_pp_scratch);
    if (ok) {
        memcpy(priv, &s_pp_scratch.body.priv, sizeof(s_pp_scratch.body.priv));
    }
    MP_PP_UNLOCK(s_pp_io_mtx);
    return ok;
}

static int s_pp_restored_any; // the trip restored had something in its pockets or wallet

int mp_passport_restored_any(void) {
    return s_pp_restored_any;
}

// title start: a traveller whose trip never saved its way home gets it back here
int mp_passport_recover(int player_no) {
    Private_c* priv;
    PersonalID_c* pid;

    if (player_no < 0 || player_no >= PLAYER_NUM || !mp_passport_load(player_no, &s_pp_scratch)) {
        return FALSE;
    }
    pid = &s_pp_scratch.body.priv.player_ID;
    if (!mLd_CheckThisLand(pid->land_name, pid->land_id)) {
        return FALSE; // belongs to the other save slot's town
    }
    priv = Save_GetPointer(private_data[player_no]);
    if (!mPr_CheckCmpPersonalID(&priv->player_ID, pid)) {
        return FALSE; // not this resident any more (the slot was reused)
    }
    s_pp_player = player_no;
    if (priv->exists) {
        // the trip home already saved; the file just outlived it
        mp_passport_delete();
        return FALSE;
    }

    // same merge a train arrival does, then the player is simply home
    mPr_CopyPrivateInfo(mPr_GetForeignerP(), &s_pp_scratch.body.priv);
    mPr_LoadPak_and_SetPrivateInfo2(mPr_GetForeignerP(), (u8)player_no);
    priv->exists = TRUE;
    priv->reset_code = 0;
    pc_mp_log("[MP] restored %.8s from passport #%u", pid->player_name, (unsigned)s_pp_scratch.hdr.seq);
    {
        int i;

        s_pp_restored_any = s_pp_scratch.body.priv.inventory.wallet != 0;
        for (i = 0; i < mPr_POCKETS_SLOT_COUNT; i++) {
            s_pp_restored_any |= s_pp_scratch.body.priv.inventory.pockets[i] != EMPTY_NO;
        }
    }
    return TRUE;
}

#endif
