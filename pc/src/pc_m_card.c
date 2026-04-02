/* pc_m_card.c - memory card manager: GCI save/load, village generation, ARAM data blocks */
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
#include "pc_save_bswap.h"
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

#ifdef TARGET_VITA
#define PC_GCI_PATH     "ux0:data/AnimalCrossing/saves/DobutsunomoriP_MURA.gci"
#define PC_GCI_TMP_PATH "ux0:data/AnimalCrossing/saves/DobutsunomoriP_MURA.gci.tmp"
#define PC_SAVE_DIR     "ux0:data/AnimalCrossing/saves"
#else
#define PC_GCI_PATH     "save/DobutsunomoriP_MURA.gci"
#define PC_GCI_TMP_PATH "save/DobutsunomoriP_MURA.gci.tmp"
#define PC_SAVE_DIR     "save"
#endif
#define PC_SAVE_MAX_BACKUPS 3

#define GCI_HEADER_SIZE      sizeof(CARDDir)        /* 64 bytes */
#define GCI_FILE_DATA_SIZE   mCD_LAND_SAVE_SIZE     /* 0x72000 */
#define GCI_OTHERS_OFFSET    0
#define GCI_SAVE_MAIN_OFFSET OTHERS_SIZE            /* 0x26000 */
#define GCI_SAVE_BACK_OFFSET (OTHERS_SIZE + sizeof(Save))  /* 0x4C000 */
#define GCI_SECTOR_SIZE      mCD_MEMCARD_SECTORSIZE /* 0x2000 */

int pc_save_loaded = 0;
static int pc_save_ready = 0;

/* ARAM data blocks (mail/diary/original designs) -malloc'd instead of ARAM DMA */

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

static int pc_save_write_gci(void) {
    FILE* fp;
    u8* file_data;
    CARDDir dir_hdr;
    Save_t* save_copy;
    u16 checksum;
    u8* others_ptr;

    if (!pc_save_ready) return TRUE;

#ifdef _WIN32
    _mkdir(PC_SAVE_DIR);
#elif defined(TARGET_VITA)
    sceIoMkdir(PC_SAVE_DIR, 0777);
#else
    mkdir(PC_SAVE_DIR, 0755);
#endif

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

    /* ARAM blocks: original, mail, diary */
    {
        u32 offset = sizeof(MemcardHeader_c) + 32; /* 0x1460 */
        offset = ALIGN_NEXT(offset, 32);
        if (l_aram_block_p_table[mCD_ARAM_DATA_ORIGINAL]) {
            memcpy(others_ptr + offset, l_aram_block_p_table[mCD_ARAM_DATA_ORIGINAL],
                   l_aram_alloc_size_table[mCD_ARAM_DATA_ORIGINAL]);
            pc_save_bswap_keep_original((mCD_keep_original_c*)(others_ptr + offset), PC_BSWAP_TO_BE);
        }
        offset += l_aram_alloc_size_table[mCD_ARAM_DATA_ORIGINAL];

        offset = ALIGN_NEXT(offset, 32);
        if (l_aram_block_p_table[mCD_ARAM_DATA_MAIL]) {
            memcpy(others_ptr + offset, l_aram_block_p_table[mCD_ARAM_DATA_MAIL],
                   l_aram_alloc_size_table[mCD_ARAM_DATA_MAIL]);
            pc_save_bswap_keep_mail((mCD_keep_mail_c*)(others_ptr + offset), PC_BSWAP_TO_BE);
        }
        offset += l_aram_alloc_size_table[mCD_ARAM_DATA_MAIL];

        offset = ALIGN_NEXT(offset, 32);
        if (l_aram_block_p_table[mCD_ARAM_DATA_DIARY]) {
            memcpy(others_ptr + offset, l_aram_block_p_table[mCD_ARAM_DATA_DIARY],
                   l_aram_alloc_size_table[mCD_ARAM_DATA_DIARY]);
            pc_save_bswap_keep_diary((mCD_keep_diary_c*)(others_ptr + offset), PC_BSWAP_TO_BE);
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
    fp = fopen(PC_GCI_TMP_PATH, "wb");
    if (!fp) {
        OSReport("[PC] GCI save: failed to open temp file '%s'\n", PC_GCI_TMP_PATH);
        free(file_data);
        return FALSE;
    }

    if (fwrite(&dir_hdr, GCI_HEADER_SIZE, 1, fp) != 1 ||
        fwrite(file_data, GCI_FILE_DATA_SIZE, 1, fp) != 1) {
        OSReport("[PC] GCI save: fwrite failed (disk full?)\n");
        fclose(fp);
        remove(PC_GCI_TMP_PATH);
        free(file_data);
        return FALSE;
    }

    fflush(fp);
    fclose(fp);
    free(file_data);

    pc_save_rotate_backups(PC_GCI_PATH);
    if (rename(PC_GCI_TMP_PATH, PC_GCI_PATH) != 0) {
        OSReport("[PC] GCI save: rename '%s' → '%s' failed, recovering...\n",
                 PC_GCI_TMP_PATH, PC_GCI_PATH);
        /* restore .bak1 so the save isn't orphaned */
        {
            char bak1[300];
            snprintf(bak1, sizeof(bak1), "%s.bak1", PC_GCI_PATH);
            rename(bak1, PC_GCI_PATH);
        }
        remove(PC_GCI_TMP_PATH);
        return FALSE;
    }

    OSReport("[PC] GCI save: written successfully (backups rotated)\n");
    pc_save_loaded = 1;
    return TRUE;
}

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

    /* --- Load ARAM blocks from Others section --- */
    {
        u8* others_ptr = file_data + GCI_OTHERS_OFFSET;
        offset = sizeof(MemcardHeader_c) + 32; /* skip header + pad */

        offset = ALIGN_NEXT(offset, 32);
        if (l_aram_block_p_table[mCD_ARAM_DATA_ORIGINAL]) {
            pc_save_bswap_verify_roundtrip_original(others_ptr + offset,
                                                     l_aram_alloc_size_table[mCD_ARAM_DATA_ORIGINAL]);
            memcpy(l_aram_block_p_table[mCD_ARAM_DATA_ORIGINAL], others_ptr + offset,
                   l_aram_alloc_size_table[mCD_ARAM_DATA_ORIGINAL]);
            pc_save_bswap_keep_original((mCD_keep_original_c*)l_aram_block_p_table[mCD_ARAM_DATA_ORIGINAL],
                                        PC_BSWAP_FROM_BE);
        }
        offset += l_aram_alloc_size_table[mCD_ARAM_DATA_ORIGINAL];

        offset = ALIGN_NEXT(offset, 32);
        if (l_aram_block_p_table[mCD_ARAM_DATA_MAIL]) {
            pc_save_bswap_verify_roundtrip_mail(others_ptr + offset,
                                                 l_aram_alloc_size_table[mCD_ARAM_DATA_MAIL]);
            memcpy(l_aram_block_p_table[mCD_ARAM_DATA_MAIL], others_ptr + offset,
                   l_aram_alloc_size_table[mCD_ARAM_DATA_MAIL]);
            pc_save_bswap_keep_mail((mCD_keep_mail_c*)l_aram_block_p_table[mCD_ARAM_DATA_MAIL],
                                    PC_BSWAP_FROM_BE);
        }
        offset += l_aram_alloc_size_table[mCD_ARAM_DATA_MAIL];

        offset = ALIGN_NEXT(offset, 32);
        if (l_aram_block_p_table[mCD_ARAM_DATA_DIARY]) {
            pc_save_bswap_verify_roundtrip_diary(others_ptr + offset,
                                                  l_aram_alloc_size_table[mCD_ARAM_DATA_DIARY]);
            memcpy(l_aram_block_p_table[mCD_ARAM_DATA_DIARY], others_ptr + offset,
                   l_aram_alloc_size_table[mCD_ARAM_DATA_DIARY]);
            pc_save_bswap_keep_diary((mCD_keep_diary_c*)l_aram_block_p_table[mCD_ARAM_DATA_DIARY],
                                     PC_BSWAP_FROM_BE);
        }
    }

    free(file_data);
    return TRUE;
}

static int pc_save_scan_gci_dir(void) {
    /* Try common AC save filenames */
    static const char* gci_names[] = {
        "save/DobutsunomoriP_MURA.gci",
        "save/8P-GAFE-DobutsunomoriP_MURA.gci",
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

void mCD_init_card(void) {
    CARDInit();
}

void mCD_InitAll(void) {
    /* ARAM blocks must persist -don't null them */
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
            pc_save_ready = 1;
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

int mCD_SaveHome_bg(int param_1, int* chan) {
    int result = pc_save_write_gci();
    if (chan) *chan = mCD_SLOT_A;
    if (!result) {
        OSReport("[PC] mCD_SaveHome_bg: save failed!\n");
        return mCD_TRANS_ERR_IOERROR;
    }
    return mCD_TRANS_ERR_NONE;
}

void mCD_toNextLand(void) {}

void mCD_ReCheckLoadLand(GAME_PLAY* play) {
    (void)play;
}

int mCD_CheckStation_bg(s32* chan) {
    if (chan) *chan = mCD_SLOT_A;
    return mCD_TRANS_ERR_NONE;
}

int mCD_SaveStation_NextLand_bg(s32* chan) {
    if (chan) *chan = mCD_SLOT_A;
    return mCD_TRANS_ERR_NONE;
}

int mCD_SaveStation_Passport_bg(s32* chan) {
    if (chan) *chan = mCD_SLOT_A;
    return mCD_TRANS_ERR_NONE;
}

int mCD_GetThisLandSlotNo(void) {
    return mCD_SLOT_A;
}

int mCD_GetThisLandSlotNo_code(int* player_no, s32* slot_card_results) {
    (void)player_no;
    (void)slot_card_results;
    return mCD_SLOT_A;
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
    (void)idx;
    return TRUE;
}

int mCD_CheckPassportFile(void) {
    return -1;
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
