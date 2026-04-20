/* pc_m_card.c - memory card manager: GCI save/load, village generation, ARAM data blocks
 *
 * Card A (save/card_a/) = player's home town
 * Card B (save/card_b/) = second town for visiting (drop any AC GCI file there)
 */
#include "m_card.h"
#include "m_start_data_init.h"
#include "m_common_data.h"
#include "m_flashrom.h"
#include "m_field_make.h"
#include "m_msg.h"
#include "m_npc.h"
#include "m_quest.h"
#include "m_island.h"
#include "m_font.h"
#include "m_vibctl.h"
#include "m_bg_item.h"
#include "m_land.h"
#include "m_private.h"
#include "m_event.h"
#include "m_time.h"
#include "m_scene.h"
#include "m_scene_table.h"
#include "m_name_table.h"
#include "m_actor.h"
#include "m_notice.h"
#include "m_player.h"
#include "m_player_lib.h"
#include "m_all_grow_ovl.h"
#include "m_home.h"
#include "sys_math3d.h"
#include "sys_math.h"
#include "zurumode.h"
#include "pc_save_bswap.h"
#include "pc_settings.h"
#include "m_cockroach.h"
#include "game.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <direct.h>  /* _mkdir */
#endif
#ifdef TARGET_VITA
#include <psp2/io/stat.h>  /* sceIoMkdir */
#endif
#include <dolphin/os.h>  /* OSReport */

/* --- Path constants --- */
#ifdef TARGET_VITA
#define PC_CARD_A_DIR     "ux0:data/AnimalCrossing/saves/card_a"
#define PC_CARD_B_DIR     "ux0:data/AnimalCrossing/saves/card_b"
#define PC_SAVE_DIR       "ux0:data/AnimalCrossing/saves"
#else
#define PC_CARD_A_DIR     "save/card_a"
#define PC_CARD_B_DIR     "save/card_b"
#define PC_SAVE_DIR       "save"
#endif
#define PC_GCI_FILENAME   "DobutsunomoriP_MURA.gci"
#define PC_GCI_PATH       PC_CARD_A_DIR "/" PC_GCI_FILENAME
#define PC_GCI_TMP_PATH   PC_CARD_A_DIR "/" PC_GCI_FILENAME ".tmp"
#define PC_SAVE_MAX_BACKUPS 3

/* Legacy paths for migration from flat save/ layout */
#define PC_GCI_PATH_LEGACY     "save/DobutsunomoriP_MURA.gci"
#define PC_GCI_TMP_PATH_LEGACY "save/DobutsunomoriP_MURA.gci.tmp"

#define GCI_HEADER_SIZE      sizeof(CARDDir)        /* 64 bytes */
#define GCI_FILE_DATA_SIZE   mCD_LAND_SAVE_SIZE     /* 0x72000 */
#define GCI_OTHERS_OFFSET    0
#define GCI_SAVE_MAIN_OFFSET OTHERS_SIZE            /* 0x26000 */
#define GCI_SAVE_BACK_OFFSET (OTHERS_SIZE + sizeof(Save))  /* 0x4C000 */
#define GCI_SECTOR_SIZE      mCD_MEMCARD_SECTORSIZE /* 0x2000 */

int pc_save_loaded = 0;
static int pc_save_ready = 0;

/* --- Travel state --- */
static Save l_keepSave;                        /* Other town's save data (for Card B visit) */
static int l_keepSave_set = FALSE;
static mCD_keep_mail_c l_keepMail;             /* Other town's mail ARAM block */
static mCD_keep_original_c l_keepOriginal;     /* Other town's original designs ARAM block */
static mCD_keep_diary_c l_keepDiary;           /* Other town's diary ARAM block */

/* Passport: the traveling player's private data + departing animal */
static union {
    mCD_foreigner_c file;
    u8 sector_align[mCD_ALIGN_SECTORSIZE(sizeof(mCD_foreigner_c))];
} l_mcd_foreigner_file;

static int l_mcd_keep_startCond = 0;
static char l_card_b_gci_path[300] = {0};      /* Path to the Card B GCI file, if found */

/* External: scan card_b/ for valid AC GCI file (defined in pc_card.c) */
extern int pc_card_scan_for_gci(int chan, char* out_path, int out_size);

/* External: functions from decomp used in mCD_toNextLand */
extern void mTM_rtcTime_limit_check(void);
extern void mEv_ClearEventInfo(void);
extern void mEv_toland_clear_common(void);
extern void mNpc_ClearInAnimal(void);
extern void mNpc_FirstClearGoodbyMail(void);
extern void mQst_ClearGrabItemInfo(void);
extern void mISL_ClearKeepIsland(void);
extern void mNpc_ClearCacheName(void);
extern void mTM_clear_renew_is(void);
extern void lbRTC_GetTime(lbRTC_time_c* time);

/* --- ARAM data blocks (mail/diary/original designs) --- */

static u32 l_aram_alloc_size_table[mCD_ARAM_DATA_NUM] = {
    ALIGN_NEXT(sizeof(mCD_keep_original_c), 32),
    ALIGN_NEXT(sizeof(mCD_keep_mail_c), 32),
    ALIGN_NEXT(sizeof(mCD_keep_diary_c), 32),
};

static void* l_aram_block_p_table[mCD_ARAM_DATA_NUM];

static void pc_init_diary_entries(void* block) {
    mCD_keep_diary_c* diary = (mCD_keep_diary_c*)block;
    int p, m;
    for (p = 0; p < mCD_KEEP_DIARY_COUNT; p++) {
        for (m = 0; m < mCD_KEEP_DIARY_ENTRY_COUNT; m++) {
            memset(diary->entries[p][m].text, CHAR_SPACE, mDI_ENTRY_SIZE);
        }
    }
}

/* Match GC's mCD_set_init_mail_data: clear mail slots with font=0xFF (unused) */
static void pc_init_mail_entries(void* block) {
    mCD_keep_mail_c* keep_mail = (mCD_keep_mail_c*)block;
    Mail_c* mail = (Mail_c*)keep_mail->mail;
    int i, j;
    for (i = 0; i < mCD_KEEP_MAIL_PAGE_COUNT; i++) {
        mem_clear(keep_mail->folder_names[i], sizeof(keep_mail->folder_names[i]), CHAR_SPACE);
        for (j = 0; j < mCD_KEEP_MAIL_COUNT; j++) {
            mMl_clear_mail(mail);
            mail++;
        }
    }
}

void mCD_save_data_aram_malloc(void) {
    int i;
    for (i = 0; i < mCD_ARAM_DATA_NUM; i++) {
        if (l_aram_block_p_table[i] == NULL) {
            l_aram_block_p_table[i] = malloc(l_aram_alloc_size_table[i]);
            if (!l_aram_block_p_table[i]) {
                fprintf(stderr, "[SAVE] Failed to allocate ARAM block %d (%u bytes)\n",
                        i, (unsigned)l_aram_alloc_size_table[i]);
                exit(1);
            }
            memset(l_aram_block_p_table[i], 0, l_aram_alloc_size_table[i]);
            if (i == mCD_ARAM_DATA_DIARY) {
                pc_init_diary_entries(l_aram_block_p_table[i]);
            } else if (i == mCD_ARAM_DATA_MAIL) {
                pc_init_mail_entries(l_aram_block_p_table[i]);
            }
        }
    }
}

int mCD_save_data_aram_to_main(void* dst, u32 size, u32 idx) {
    void* block;
    if (idx >= mCD_ARAM_DATA_NUM) {
        fprintf(stderr, "[SAVE] Invalid ARAM data index %u (max %d)\n", idx, mCD_ARAM_DATA_NUM);
        return FALSE;
    }
    block = l_aram_block_p_table[idx];
    if (block != NULL) {
        u32 copy_size = size < l_aram_alloc_size_table[idx] ? size : l_aram_alloc_size_table[idx];
        memcpy(dst, block, copy_size);
        return TRUE;
    }
    return FALSE;
}

int mCD_save_data_main_to_aram(void* src, u32 size, u32 idx) {
    void* block;
    if (idx >= mCD_ARAM_DATA_NUM) {
        fprintf(stderr, "[SAVE] Invalid ARAM data index %u (max %d)\n", idx, mCD_ARAM_DATA_NUM);
        return FALSE;
    }
    block = l_aram_block_p_table[idx];
    if (block != NULL) {
        u32 copy_size = size < l_aram_alloc_size_table[idx] ? size : l_aram_alloc_size_table[idx];
        memcpy(block, src, copy_size);
        return TRUE;
    }
    return FALSE;
}

void mCD_set_aram_save_data(void) {
    int i;
    for (i = 0; i < mCD_ARAM_DATA_NUM; i++) {
        if (l_aram_block_p_table[i] != NULL) {
            memset(l_aram_block_p_table[i], 0, l_aram_alloc_size_table[i]);
            if (i == mCD_ARAM_DATA_DIARY) {
                pc_init_diary_entries(l_aram_block_p_table[i]);
            } else if (i == mCD_ARAM_DATA_MAIL) {
                pc_init_mail_entries(l_aram_block_p_table[i]);
            }
        }
    }
}

/* --- GCI read/write helpers --- */

static void put_be32(u8* dst, u32 val) {
    dst[0] = (u8)(val >> 24);
    dst[1] = (u8)(val >> 16);
    dst[2] = (u8)(val >> 8);
    dst[3] = (u8)(val);
}

static void put_be16(u8* dst, u16 val) {
    dst[0] = (u8)(val >> 8);
    dst[1] = (u8)(val);
}

/* rotate backups: .bak3→delete, .bak2→.bak3, .bak1→.bak2, current→.bak1 */
static void pc_save_rotate_backups(const char* base_path) {
    char older[300], newer[300];
    int i;
    struct stat st;

    snprintf(older, sizeof(older), "%s.bak%d", base_path, PC_SAVE_MAX_BACKUPS);
    remove(older);

    /* Windows rename() fails if dest exists, so remove first */
    for (i = PC_SAVE_MAX_BACKUPS; i > 1; i--) {
        snprintf(older, sizeof(older), "%s.bak%d", base_path, i - 1);
        snprintf(newer, sizeof(newer), "%s.bak%d", base_path, i);
        remove(newer);
        rename(older, newer);
    }

    if (stat(base_path, &st) == 0) {
        snprintf(newer, sizeof(newer), "%s.bak1", base_path);
        remove(newer);
        rename(base_path, newer);
    }
}

static void pc_ensure_save_dirs(void) {
#ifdef TARGET_VITA
    sceIoMkdir("ux0:data/AnimalCrossing", 0777);
    sceIoMkdir(PC_SAVE_DIR, 0777);
    sceIoMkdir(PC_CARD_A_DIR, 0777);
    sceIoMkdir(PC_CARD_B_DIR, 0777);
#elif defined(_WIN32)
    _mkdir(PC_SAVE_DIR);
    _mkdir(PC_CARD_A_DIR);
    _mkdir(PC_CARD_B_DIR);
#else
    mkdir(PC_SAVE_DIR, 0755);
    mkdir(PC_CARD_A_DIR, 0755);
    mkdir(PC_CARD_B_DIR, 0755);
#endif
}

static int pc_save_write_gci_to(const char* gci_path, const char* tmp_path);

static int pc_save_write_gci(void) {
    return pc_save_write_gci_to(PC_GCI_PATH, PC_GCI_TMP_PATH);
}

static int pc_save_write_gci_to(const char* gci_path, const char* tmp_path) {
    FILE* fp;
    u8* file_data;
    CARDDir dir_hdr;
    Save_t* save_copy;
    u16 checksum;
    u8* others_ptr;

    if (!pc_save_ready) return TRUE;

    pc_ensure_save_dirs();

    Save_Get(save_exist) = TRUE;
    Save_Get(save_check).version = mFRm_VERSION;
    mFRm_SetSaveCheckData(Save_GetPointer(save_check));

    file_data = (u8*)calloc(1, GCI_FILE_DATA_SIZE);
    if (!file_data) return FALSE;

    /* Others block (offset 0) -comment, banner, ARAM blocks */
    others_ptr = file_data + GCI_OTHERS_OFFSET;
    {
        const char* title = "DobutsunomoriP (AC PC Port)";
        u8* comment = others_ptr;
        memset(comment, 0, CARD_COMMENT_SIZE);
        strncpy((char*)comment, title, 32);
        memcpy(comment + 32, Save_Get(land_info).name, 8);
    }

    /* ARAM blocks: mail, original, diary (GC/Dolphin order).
     * Set landid on mail block so load-time detection identifies the order. */
    {
        u32 offset = sizeof(MemcardHeader_c) + 32; /* 0x1460 */
        u16 land_id = Save_Get(land_info).id;

        offset = ALIGN_NEXT(offset, 32);
        if (l_aram_block_p_table[mCD_ARAM_DATA_MAIL]) {
            u8* blk = others_ptr + offset;
            u32 sz = l_aram_alloc_size_table[mCD_ARAM_DATA_MAIL];
            memcpy(blk, l_aram_block_p_table[mCD_ARAM_DATA_MAIL], sz);
            pc_save_bswap_keep_mail((mCD_keep_mail_c*)blk, PC_BSWAP_TO_BE);
            /* Set landid (BE u16 at offset 2) so load detects GC order */
            put_be16(blk + 2, land_id);
            /* Recompute BE checksum over the bswapped block so Dolphin's
             * mFRm_ReturnCheckSum validates. */
            blk[0] = 0;
            blk[1] = 0;
            put_be16(blk, pc_checksum_be(blk, sz, 0));
        }
        offset += l_aram_alloc_size_table[mCD_ARAM_DATA_MAIL];

        offset = ALIGN_NEXT(offset, 32);
        if (l_aram_block_p_table[mCD_ARAM_DATA_ORIGINAL]) {
            u8* blk = others_ptr + offset;
            u32 sz = l_aram_alloc_size_table[mCD_ARAM_DATA_ORIGINAL];
            memcpy(blk, l_aram_block_p_table[mCD_ARAM_DATA_ORIGINAL], sz);
            pc_save_bswap_keep_original((mCD_keep_original_c*)blk, PC_BSWAP_TO_BE);
            blk[0] = 0;
            blk[1] = 0;
            put_be16(blk, pc_checksum_be(blk, sz, 0));
        }
        offset += l_aram_alloc_size_table[mCD_ARAM_DATA_ORIGINAL];

        offset = ALIGN_NEXT(offset, 32);
        if (l_aram_block_p_table[mCD_ARAM_DATA_DIARY]) {
            u8* blk = others_ptr + offset;
            u32 sz = l_aram_alloc_size_table[mCD_ARAM_DATA_DIARY];
            memcpy(blk, l_aram_block_p_table[mCD_ARAM_DATA_DIARY], sz);
            pc_save_bswap_keep_diary((mCD_keep_diary_c*)blk, PC_BSWAP_TO_BE);
            blk[0] = 0;
            blk[1] = 0;
            put_be16(blk, pc_checksum_be(blk, sz, 0));
        }
    }

    /* Main Save_t (offset 0x26000) */
    save_copy = (Save_t*)(file_data + GCI_SAVE_MAIN_OFFSET);
    memcpy(save_copy, &common_data.save.save, sizeof(Save_t));

    pc_save_bswap(save_copy, PC_BSWAP_TO_BE);
    {
        u8* chk_ptr = (u8*)&save_copy->save_check.checksum;
        chk_ptr[0] = 0;
        chk_ptr[1] = 0;
    }
    checksum = pc_checksum_be((const u8*)save_copy, sizeof(Save_t), 0);
    put_be16((u8*)&save_copy->save_check.checksum, checksum);

    /* Backup = copy of main */
    memcpy(file_data + GCI_SAVE_BACK_OFFSET, file_data + GCI_SAVE_MAIN_OFFSET, sizeof(Save));

    /* CARDDir header */
    memset(&dir_hdr, 0, sizeof(dir_hdr));
    memcpy(dir_hdr.gameName, "GAFE", 4);
    memcpy(dir_hdr.company, "01", 2);
    dir_hdr.bannerFormat = 0;
    strncpy((char*)dir_hdr.fileName, "DobutsunomoriP_MURA", CARD_FILENAME_MAX);
    {
        time_t unix_now = time(NULL);
        u32 gc_secs = (u32)(unix_now - 946684800LL);
        put_be32((u8*)&dir_hdr.time, gc_secs);
    }
    put_be32((u8*)&dir_hdr.iconAddr, 0xFFFFFFFF);
    put_be16((u8*)&dir_hdr.iconFormat, 0);
    put_be16((u8*)&dir_hdr.iconSpeed, 0);
    dir_hdr.permission = 0x04;
    dir_hdr.copyTimes = 0;
    put_be16((u8*)&dir_hdr.startBlock, 5);
    put_be16((u8*)&dir_hdr.length, (u16)(GCI_FILE_DATA_SIZE / GCI_SECTOR_SIZE));
    put_be32((u8*)&dir_hdr.commentAddr, 0);

    /* write temp file → rotate backups → rename */
    fp = fopen(tmp_path, "wb");
    if (!fp) {
        OSReport("[PC] GCI save: failed to open temp file '%s'\n", tmp_path);
        free(file_data);
        return FALSE;
    }

    if (fwrite(&dir_hdr, GCI_HEADER_SIZE, 1, fp) != 1 ||
        fwrite(file_data, GCI_FILE_DATA_SIZE, 1, fp) != 1) {
        OSReport("[PC] GCI save: fwrite failed (disk full?)\n");
        fclose(fp);
        remove(tmp_path);
        free(file_data);
        return FALSE;
    }

    fflush(fp);
    fclose(fp);
    free(file_data);

    pc_save_rotate_backups(gci_path);
    if (rename(tmp_path, gci_path) != 0) {
        OSReport("[PC] GCI save: rename '%s' -> '%s' failed, recovering...\n",
                 tmp_path, gci_path);
        {
            char bak1[300];
            snprintf(bak1, sizeof(bak1), "%s.bak1", gci_path);
            rename(bak1, gci_path);
        }
        remove(tmp_path);
        return FALSE;
    }

    OSReport("[PC] GCI save: written successfully to %s (backups rotated)\n", gci_path);
    pc_save_loaded = 1;
    return TRUE;
}

/* Read a GCI file into common_data (for home town / Card A) */
static int pc_save_read_gci(const char* path) {
    FILE* fp;
    CARDDir dir_hdr;
    u8* file_data;
    Save_t* save_src;
    u32 offset;
    long file_size;

    fp = fopen(path, "rb");
    if (!fp) {
        OSReport("[PC] GCI: fopen('%s') failed\n", path);
        return FALSE;
    }

    fseek(fp, 0, SEEK_END);
    file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    OSReport("[PC] GCI: opened '%s', size = %ld (0x%lX), expected %ld (0x%lX)\n",
             path, file_size, (unsigned long)file_size,
             (long)(GCI_HEADER_SIZE + GCI_FILE_DATA_SIZE),
             (unsigned long)(GCI_HEADER_SIZE + GCI_FILE_DATA_SIZE));

    if (fread(&dir_hdr, GCI_HEADER_SIZE, 1, fp) != 1) {
        OSReport("[PC] GCI: failed to read %u-byte header\n", (unsigned)GCI_HEADER_SIZE);
        fclose(fp);
        return FALSE;
    }

    OSReport("[PC] GCI: gameName='%c%c%c%c' company='%c%c' fileName='%.32s'\n",
             dir_hdr.gameName[0], dir_hdr.gameName[1],
             dir_hdr.gameName[2], dir_hdr.gameName[3],
             dir_hdr.company[0], dir_hdr.company[1],
             dir_hdr.fileName);
    if (memcmp(dir_hdr.gameName, "GAF", 3) != 0) {
        OSReport("[PC] GCI: not an Animal Crossing save (expected GAFx, got '%.4s')\n",
                 dir_hdr.gameName);
        fclose(fp);
        return FALSE;
    }

    file_data = (u8*)malloc(GCI_FILE_DATA_SIZE);
    if (!file_data) {
        OSReport("[PC] GCI: malloc(%u) failed\n", (unsigned)GCI_FILE_DATA_SIZE);
        fclose(fp);
        return FALSE;
    }

    if (fread(file_data, GCI_FILE_DATA_SIZE, 1, fp) != 1) {
        OSReport("[PC] GCI: failed to read %u bytes of file data (file may be too small)\n",
                 (unsigned)GCI_FILE_DATA_SIZE);
        fclose(fp);
        free(file_data);
        return FALSE;
    }
    fclose(fp);

    save_src = (Save_t*)(file_data + GCI_SAVE_MAIN_OFFSET);
    pc_save_bswap_verify_roundtrip((const u8*)save_src, sizeof(Save_t));

    memcpy(&common_data.save.save, save_src, sizeof(Save_t));
    pc_save_bswap(&common_data.save.save, PC_BSWAP_FROM_BE);

    /* --- Load ARAM blocks from Others section ---
     * Current saves (PC + Dolphin/GC) use order: mail, original, diary.
     * Old PC saves (before landid fix) used: original, mail, diary.
     * Detect by checking the landid field at offset 2 of the first block:
     * if it matches save's land_info.id, first block is mail (GC order). */
    {
        u8* others_ptr = file_data + GCI_OTHERS_OFFSET;
        u32 block_start = ALIGN_NEXT(sizeof(MemcardHeader_c) + 32, 32);
        u16 first_landid = ((u16)others_ptr[block_start + 2] << 8) | others_ptr[block_start + 3];
        u16 save_land_id = common_data.save.save.land_info.id;
        int gc_order = (first_landid == save_land_id && save_land_id != 0);
        u32 mail_size = l_aram_alloc_size_table[mCD_ARAM_DATA_MAIL];
        u32 orig_size = l_aram_alloc_size_table[mCD_ARAM_DATA_ORIGINAL];
        u32 off_mail, off_orig, off_diary;

        if (gc_order) {
            /* Dolphin/GC save: mail, original, diary */
            off_mail = block_start;
            off_orig = ALIGN_NEXT(block_start + mail_size, 32);
            off_diary = ALIGN_NEXT(off_orig + orig_size, 32);
        } else {
            /* PC save: original, mail, diary */
            off_orig = block_start;
            off_mail = ALIGN_NEXT(block_start + orig_size, 32);
            off_diary = ALIGN_NEXT(off_mail + mail_size, 32);
        }

        if (l_aram_block_p_table[mCD_ARAM_DATA_MAIL]) {
            pc_save_bswap_verify_roundtrip_mail(others_ptr + off_mail, mail_size);
            memcpy(l_aram_block_p_table[mCD_ARAM_DATA_MAIL], others_ptr + off_mail, mail_size);
            pc_save_bswap_keep_mail((mCD_keep_mail_c*)l_aram_block_p_table[mCD_ARAM_DATA_MAIL],
                                    PC_BSWAP_FROM_BE);
        }
        if (l_aram_block_p_table[mCD_ARAM_DATA_ORIGINAL]) {
            pc_save_bswap_verify_roundtrip_original(others_ptr + off_orig, orig_size);
            memcpy(l_aram_block_p_table[mCD_ARAM_DATA_ORIGINAL], others_ptr + off_orig, orig_size);
            pc_save_bswap_keep_original((mCD_keep_original_c*)l_aram_block_p_table[mCD_ARAM_DATA_ORIGINAL],
                                        PC_BSWAP_FROM_BE);
        }
        if (l_aram_block_p_table[mCD_ARAM_DATA_DIARY]) {
            pc_save_bswap_verify_roundtrip_diary(others_ptr + off_diary,
                                                  l_aram_alloc_size_table[mCD_ARAM_DATA_DIARY]);
            memcpy(l_aram_block_p_table[mCD_ARAM_DATA_DIARY], others_ptr + off_diary,
                   l_aram_alloc_size_table[mCD_ARAM_DATA_DIARY]);
            pc_save_bswap_keep_diary((mCD_keep_diary_c*)l_aram_block_p_table[mCD_ARAM_DATA_DIARY],
                                     PC_BSWAP_FROM_BE);
        }
    }

    free(file_data);
    return TRUE;
}

/* Read a GCI file's Save_t into a provided buffer (for Card B — does NOT touch common_data).
 * Also loads ARAM blocks (mail/original/diary) into the l_keep* buffers.
 * Returns TRUE on success. */
static int pc_save_read_gci_to_keep(const char* path) {
    FILE* fp;
    CARDDir dir_hdr;
    u8* file_data;
    Save_t* save_src;
    u32 offset;

    fp = fopen(path, "rb");
    if (!fp) return FALSE;

    if (fread(&dir_hdr, GCI_HEADER_SIZE, 1, fp) != 1) { fclose(fp); return FALSE; }
    if (memcmp(dir_hdr.gameName, "GAF", 3) != 0) { fclose(fp); return FALSE; }

    file_data = (u8*)malloc(GCI_FILE_DATA_SIZE);
    if (!file_data) { fclose(fp); return FALSE; }

    if (fread(file_data, GCI_FILE_DATA_SIZE, 1, fp) != 1) {
        fclose(fp); free(file_data); return FALSE;
    }
    fclose(fp);

    /* Load Save_t into l_keepSave (try main, fall back to backup) */
    save_src = (Save_t*)(file_data + GCI_SAVE_MAIN_OFFSET);
    memcpy(&l_keepSave.save, save_src, sizeof(Save_t));
    pc_save_bswap(&l_keepSave.save, PC_BSWAP_FROM_BE);

    /* Validate — if main is corrupt, try backup */
    if (!mLd_CheckId(l_keepSave.save.land_info.id)) {
        OSReport("[PC] Card B: main save invalid, trying backup\n");
        save_src = (Save_t*)(file_data + GCI_SAVE_BACK_OFFSET);
        memcpy(&l_keepSave.save, save_src, sizeof(Save_t));
        pc_save_bswap(&l_keepSave.save, PC_BSWAP_FROM_BE);
        if (!mLd_CheckId(l_keepSave.save.land_info.id)) {
            OSReport("[PC] Card B: backup save also invalid\n");
            free(file_data);
            return FALSE;
        }
    }

    /* Load ARAM blocks — detect GC vs legacy PC order (same landid check as main load) */
    {
        u8* others_ptr = file_data + GCI_OTHERS_OFFSET;
        u32 block_start = ALIGN_NEXT(sizeof(MemcardHeader_c) + 32, 32);
        u16 first_landid = ((u16)others_ptr[block_start + 2] << 8) | others_ptr[block_start + 3];
        u16 save_land_id = l_keepSave.save.land_info.id;
        int gc_order = (first_landid == save_land_id && save_land_id != 0);
        u32 mail_size = l_aram_alloc_size_table[mCD_ARAM_DATA_MAIL];
        u32 orig_size = l_aram_alloc_size_table[mCD_ARAM_DATA_ORIGINAL];
        u32 off_mail, off_orig, off_diary;

        if (gc_order) {
            off_mail = block_start;
            off_orig = ALIGN_NEXT(block_start + mail_size, 32);
            off_diary = ALIGN_NEXT(off_orig + orig_size, 32);
        } else {
            off_orig = block_start;
            off_mail = ALIGN_NEXT(block_start + orig_size, 32);
            off_diary = ALIGN_NEXT(off_mail + mail_size, 32);
        }

        memcpy(&l_keepMail, others_ptr + off_mail, mail_size);
        pc_save_bswap_keep_mail(&l_keepMail, PC_BSWAP_FROM_BE);

        memcpy(&l_keepOriginal, others_ptr + off_orig, orig_size);
        pc_save_bswap_keep_original(&l_keepOriginal, PC_BSWAP_FROM_BE);

        memcpy(&l_keepDiary, others_ptr + off_diary, l_aram_alloc_size_table[mCD_ARAM_DATA_DIARY]);
        pc_save_bswap_keep_diary(&l_keepDiary, PC_BSWAP_FROM_BE);
    }

    free(file_data);
    l_keepSave_set = TRUE;
    OSReport("[PC] Card B: loaded town '%.*s' (id=0x%04X) from '%s'\n",
             8, l_keepSave.save.land_info.name,
             l_keepSave.save.land_info.id, path);
    return TRUE;
}

/* Migrate legacy flat save/ layout to save/card_a/ */
static void pc_save_migrate_legacy(void) {
    struct stat st_legacy, st_new;

    if (stat(PC_GCI_PATH_LEGACY, &st_legacy) == 0 &&
        stat(PC_GCI_PATH, &st_new) != 0) {
        int b;
        OSReport("[PC] Migrating save from '%s' to '%s'\n", PC_GCI_PATH_LEGACY, PC_GCI_PATH);
        pc_ensure_save_dirs();

        /* Move main save */
        remove(PC_GCI_PATH); /* in case it somehow exists */
        rename(PC_GCI_PATH_LEGACY, PC_GCI_PATH);

        /* Move backups */
        for (b = 1; b <= PC_SAVE_MAX_BACKUPS; b++) {
            char old_bak[300], new_bak[300];
            snprintf(old_bak, sizeof(old_bak), "%s.bak%d", PC_GCI_PATH_LEGACY, b);
            snprintf(new_bak, sizeof(new_bak), "%s.bak%d", PC_GCI_PATH, b);
            remove(new_bak);
            rename(old_bak, new_bak);
        }

        /* Move temp file if orphaned */
        {
            char old_tmp[300];
            snprintf(old_tmp, sizeof(old_tmp), "%s.tmp", PC_GCI_PATH_LEGACY);
            remove(PC_GCI_TMP_PATH);
            rename(old_tmp, PC_GCI_TMP_PATH);
        }

        OSReport("[PC] Migration complete\n");
    }
}

static int pc_save_scan_gci_dir(void) {
    /* Try common AC save filenames in card_a/ */
    static const char* gci_names[] = {
        PC_CARD_A_DIR "/DobutsunomoriP_MURA.gci",
        PC_CARD_A_DIR "/8P-GAFE-DobutsunomoriP_MURA.gci",
        NULL
    };
    int i;
    struct stat st;

    for (i = 0; gci_names[i] != NULL; i++) {
        if (stat(gci_names[i], &st) == 0) {
            OSReport("[PC] GCI scan: found '%s'\n", gci_names[i]);
            if (pc_save_read_gci(gci_names[i])) {
                return TRUE;
            }
        }
    }

    /* Also try dynamic scan of card_a/ for any GCI */
    {
        char found_path[300];
        if (pc_card_scan_for_gci(0, found_path, sizeof(found_path))) {
            OSReport("[PC] GCI scan: found '%s' via directory scan\n", found_path);
            if (pc_save_read_gci(found_path)) {
                return TRUE;
            }
        }
    }

    return FALSE;
}

/* Reload save from GCI file on disk. PC equivalent of GC re-reading the
 * memory card. */
int pc_save_reload(void) {
    struct stat st;
    if (!pc_save_loaded) return 0;
    if (stat(PC_GCI_PATH, &st) == 0) {
        return pc_save_read_gci(PC_GCI_PATH);
    }
    return pc_save_scan_gci_dir();
}

int pc_save_check_and_load(void) {
    struct stat st;
    {
        char cwd[512];
        if (getcwd(cwd, sizeof(cwd))) {
            OSReport("[PC] Save: current working directory = '%s'\n", cwd);
        }
    }

    pc_ensure_save_dirs();
    pc_save_migrate_legacy();

    if (stat(PC_GCI_PATH, &st) == 0) {
        OSReport("[PC] Found GCI save: %s (%ld bytes)\n", PC_GCI_PATH, (long)st.st_size);
        if (pc_save_read_gci(PC_GCI_PATH)) {
            OSReport("[PC] GCI save loaded successfully\n");
            return TRUE;
        }
        OSReport("[PC] GCI save load FAILED\n");
    } else {
        OSReport("[PC] No GCI save at %s\n", PC_GCI_PATH);
    }

    OSReport("[PC] Scanning for other GCI files...\n");
    if (pc_save_scan_gci_dir()) {
        OSReport("[PC] GCI save loaded via scan\n");
        return TRUE;
    }

    /* recovery: try temp file, then backups */
    if (stat(PC_GCI_TMP_PATH, &st) == 0) {
        OSReport("[PC] Found orphaned temp save '%s', recovering...\n", PC_GCI_TMP_PATH);
        if (rename(PC_GCI_TMP_PATH, PC_GCI_PATH) == 0 && pc_save_read_gci(PC_GCI_PATH)) {
            OSReport("[PC] Recovered save from temp file\n");
            return TRUE;
        }
    }
    {
        char bak_path[300];
        int b;
        for (b = 1; b <= PC_SAVE_MAX_BACKUPS; b++) {
            snprintf(bak_path, sizeof(bak_path), "%s.bak%d", PC_GCI_PATH, b);
            if (stat(bak_path, &st) == 0) {
                OSReport("[PC] Found backup save '%s', recovering...\n", bak_path);
                if (pc_save_read_gci(bak_path)) {
                    OSReport("[PC] Recovered save from backup %d\n", b);
                    return TRUE;
                }
            }
        }
    }

    OSReport("[PC] No save file found\n");
    return FALSE;
}

/* --- Card B scanning --- */

/* Check if Card B directory has a valid AC town GCI */
static int pc_card_b_find_town(void) {
    if (pc_card_scan_for_gci(1, l_card_b_gci_path, sizeof(l_card_b_gci_path))) {
        OSReport("[PC] Card B: found GCI at '%s'\n", l_card_b_gci_path);
        return TRUE;
    }
    l_card_b_gci_path[0] = '\0';
    return FALSE;
}

/* --- Memory card API implementation --- */

void mCD_init_card(void) {
    CARDInit();
}

void mCD_InitAll(void) {
    /* ARAM blocks must persist -- don't null them */
    memset(&l_mcd_foreigner_file, 0, sizeof(l_mcd_foreigner_file));
    l_keepSave_set = FALSE;
    l_mcd_keep_startCond = 0;
    l_card_b_gci_path[0] = '\0';
}

int mCD_InitGameStart_bg(int player_no, int card_private_idx, int start_cond, s32* mounted_chan) {
    static int init_done = 0;

    /* On GC, save is re-read from the memory card each game start.
     * On PC, the save was already reloaded from disk in common_data_reinit
     * and aAL_title_game_data_init_start_select. We just need to allow
     * mSDI_StartDataInit to re-process it. */
    if (init_done && pc_save_loaded) {
        init_done = 0;
    }

    if (!init_done) {
        init_done = 1; /* before call -prevents re-entry via crash recovery */

        if (pc_save_loaded) {
            static int init_mode_table[] = { mSDI_INIT_MODE_NEW, mSDI_INIT_MODE_FROM,
                                             mSDI_INIT_MODE_NEW_PLAYER, mSDI_INIT_MODE_PAK,
                                             mSDI_INIT_MODE_PAK };
            int mode = mSDI_INIT_MODE_FROM;
            if (start_cond >= 0 && start_cond < 5) {
                mode = init_mode_table[start_cond];
            }
            mSDI_StartDataInit(gamePT, player_no, mode);
            l_mcd_keep_startCond = start_cond;
            pc_save_ready = 1;

            /* Reset detection (Resetti): check if previous session ended without saving */
            if (Now_Private != NULL && start_cond != mCD_START_COND_INCOMING_FOREIGNER
                && start_cond != mCD_START_COND_OUTGOING_FOREIGNER) {
                Common_Set(reset_flag, FALSE);
                if (Now_Private->reset_code != 0) {
                    /* Birthday clears reset penalty (matches original) */
                    if (Now_Private->state_flags & mPr_FLAG_BIRTHDAY_ACTIVE) {
                        Now_Private->reset_code = 0;
                    } else if (!ZURUMODE2_ENABLED() && !g_pc_settings.disable_resetti) {
                        Common_Set(reset_flag, TRUE);
                        Now_Private->reset_count++;
                        OSReport("[PC] Reset detected! reset_count=%d — Resetti will appear\n",
                                 Now_Private->reset_count);
                    }
                }
                /* Arm reset code: if player quits without saving, next load detects it */
                Now_Private->reset_code = (u32)RANDOM_F(USHT_MAX_S);
                Now_Private->reset_code++;
            }

            /* Handle foreigner start conditions */
            if (start_cond == mCD_START_COND_INCOMING_FOREIGNER) {
                /* Arriving at another town as foreigner — point now_private to passport */
                Common_Set(now_private, &l_mcd_foreigner_file.file.priv);
                Common_Set(player_no, mPr_FOREIGNER);
                OSReport("[PC] InitGameStart: INCOMING_FOREIGNER — player is visiting\n");
            } else if (start_cond == mCD_START_COND_OUTGOING_FOREIGNER) {
                /* Returning home. Matches m_card.c:4780 (GC case 4): merge
                 * the session passport back into the matching home save slot
                 * — this restores player_no/now_private and carries visit
                 * inventory changes into the home save. */
                Private_c* foreigner = mPr_GetForeignerP();
                mPr_CopyPrivateInfo(foreigner, &l_mcd_foreigner_file.file.priv);
                mPr_LoadPak_and_SetPrivateInfo2(foreigner, (u8)player_no);
                OSReport("[PC] InitGameStart: OUTGOING_FOREIGNER — landed player_no=%d\n",
                         Common_Get(player_no));
            }
        } else if (start_cond == mCD_START_COND_0 || start_cond == mCD_START_COND_2) {
            mSDI_StartDataInit(gamePT, player_no, mSDI_INIT_MODE_NEW);
            pc_save_ready = 1;
        }
    }

    if (mounted_chan) *mounted_chan = mCD_SLOT_A;
    return mCD_TRANS_ERR_NONE;
}

void mCD_LoadLand(void) {
    (void)pc_save_loaded;
}

// match GC mCD_get_land_copyProtect (static in m_card.c so we replicate).
static u16 pc_land_copy_protect(void) {
    u16 code = (u16)RANDOM(0xFFF0);
    return code + 1;
}

// lightweight prep that's safe to run on every save (including the 1-min
// auto-save tick). just updates timers and flags that the game expects
// to be current; doesn't touch any player-visible state.
static void pc_save_prep_incremental(void) {
    mCkRh_SavePlayTime(Common_Get(player_no));
    if (Now_Private != NULL) Now_Private->reset_code = 0;
    Save_Set(save_exist, TRUE);
}

// full prep used when the PLAYER explicitly chose to save (gyroid, porter,
// train travel). runs the incremental prep plus the GC-intended side
// effects of a committed save:
//   - harden money rocks + clear shine spots (prevents save-scumming the
//     rock + scumming buried money across loads)
//   - wipe Wisp spirits from inventory (they're not supposed to persist
//     past the save point where the quest was active)
//   - refresh travel_hard_time (travel cooldown anchor)
//   - refresh copy_protect (per-save random tag)
//
// DO NOT call this from the auto-save tick. running it every minute
// would clobber money rocks mid-hit, make shine spots disappear, and
// wipe Wisp items the player is actively using in a quest.
static void pc_save_prep_explicit(void) {
    pc_save_prep_incremental();

    if (Now_Private != NULL) {
        mActor_name_t* pockets = Now_Private->inventory.pockets;
        for (int i = 0; i < mPr_POCKETS_SLOT_COUNT; i++) {
            if (ITEM_IS_WISP(pockets[i])) {
                mPr_SetPossessionItem(Now_Private, i, EMPTY_NO, mPr_ITEM_COND_NORMAL);
            }
        }
    }

    mAGrw_ClearMoneyStoneShineGround();

    Save_Set(travel_hard_time, lbRTC_HardTime());
    {
        u16 cp = pc_land_copy_protect();
        Common_Set(copy_protect, cp);
        Save_Set(copy_protect, cp);
    }
}

// re-arm reset code after a successful save so a crash before the next
// save is detectable as "quit without saving" on the following load.
static void pc_save_rearm_reset_code(void) {
    if (Now_Private != NULL) {
        Now_Private->reset_code = (u32)RANDOM_F(USHT_MAX_S);
        Now_Private->reset_code++;
    }
}

// set by auto-save before calling mCD_SaveHome_bg so the function runs
// only the incremental prep (skips Wisp clear, money-rock harden, etc).
// the game's own save paths (gyroid, porter) leave this 0 and get the
// full explicit-save prep.
static int s_pc_save_is_incremental = 0;

int mCD_SaveHome_bg(int param_1, int* chan) {
    int slot = mCD_GetThisLandSlotNo();
    int result;
    (void)param_1;

    if (s_pc_save_is_incremental) {
        pc_save_prep_incremental();
    } else {
        pc_save_prep_explicit();
    }

    if (slot == mCD_SLOT_B && l_card_b_gci_path[0] != '\0') {
        // visiting another town; save back to Card B
        char tmp_path[300];
        snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", l_card_b_gci_path);
        result = pc_save_write_gci_to(l_card_b_gci_path, tmp_path);
        if (chan) *chan = mCD_SLOT_B;
    } else {
        result = pc_save_write_gci();
        if (chan) *chan = mCD_SLOT_A;
    }

    if (!result) {
        OSReport("[PC] mCD_SaveHome_bg: save failed!\n");
        return mCD_TRANS_ERR_IOERROR;
    }

    pc_save_rearm_reset_code();
    return mCD_TRANS_ERR_NONE;
}

// auto-save driven from the per-frame poll. only fires when the player
// is outdoors in the town overworld (SCENE_FG), since sub-scenes leave
// Save_t in a transient state the writer can't roundtrip. calls the same
// mCD_SaveHome_bg path the gyroid uses so the disk format matches a
// normal save.
//
// the actual save-on-quit hook is in vita_platform.c's scePower callback
// (PS button press). this periodic tick is a safety net for crashes and
// hard reboots where no power event fires.
#define PC_AUTO_SAVE_INTERVAL_SEC 60
#define PC_AUTO_SAVE_RETRY_SEC    10

static time_t s_pc_auto_save_last = 0;

// mirror the gyroid's pre-save prep. buildings / items / FG actors track
// their state in actor structs while the game runs; Actor_info_save_actor
// walks the actor list and calls each sv_proc to flush that state back
// into Save_t. skipping this writes a stale combi_table + fg array, which
// looks like vanished buildings on reload. mNtc_set_auto_nwrite_data
// flushes the noticeboard auto-write buffer same as the gyroid talk.
static void pc_auto_save_flush_world_state(void) {
    GAME_PLAY* play = (GAME_PLAY*)gamePT;
    if (play == NULL) return;
    Actor_info_save_actor(play);
    mNtc_set_auto_nwrite_data();
}

// only allow auto-save when the player is unambiguously in free control
// (walk / run / wait). anything else means they're in a menu, an NPC
// conversation, an item pickup animation, a demo cutscene, or a save
// dialog where the game is managing Save_t itself. auto-saving through
// any of those can stall porter's state machine (the bug that hung the
// "saving town data" dialogue) or write a torn Save_t.
static int pc_auto_save_player_busy(void) {
    GAME_PLAY* play = (GAME_PLAY*)gamePT;
    if (play == NULL) return 1; // no play context, don't risk it
    int idx = mPlib_get_player_actor_main_index((GAME*)play);
    switch (idx) {
        case mPlayer_INDEX_WAIT:
        case mPlayer_INDEX_WALK:
        case mPlayer_INDEX_RUN:
        case mPlayer_INDEX_DASH:
        case mPlayer_INDEX_TURN_DASH:
            return 0; // safe to save
        default:
            return 1; // every other state is hands-off
    }
}

void pc_auto_save_tick(void) {
    time_t now;

    if (!g_pc_settings.auto_save) {
        // feature off, reset timer so a later toggle-on starts fresh
        s_pc_auto_save_last = 0;
        return;
    }
    if (!pc_save_loaded || !pc_save_ready) return;
    if (Now_Private == NULL) return;
    if (Save_Get(scene_no) != SCENE_FG) return;
    if (pc_auto_save_player_busy()) return;

    now = time(NULL);
    if (s_pc_auto_save_last == 0) {
        s_pc_auto_save_last = now;
        return;
    }
    if ((now - s_pc_auto_save_last) < PC_AUTO_SAVE_INTERVAL_SEC) return;

    {
        int chan = 0;
        int result;
        pc_auto_save_flush_world_state();
        s_pc_save_is_incremental = 1;
        result = mCD_SaveHome_bg(0, &chan);
        s_pc_save_is_incremental = 0;
        if (result == mCD_TRANS_ERR_NONE) {
            OSReport("[PC] Auto-save complete (chan=%d)\n", chan);
            s_pc_auto_save_last = now;
        } else {
            OSReport("[PC] Auto-save failed: %d (retry in %ds)\n",
                     result, PC_AUTO_SAVE_RETRY_SEC);
            s_pc_auto_save_last = now - PC_AUTO_SAVE_INTERVAL_SEC + PC_AUTO_SAVE_RETRY_SEC;
        }
    }
}

// force a save now, ignoring the periodic interval. same safety checks
// as the tick. called from the power callback (PS button / sleep) and on
// long-suspend resume. returns 1 if a save was written, 0 if skipped.
int pc_auto_save_force(void) {
    int chan = 0;
    int result;

    if (!g_pc_settings.auto_save) return 0;
    if (!pc_save_loaded || !pc_save_ready) return 0;
    if (Now_Private == NULL) return 0;
    if (Save_Get(scene_no) != SCENE_FG) {
        OSReport("[PC] Forced save skipped: not in town overworld (scene=%d)\n",
                 (int)Save_Get(scene_no));
        return 0;
    }
    if (pc_auto_save_player_busy()) {
        // the game is handling its own save (porter, gyroid, etc) so
        // let it finish, we'll catch the next suspend or tick
        OSReport("[PC] Forced save skipped: player busy in dialogue/demo\n");
        return 0;
    }

    pc_auto_save_flush_world_state();
    s_pc_save_is_incremental = 1;
    result = mCD_SaveHome_bg(0, &chan);
    s_pc_save_is_incremental = 0;
    if (result == mCD_TRANS_ERR_NONE) {
        OSReport("[PC] Forced save complete (chan=%d)\n", chan);
        s_pc_auto_save_last = time(NULL);
        return 1;
    }
    OSReport("[PC] Forced save failed: %d\n", result);
    return 0;
}

/* --- Travel / Station functions --- */

/* Read a GCI's Save_t into `out` (byte-swapped). Returns TRUE on success. */
static int pc_read_gci_land_info(const char* path, Save_t* out) {
    FILE* fp;
    CARDDir hdr;
    u8* file_data;
    int ok = FALSE;

    fp = fopen(path, "rb");
    if (!fp) return FALSE;

    if (fread(&hdr, GCI_HEADER_SIZE, 1, fp) == 1 &&
        memcmp(hdr.gameName, "GAF", 3) == 0) {
        file_data = (u8*)malloc(GCI_FILE_DATA_SIZE);
        if (file_data) {
            if (fread(file_data, GCI_FILE_DATA_SIZE, 1, fp) == 1) {
                Save_t* save_src = (Save_t*)(file_data + GCI_SAVE_MAIN_OFFSET);
                memcpy(out, save_src, sizeof(Save_t));
                pc_save_bswap(out, PC_BSWAP_FROM_BE);
                ok = TRUE;
            }
            free(file_data);
        }
    }
    fclose(fp);
    return ok;
}

// scan the "other" slot for a travel-eligible town.
//  - resident: check card B for a different town
//  - foreigner: check card A for the home town
//
// return value drives porter's dialogue in aSTM_cardproc:
//   NONE_NEXTLAND    - travel is possible, porter offers "visit" flow
//   NO_TOWN_DATA     - the other slot has nothing (or nothing valid);
//                      porter shows "no travel data" and closes the dialog
//                      instead of dragging the player through a pointless
//                      save flow
//   CORRUPT          - other slot has a GCI but its land_info is broken
//   NONE             - other slot exists but holds our own town, fall
//                      through so porter handles whatever the train is
//                      doing today
int mCD_CheckStation_bg(s32* chan) {
    int is_foreigner = mLd_PlayerManKindCheck();

    if (is_foreigner) {
        // visitor looking to go home: read card A
        Save_t temp_save;
        if (chan) *chan = mCD_SLOT_B;
        if (!pc_read_gci_land_info(PC_GCI_PATH, &temp_save)) {
            OSReport("[PC] CheckStation: card A unreadable, no home to return to\n");
            return mCD_TRANS_ERR_NO_TOWN_DATA;
        }
        if (!mLd_CheckId(temp_save.land_info.id)) {
            OSReport("[PC] CheckStation: card A has invalid land_info\n");
            return mCD_TRANS_ERR_CORRUPT;
        }
        if (mLd_CheckThisLand(temp_save.land_info.name, temp_save.land_info.id)) {
            // Card A holds the same town we're currently in — shouldn't
            // happen unless something got mis-copied; let porter play a
            // normal non-travel flow instead of erroring out
            OSReport("[PC] CheckStation: card A is the same town we're in\n");
            return mCD_TRANS_ERR_NONE;
        }
        OSReport("[PC] CheckStation: card A has '%.*s' (id=0x%04X), return available\n",
                 8, temp_save.land_info.name, temp_save.land_info.id);
        if (chan) *chan = mCD_SLOT_A;
        return mCD_TRANS_ERR_NONE_NEXTLAND;
    }

    // resident looking to visit: read card B
    if (chan) *chan = mCD_SLOT_A;
    if (!pc_card_b_find_town()) {
        OSReport("[PC] CheckStation: card B is empty, nothing to visit\n");
        return mCD_TRANS_ERR_NO_TOWN_DATA;
    }
    {
        Save_t temp_save;
        if (!pc_read_gci_land_info(l_card_b_gci_path, &temp_save)) {
            OSReport("[PC] CheckStation: card B GCI unreadable\n");
            return mCD_TRANS_ERR_CORRUPT;
        }
        if (!mLd_CheckId(temp_save.land_info.id)) {
            OSReport("[PC] CheckStation: card B has invalid land_info\n");
            return mCD_TRANS_ERR_CORRUPT;
        }
        if (mLd_CheckThisLand(temp_save.land_info.name, temp_save.land_info.id)) {
            OSReport("[PC] CheckStation: card B is the same town as card A\n");
            return mCD_TRANS_ERR_NONE;
        }
        OSReport("[PC] CheckStation: card B has '%.*s' (id=0x%04X), travel available\n",
                 8, temp_save.land_info.name, temp_save.land_info.id);
        if (chan) *chan = mCD_SLOT_B;
        return mCD_TRANS_ERR_NONE_NEXTLAND;
    }
}

// record the departure town in travel_persistent_data so rover's dialogue
// on the return trip has the right town name / player IDs.
static void pc_save_record_departure(void) {
    mCD_persistent_data_c* persistant = Common_GetPointer(travel_persistent_data);
    memcpy(&persistant->land, Save_GetPointer(land_info), sizeof(mLd_land_info_c));
    for (int i = 0; i < PLAYER_NUM; i++) {
        mPr_CopyPersonalID(&persistant->pid[i], &Save_Get(private_data[i]).player_ID);
    }
}

// build the foreigner file (passport) from Now_Private plus the pending
// moveout animal (if any). matches GC mCD_SetForeignerFile semantics.
static void pc_build_foreigner_file(void) {
    memset(&l_mcd_foreigner_file, 0, sizeof(l_mcd_foreigner_file));
    if (Now_Private != NULL) {
        mPr_CopyPrivateInfo(&l_mcd_foreigner_file.file.priv, Now_Private);
    }
    // mNpc_GetRemoveAnimal moves a pending-moveout villager (if any) into
    // the passed Animal_c so the visit carries them along. it's a no-op
    // when no villager is scheduled to leave.
    {
        Animal_c* in_animal = mNpc_GetInAnimalP();
        mNpc_GetRemoveAnimal(in_animal, TRUE);
        memcpy(&l_mcd_foreigner_file.file.remove_animal, in_animal, sizeof(Animal_c));
    }
    l_mcd_foreigner_file.file.copy_protect = (u16)Common_Get(copy_protect);
    l_mcd_foreigner_file.file.checksum = 0;
    l_mcd_foreigner_file.file.checksum =
        mFRm_GetFlatCheckSum((u16*)&l_mcd_foreigner_file.file,
                             sizeof(mCD_foreigner_c),
                             l_mcd_foreigner_file.file.checksum);
}

// persist current town and swap in the other town via l_keepSave.
//  - resident: save card A (home) with player flagged "away", load card B
//  - foreigner: refresh passport, save card B (visit state), load card A
int mCD_SaveStation_NextLand_bg(s32* chan) {
    int is_foreigner = mLd_PlayerManKindCheck();
    OSReport("[PC] SaveStation_NextLand_bg: enter (is_foreigner=%d)\n", is_foreigner);

    if (is_foreigner) {
        // returning visitor -> refresh passport from current state so
        // inventory / item changes during the visit carry home, then save
        // the visited town with player state intact.
        pc_save_prep_explicit();
        pc_save_record_departure();

        if (Now_Private != NULL) {
            mPr_CopyPrivateInfo(&l_mcd_foreigner_file.file.priv, Now_Private);
            l_mcd_foreigner_file.file.checksum = 0;
            l_mcd_foreigner_file.file.checksum =
                mFRm_GetFlatCheckSum((u16*)&l_mcd_foreigner_file.file,
                                     sizeof(mCD_foreigner_c),
                                     l_mcd_foreigner_file.file.checksum);
            OSReport("[PC] SaveStation_NextLand(return): refreshed passport for '%.*s'\n",
                     PLAYER_NAME_LEN, l_mcd_foreigner_file.file.priv.player_ID.player_name);
        }

        if (l_card_b_gci_path[0] != '\0') {
            char tmp_path[320];
            snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", l_card_b_gci_path);
            if (!pc_save_write_gci_to(l_card_b_gci_path, tmp_path)) {
                OSReport("[PC] SaveStation_NextLand(return): failed to save visited town\n");
                if (chan) *chan = mCD_SLOT_B;
                return mCD_TRANS_ERR_IOERROR;
            }
        } else {
            OSReport("[PC] SaveStation_NextLand(return): no Card B path cached\n");
        }

        if (!pc_save_read_gci_to_keep(PC_GCI_PATH)) {
            OSReport("[PC] SaveStation_NextLand(return): failed to load home town\n");
            if (chan) *chan = mCD_SLOT_A;
            return mCD_TRANS_ERR_CORRUPT;
        }

        l_mcd_keep_startCond = mCD_START_COND_OUTGOING_FOREIGNER;
        pc_save_rearm_reset_code();
        if (chan) *chan = mCD_SLOT_A;
        return mCD_TRANS_ERR_NONE;
    }

    // resident departing to visit another town
    if (chan) *chan = mCD_SLOT_A;
    if (l_card_b_gci_path[0] == '\0') {
        OSReport("[PC] SaveStation_NextLand: no Card B path\n");
        return mCD_TRANS_ERR_NO_TOWN_DATA;
    }

    // run the common prep (cockroach, Wisps, money stones, copy_protect,
    // travel_hard_time) BEFORE marking the player as away so the passport
    // captures the cleaned-up state.
    pc_save_prep_explicit();
    pc_save_record_departure();

    // save current house size into Save_t.keep_house_size so the visiting
    // town's world knows what size to build when we return
    mHm_KeepHouseSize(Common_Get(player_no));

    // build the passport (carries player private data + any scheduled-
    // moveout villager into the visit)
    pc_build_foreigner_file();
    OSReport("[PC] SaveStation_NextLand: built passport for '%.*s'\n",
             PLAYER_NAME_LEN, l_mcd_foreigner_file.file.priv.player_ID.player_name);

    // flag the player as "away" on card A. if the player quits mid-visit,
    // the next load sees exists==FALSE and gives them the gyroid face +
    // wiped inventory (see m_start_data_init.c:426).
    if (Now_Private != NULL && mLd_PlayerManKindCheckNo(Common_Get(player_no)) == FALSE) {
        Now_Private->exists = FALSE;
        OSReport("[PC] SaveStation_NextLand: marked player as away (exists=FALSE)\n");
    }

    if (!pc_save_write_gci()) {
        if (Now_Private != NULL) Now_Private->exists = TRUE;
        OSReport("[PC] SaveStation_NextLand: failed to save home town\n");
        return mCD_TRANS_ERR_IOERROR;
    }

    if (!pc_save_read_gci_to_keep(l_card_b_gci_path)) {
        OSReport("[PC] SaveStation_NextLand: failed to load Card B town\n");
        return mCD_TRANS_ERR_CORRUPT;
    }

    l_mcd_keep_startCond = mCD_START_COND_INCOMING_FOREIGNER;
    if (chan) *chan = mCD_SLOT_B;
    return mCD_TRANS_ERR_NONE;
}

// station save when CheckStation returned NONE (no valid travel target).
// porter still offers this flow if a train is parked in the station. on GC
// the passport-only save just writes a visitor file; on PC we want the
// home town saved too so the player isn't stuck watching a never-ending
// "saving..." dialogue when card_b is empty. for foreigners the passport
// is already in memory (refreshed by SaveStation_NextLand_bg).
int mCD_SaveStation_Passport_bg(s32* chan) {
    int is_foreigner = mLd_PlayerManKindCheck();
    if (chan) *chan = mCD_SLOT_B;

    OSReport("[PC] SaveStation_Passport_bg: enter (is_foreigner=%d)\n", is_foreigner);

    if (is_foreigner) {
        // foreigner at visited town's station without returning home (no
        // Card A to travel back to). nothing to write; passport is already
        // in memory.
        OSReport("[PC] SaveStation_Passport: passport held in memory\n");
        return mCD_TRANS_ERR_NONE;
    }

    // resident using porter to save at the station without travel. same
    // on-disk result as a gyroid save. shares the full prep pipeline so
    // Wisps, money stones, cockroach timer, etc. all get updated.
    pc_save_prep_explicit();
    if (!pc_save_write_gci()) {
        OSReport("[PC] SaveStation_Passport: home save failed\n");
        return mCD_TRANS_ERR_IOERROR;
    }
    pc_save_rearm_reset_code();
    OSReport("[PC] SaveStation_Passport: home save complete\n");
    return mCD_TRANS_ERR_NONE;
}

/* Transition to the other town. Called from scene cleanup when switching to the visited town.
 * Faithfully reproduces the original mCD_toNextLand from m_card.c:7188. */
void mCD_toNextLand(void) {
    Save_t* save = &l_keepSave.save;
    int scene_no;
    mCD_persistent_data_c persis;
    mLd_land_info_c* land_info;
    int last_scene_no;
    mActor_name_t last_field_id;
    s16 demo_profile[2];
    Time_c time;
    Transition_c transition;
    int rtc_enabled;

    if (l_keepSave_set != TRUE) {
        OSReport("[PC] toNextLand: l_keepSave not set, aborting\n");
        return;
    }

    land_info = &save->land_info;
    if (!mLd_CheckId(land_info->id)) {
        OSReport("[PC] toNextLand: invalid land_info in l_keepSave\n");
        return;
    }

    scene_no = Save_Get(scene_no);

    /* Save persistent data that must survive the common_data wipe */
    memcpy(&persis, Common_GetPointer(travel_persistent_data), sizeof(persis));
    last_scene_no = Common_Get(last_scene_no);
    last_field_id = Common_Get(last_field_id);
    memcpy(demo_profile, Common_Get(demo_profiles), sizeof(demo_profile));
    memcpy(&time, Common_GetPointer(time), sizeof(time));
    memcpy(&transition, Common_GetPointer(transition), sizeof(transition));

    /* Wipe common_data */
    memset(&common_data, 0, sizeof(common_data_t));

    /* Restore persistent fields */
    memcpy(Common_GetPointer(travel_persistent_data), &persis, sizeof(persis));
    Common_Set(last_field_id, last_field_id);
    Common_Set(last_scene_no, last_scene_no);
    memcpy(Common_Get(demo_profiles), demo_profile, sizeof(demo_profile));
    memcpy(Common_GetPointer(time), &time, sizeof(time));
    memcpy(Common_GetPointer(transition), &transition, sizeof(transition));

    /* Load the other town's save data */
    memcpy(Common_GetPointer(save), save, sizeof(Save));

    Common_Set(copy_protect, Save_Get(copy_protect));
    Save_Set(scene_no, scene_no);

    /* RTC check */
    rtc_enabled = Common_Get(time.rtc_enabled);
    Common_Set(time.rtc_enabled, TRUE);
    mTM_rtcTime_limit_check();
    Common_Set(time.rtc_enabled, rtc_enabled);
    lbRTC_GetTime(Common_GetPointer(time.rtc_time));

    /* Set current player as foreigner */
    Common_Set(now_private, &l_mcd_foreigner_file.file.priv);
    Common_Set(player_no, mPr_FOREIGNER);
    Common_Set(auto_nwrite_set, FALSE);
    memset(Common_GetPointer(auto_nwrite_time), 0, sizeof(Common_Get(auto_nwrite_time)));
    Common_Set(ball_pos, ZeroVec);

    /* Clear town-specific state */
    mTM_clear_renew_is();
    mEv_ClearEventInfo();
    mEv_toland_clear_common();
    mNpc_ClearInAnimal();
    mNpc_FirstClearGoodbyMail();
    mQst_ClearGrabItemInfo();
    memset(Common_Get(npc_schedule), 0, sizeof(Common_Get(npc_schedule)));
    mISL_ClearKeepIsland();
    memset(Common_GetPointer(unused_mail_26522), 0, sizeof(Mail_c));
    Common_Set(_2664E, 0);
    Common_Set(_26650, 0);
    mNpc_ClearCacheName();

    Common_Set(submenu_disabled, TRUE);

    /* Clear keepSave now that it's been applied */
    memset(&l_keepSave, 0, sizeof(Save));
    l_keepSave_set = FALSE;

    /* Load ARAM blocks from the other town */
    mCD_save_data_main_to_aram(&l_keepMail, l_aram_alloc_size_table[mCD_ARAM_DATA_MAIL], mCD_ARAM_DATA_MAIL);
    mCD_save_data_main_to_aram(&l_keepOriginal, l_aram_alloc_size_table[mCD_ARAM_DATA_ORIGINAL], mCD_ARAM_DATA_ORIGINAL);
    mCD_save_data_main_to_aram(&l_keepDiary, l_aram_alloc_size_table[mCD_ARAM_DATA_DIARY], mCD_ARAM_DATA_DIARY);

    OSReport("[PC] toNextLand: transitioned to visited town, player_no=%d\n",
             Common_Get(player_no));
}

/* Re-check and reload home town after returning from visit */
void mCD_ReCheckLoadLand(GAME_PLAY* play) {
    int scene = Save_Get(scene_no);

    /* Reload home town from Card A */
    pc_save_check_and_load();
    Save_Set(scene_no, scene);

    if (mFRm_CheckSaveData()) {
        int rtc_on = Common_Get(time.rtc_enabled);
        Common_Set(time.rtc_enabled, TRUE);
        mTM_rtcTime_limit_check();
        play->next_scene_no = SCENE_PLAYERSELECT_2;
        Common_Set(time.rtc_enabled, rtc_on);
        mEv_ClearEventInfo();
    } else {
        play->next_scene_no = SCENE_PLAYERSELECT;
        Common_Set(house_owner_name, RSV_NO);
        Common_Set(last_field_id, RSV_NO);
    }

    OSReport("[PC] ReCheckLoadLand: next_scene=%d\n", play->next_scene_no);
}

/* --- Remaining card management functions --- */

int mCD_GetThisLandSlotNo(void) {
    /* If visiting another town, current land is on Card B */
    if (Common_Get(player_no) == mPr_FOREIGNER) return mCD_SLOT_B;
    return mCD_SLOT_A;
}

int mCD_GetThisLandSlotNo_code(int* player_no, s32* slot_card_results) {
    int slot = mCD_GetThisLandSlotNo();

    if (player_no) *player_no = Common_Get(player_no);
    if (slot_card_results) {
        slot_card_results[mCD_SLOT_A] = CARD_RESULT_READY;
        slot_card_results[mCD_SLOT_B] = (slot == mCD_SLOT_B) ? CARD_RESULT_READY : CARD_RESULT_NOCARD;
    }
    return slot;
}

int mCD_GetSaveHomeSlotNo(void) {
    return mCD_SLOT_A;
}

int mCD_GetPlayerNum(void) {
    return 1;
}

int mCD_GetCardPrivateNameCopy(u8* name, int idx) {
    (void)name;
    (void)idx;
    return 0;
}

int mCD_CheckCardPlayerNative(int idx) {
    /* If foreigner mode is active, player 4 (mPr_FOREIGNER) is not native */
    if (Common_Get(player_no) == mPr_FOREIGNER && idx == mPr_FOREIGNER) {
        return FALSE;
    }
    return TRUE;
}

int mCD_CheckPassportFile(void) {
    /* Check if there's a passport in memory (from a previous travel) */
    if (l_mcd_foreigner_file.file.checksum != 0) {
        return 0; /* passport exists on slot 0 */
    }
    return -1; /* no passport */
}

int mCD_CheckBrokenPassportFile(int slot) {
    (void)slot;
    return 0;
}

int mCD_EraseBrokenLand_bg(int* slot) {
    if (slot) *slot = mCD_SLOT_A;
    return mCD_TRANS_ERR_NONE;
}

int mCD_EraseLand_bg(int* slot) {
    if (slot) *slot = mCD_SLOT_A;
    return mCD_TRANS_ERR_NONE;
}

int mCD_ErasePassportFile_bg(int slot) {
    (void)slot;
    /* Clear the in-memory passport */
    memset(&l_mcd_foreigner_file, 0, sizeof(l_mcd_foreigner_file));
    return mCD_TRANS_ERR_NONE;
}

int mCD_SaveErasePlayer_bg(int* slot) {
    if (slot) *slot = mCD_SLOT_A;
    return mCD_TRANS_ERR_NONE;
}

int mCD_card_format_bg(s32 chan) {
    (void)chan;
    return mCD_TRANS_ERR_NONE;
}

void mCD_PrintErrInfo(gfxprint_t* gfxprint) {
    (void)gfxprint;
}
