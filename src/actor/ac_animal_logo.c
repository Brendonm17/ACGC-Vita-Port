#include "ac_animal_logo.h"

#include "m_common_data.h"
#include "m_malloc.h"
#include "m_event.h"
#include "m_play.h"
#include "m_bgm.h"
#include "m_npc.h"
#include "libc64/qrand.h"
#include "m_name_table.h"
#include "padmgr.h"
#include "audio.h"
#include "Famicom/famicom.h"
#include "m_land.h"
#include "m_titledemo.h"
#include "m_card.h"
#include "m_rcp.h"
#include "m_cpak.h"
#include "sys_matrix.h"
#include "m_time.h"
#include "m_font.h"
#include "libultra/libultra.h"
#include "m_flashrom.h"
#ifdef PC_ENHANCEMENTS
#include "pc_settings.h"
#include "pc_controls.h"
#include "main.h"
#include "dolphin/pad.h"   // PAD_BUTTON_A etc. for the Controls page tables
#include <stdio.h>

#ifdef TARGET_VITA
// raw per-frame Vita button state, written by pc_pad.c. Read by the
// Controls page during remap capture so we see the physical press, not
// the post-mapped GC bits.
extern uint8_t g_pc_vita_pressed[PCV_COUNT];
extern void pc_controls_save(void);
#endif
#endif
#ifdef TARGET_VITA
#include "vita_shared.h"
#include "vita_banner.h"
#include "vita_texpack.h"
#include <psp2/kernel/processmgr.h>
#include <psp2/appmgr.h>
#endif

#define G_CC_TITLE PRIMITIVE, ENVIRONMENT, TEXEL0, ENVIRONMENT, PRIMITIVE, 0, TEXEL0, 0
#define G_CC_TM 0, 0, 0, PRIMITIVE, 0, 0, 0, TEXEL0
#define G_CC_BACK 0, 0, 0, PRIMITIVE, TEXEL0, 0, PRIMITIVE, 0
#define G_CC_PRESS_START PRIMITIVE, ENVIRONMENT, TEXEL0, ENVIRONMENT, PRIMITIVE, 0, TEXEL0, 0

#define TITLE_WIDTH 64
#define TITLE_HEIGHT 16

#if VERSION == VER_GAFE01_00
#define aAL_IN_FRAMES 121.0f
#elif VERSION == VER_GAFU01_00
#define aAL_IN_FRAMES 101.0f
#endif

extern u8 log_win_nintendo1_tex[];
extern u8 log_win_nintendo2_tex[];
extern u8 log_win_nintendo3_tex[];

extern Gfx logo_us_tm_model[];

extern Gfx logo_us_backA_model[];
extern Gfx logo_us_backB_model[];
extern Gfx logo_us_backC_model[];
extern Gfx logo_us_backD_model[];

extern u8 log_win_logo3_tex[];
extern u8 log_win_logo4_tex[];

extern cKF_Skeleton_R_c cKF_bs_r_logo_us_animal;
extern cKF_Skeleton_R_c cKF_bs_r_logo_us_cros;
extern cKF_Skeleton_R_c cKF_bs_r_logo_us_sing;

static void aAL_actor_ct(ACTOR* actor, GAME* game);
static void aAL_actor_dt(ACTOR* actor, GAME* game);
static void aAL_actor_move(ACTOR* actor, GAME* game);
static void aAL_actor_draw(ACTOR* actor, GAME* game);

#ifdef PC_ENHANCEMENTS
// mirrors actor->pc_options_open so title_demo_move can pause the demo
// frame counter while the options menu is up (otherwise the demo would
// transition out and destroy the actor)
int g_aAL_options_menu_open = 0;
#endif

ACTOR_PROFILE Animal_Logo_Profile = {
  mAc_PROFILE_ANIMAL_LOGO,
  ACTOR_PART_BG,
  ACTOR_STATE_NO_MOVE_WHILE_CULLED | ACTOR_STATE_NO_DRAW_WHILE_CULLED,
  EMPTY_NO,
  ACTOR_OBJ_BANK_KEEP,
  sizeof(ANIMAL_LOGO_ACTOR),
  &aAL_actor_ct,
  &aAL_actor_dt,
  &aAL_actor_move,
  &aAL_actor_draw,
  NULL
};

#include "../src/actor/ac_animal_logo_misc.c"

static void aAL_setupAction(ANIMAL_LOGO_ACTOR* actor, GAME* game, int action);
static void aAL_title_decide_p_sel_npc();

static void aAL_actor_ct(ACTOR* actor, GAME* game) {
  ANIMAL_LOGO_ACTOR* logo_actor = (ANIMAL_LOGO_ACTOR*)actor;
  GAME_PLAY* play = (GAME_PLAY*)game;
  Clip_c* clip = Common_GetPointer(clip);
  aAL_SkeletonInfo_c* skeleton_info;

#ifdef TARGET_PC
  { extern int g_pc_verbose; if (g_pc_verbose) printf("[LOGO] aAL_actor_ct: Animal Logo actor created\n"); }
#else
  printf("[LOGO] aAL_actor_ct: Animal Logo actor created\n");
#endif

  if (clip->animal_logo_clip == NULL) {
    clip->animal_logo_clip = (aAL_Clip_c*)zelda_malloc(sizeof(aAL_Clip_c));
    clip->animal_logo_clip->data_init_proc = &title_action_data_init_start_select;
  }

  skeleton_info = &logo_actor->animal;
  skeleton_info->work_area_p = logo_actor->animal_work_area;
  skeleton_info->morph_area_p = logo_actor->animal_morph_area;
  cKF_SkeletonInfo_R_ct(&skeleton_info->skeleton, &cKF_bs_r_logo_us_animal, NULL, skeleton_info->work_area_p, skeleton_info->morph_area_p);

  skeleton_info = &logo_actor->cros;
  skeleton_info->work_area_p = logo_actor->cros_work_area;
  skeleton_info->morph_area_p = logo_actor->cros_morph_area;
  cKF_SkeletonInfo_R_ct(&skeleton_info->skeleton, &cKF_bs_r_logo_us_cros, NULL, skeleton_info->work_area_p, skeleton_info->morph_area_p);


  skeleton_info = &logo_actor->sing;
  skeleton_info->work_area_p = logo_actor->sing_work_area;
  skeleton_info->morph_area_p = logo_actor->sing_morph_area;
  cKF_SkeletonInfo_R_ct(&skeleton_info->skeleton, &cKF_bs_r_logo_us_sing, NULL, skeleton_info->work_area_p, skeleton_info->morph_area_p);

  aAL_setupAction(logo_actor, (GAME*)play, aAL_ACTION_IN);
}

static void aAL_actor_dt(ACTOR* actor, GAME* game) {
  ANIMAL_LOGO_ACTOR* logo_actor = (ANIMAL_LOGO_ACTOR*)actor;

#ifdef PC_ENHANCEMENTS
  // defensive: clear in case the actor died with the menu still up
  g_aAL_options_menu_open = 0;
#endif

  if (Common_Get(clip.animal_logo_clip) != NULL) {
    zelda_free(Common_Get(clip.animal_logo_clip));
    Common_Set(clip.animal_logo_clip, NULL);
  }

  if (mEv_CheckTitleDemo() != mEv_TITLEDEMO_LOGO) {
    mEv_SetTitleDemo(mEv_TITLEDEMO_NONE);
  }

  cKF_SkeletonInfo_R_dt(&logo_actor->animal.skeleton);
  cKF_SkeletonInfo_R_dt(&logo_actor->cros.skeleton);
  cKF_SkeletonInfo_R_dt(&logo_actor->sing.skeleton);

  if (mFRm_CheckSaveData() == TRUE) {
    aAL_title_decide_p_sel_npc();
  }
}

static void aAL_title_game_data_init_start_select(ANIMAL_LOGO_ACTOR* actor, GAME* game) {
  GAME_PLAY* play = (GAME_PLAY*)game;

#ifdef TARGET_PC
  /* Reload save from disk; the title demo mutated save data in RAM.
   * On GC the memory card is re-read; on PC re-read the GCI file. */
  if (pc_save_loaded) {
    pc_save_reload();
  }
#endif

  play->fb_fade_type = FADE_TYPE_SELECT;
  play->fb_wipe_type = WIPE_TYPE_FADE_BLACK;
  Common_Set(transition.wipe_type, WIPE_TYPE_FADE_BLACK);
  mBGMPsComp_make_ps_wipe(0x1168);
}

static void aAL_title_decide_p_sel_npc() {
  int selected;
  mActor_name_t npc_name;
  int idx;

  while (TRUE) {
    selected = (int)(fqrand() * (f32)ANIMAL_NUM_MAX);
    if (mNpc_CheckFreeAnimalPersonalID(Save_GetPointer(animals[selected].id)) == FALSE) {
      npc_name = Save_Get(animals[selected].id.npc_id);
      break;
    }
  }
  
  idx = mNpc_SearchAnimalinfo(Save_Get(animals), npc_name, ANIMAL_NUM_MAX);
  mNpc_RegistEventNpc(SP_NPC_P_SEL2, npc_name, npc_name, Save_Get(animals[idx].cloth));
}

static int aAL_wipe_end_check(GAME* game) {
  GAME_PLAY* play = (GAME_PLAY*)game;
  int res = FALSE;
  fbdemo_wipe* wipe = &play->fbdemo_wipe;

  if ((*wipe->wipe_procs.isfinished_proc)(&wipe->wipe_data)) {
    res = TRUE;
  }

  return res;
}

static int aAL_chk_start_key() {
  int res = FALSE;

  if (padmgr_isConnectedController(PAD0) && ((gamePT->pads[PAD0].on.button & BUTTON_START) == BUTTON_START || (gamePT->pads[PAD0].on.button & BUTTON_A) == BUTTON_A)) {
    res = TRUE;
  }

  return res;
}

static int aAL_chk_start_key2(ANIMAL_LOGO_ACTOR* actor, GAME* game) {
  int res = FALSE;

  if (aAL_chk_start_key() == TRUE) {
    aAL_setupAction(actor, game, aAL_ACTION_START_KEY_CHK_START);
    res = TRUE;
  }

  return res;
}

static void aAL_logo_in(ANIMAL_LOGO_ACTOR* actor, GAME* game) {
  int animal_done;
  int cros_done;
  int sing_done;
    
  if (aAL_chk_start_key2(actor, game) == FALSE) {
    animal_done = cKF_SkeletonInfo_R_play(&actor->animal.skeleton);
    cros_done = cKF_SkeletonInfo_R_play(&actor->cros.skeleton);
    sing_done = cKF_SkeletonInfo_R_play(&actor->sing.skeleton);
    if (animal_done == TRUE && cros_done == TRUE && sing_done == TRUE) {
      aAL_setupAction(actor, game, aAL_ACTION_BACK_FADE_IN);
    }
  }
}

static void aAL_back_fadein(ANIMAL_LOGO_ACTOR* actor, GAME* game) {
  if (aAL_chk_start_key2(actor, game) == FALSE) {
    s16 opacity = actor->back_opacity;
    opacity += aAL_BACK_FADEIN_RATE;
    
    if (opacity > aAL_BACK_FADEIN_MAX) {
      opacity = aAL_BACK_FADEIN_MAX;
      aAL_setupAction(actor, game, aAL_ACTION_START_KEY_CHK_START);
    }

    actor->back_opacity = opacity;
  }
}

static void aAL_start_key_chk_start_wait(ANIMAL_LOGO_ACTOR* actor, GAME* game) {
  if (padmgr_isConnectedController(PAD0) && actor->title_timer <= 0 && famicom_mount_archive_end_check()) {
    aAL_setupAction(actor, game, aAL_ACTION_GAME_START);
  }
}

static void aAL_game_start_wait(ANIMAL_LOGO_ACTOR* actor, GAME* game) {
  GAME_PLAY* play = (GAME_PLAY*)game;
  f32 start_opacity;
  s16 t;
  s16 new_opacity_timer;

  new_opacity_timer = actor->start_opacity_timer;
  new_opacity_timer += (s16)(32768.0f / (actor->start_opacity_timer > 0 ? 50.0f : 22.0f));
  start_opacity = 127.5f * sin_s(new_opacity_timer) + 127.5f; // 127.5f + 127.5f * [0, 1] = [127.5f, 255.0f] (opacity)

  if (start_opacity > 255.0f) {
    start_opacity = 255.0f;
  }
  else if (start_opacity < 0.0f) {
    start_opacity = 0.0f;
  }

  actor->press_start_opacity = start_opacity;
  actor->start_opacity_timer = new_opacity_timer;

  if (play->fb_fade_type == FADE_TYPE_SELECT_END) {
    aAL_setupAction(actor, game, aAL_ACTION_6);
  }
  else if (
     ((gamePT->pads[PAD0].on.button & BUTTON_START) == BUTTON_START || (gamePT->pads[PAD0].on.button & BUTTON_A) == BUTTON_A) &&
     mLd_CheckStartFlag() == TRUE &&
     aAL_wipe_end_check(game) == TRUE &&
     mTD_tdemo_button_ok_check()
  ) {
    aAL_setupAction(actor, game, aAL_ACTION_FADE_OUT_START);
  }
}

static void aAL_fade_out_start_wait(ANIMAL_LOGO_ACTOR* actor, GAME* game) {
  if (actor->title_timer <= 0) {
    aAL_title_game_data_init_start_select(actor, game);
    aAL_setupAction(actor, game, aAL_ACTION_OUT);
  }
}

extern cKF_Animation_R_c cKF_ba_r_logo_us_animal;
extern cKF_Animation_R_c cKF_ba_r_logo_us_cros;
extern cKF_Animation_R_c cKF_ba_r_logo_us_sing;

static void aAL_logo_in_init(ANIMAL_LOGO_ACTOR* actor, GAME* game) {
  cKF_SkeletonInfo_R_init(&actor->animal.skeleton, actor->animal.skeleton.skeleton, &cKF_ba_r_logo_us_animal, 1.0f, aAL_IN_FRAMES, 1.0f, 0.5f, 0.0f, cKF_FRAMECONTROL_STOP, NULL);
  cKF_SkeletonInfo_R_init(&actor->cros.skeleton, actor->cros.skeleton.skeleton, &cKF_ba_r_logo_us_cros, 1.0f, aAL_IN_FRAMES, 1.0f, 0.5f, 0.0f, cKF_FRAMECONTROL_STOP, NULL);
  cKF_SkeletonInfo_R_init(&actor->sing.skeleton, actor->sing.skeleton.skeleton, &cKF_ba_r_logo_us_sing, 1.0f, aAL_IN_FRAMES, 1.0f, 0.5f, 0.0f, cKF_FRAMECONTROL_STOP, NULL);

  actor->copyright_opacity = 0;
  actor->titledemo_no = mTD_get_titledemo_no();

  mCD_set_aram_save_data();
  lbRTC_GetTime(Common_GetPointer(time.rtc_time));
  Common_Set(player_no, 0);
  Common_Set(player_data_mode, 0);
  Common_Set(scene_from_title_demo, -1);
}

static void aAL_back_fadein_init(ANIMAL_LOGO_ACTOR* actor, GAME* game) {
  actor->back_opacity = 0;
}

static void aAL_start_key_chk_start_wait_init(ANIMAL_LOGO_ACTOR* actor, GAME* game) {
  /* move all animations to the final frame (in case animation was skipped) */
  actor->animal.skeleton.frame_control.current_frame = actor->animal.skeleton.frame_control.end_frame;
  actor->cros.skeleton.frame_control.current_frame = actor->cros.skeleton.frame_control.end_frame;
  actor->sing.skeleton.frame_control.current_frame = actor->sing.skeleton.frame_control.end_frame;

  cKF_SkeletonInfo_R_play(&actor->animal.skeleton);
  cKF_SkeletonInfo_R_play(&actor->cros.skeleton);
  cKF_SkeletonInfo_R_play(&actor->sing.skeleton);

  actor->copyright_opacity = 255;
  actor->back_opacity = aAL_BACK_FADEIN_MAX;
  actor->title_timer = aAL_TIMER;
}

static void aAL_fade_out_start_wait_init(ANIMAL_LOGO_ACTOR* actor, GAME* game) {
  sAdo_SysTrgStart(0x44D);
  actor->title_timer = aAL_FADEOUT_TIMER;
  actor->press_start_opacity = 255.0f;
}

#ifdef PC_ENHANCEMENTS
// snapshot of settings as of menu open. used to mark unsaved fields
// with an asterisk in the draw routine. updated on save/close, replaced
// on re-open (revert via B reloads from disk so baseline becomes stale).
static PCSettings g_pc_options_baseline;
#ifdef TARGET_VITA
// Snapshot of g_pc_controls at menu open; restored on B-cancel.
static PCControls g_pc_controls_baseline;

// Delete Town modal: 0=hidden, 1=No selected, 2=Yes selected.
static int s_pc_confirm_delete = 0;

// Re-exec ourselves so settings/controls changes take effect without the
// user manually relaunching from LiveArea.
static void aAL_pc_restart_app(void) {
    sceAppMgrLoadExec("app0:eboot.bin", NULL, NULL);
    sceKernelExitProcess(0); // fallback if LoadExec ever returns
}

typedef enum {
    PC_PAGE_GRAPHICS = 0,
    PC_PAGE_GAMEPLAY,
    PC_PAGE_SAVE,
    PC_PAGE_CONTROLS,
    PC_PAGE_COUNT
} PCOptionsPage;

// Controls page rows. HEADER exists but isn't used in the current tables
// (kept so navigation can still skip headers if they come back).
typedef enum {
    CR_HEADER = 0,
    CR_MAIN_BTN,    // -> main_map
    CR_GC_AXIS,     // -> main_axis_map (push a Vita stick to bind)
    CR_NES_BTN,     // -> nes_map
    CR_NES_TURBO,   // -> nes_turbo_map
} PCControlsRowKind;

typedef struct {
    int8_t      kind;
    uint16_t    bit;   // MAIN: GC bit / GC_AXIS: PCG_AXIS_* / NES_*: NES bit
    const char* label;
} PCControlsRow;

typedef enum {
    PC_SUBPAGE_GAME = 0,
    PC_SUBPAGE_NES,
    PC_SUBPAGE_COUNT
} PCControlsSubPage;

static const PCControlsRow s_controls_game_rows[] = {
    { CR_MAIN_BTN, PAD_BUTTON_A,       "A (Confirm)" },
    { CR_MAIN_BTN, PAD_BUTTON_B,       "B (Cancel)" },
    { CR_MAIN_BTN, PAD_BUTTON_X,       "X" },
    { CR_MAIN_BTN, PAD_BUTTON_Y,       "Y" },
    { CR_MAIN_BTN, PAD_BUTTON_START,   "Start" },
    { CR_MAIN_BTN, PAD_TRIGGER_Z,      "Z (Inventory)" },
    { CR_MAIN_BTN, PAD_TRIGGER_L,      "L (Camera)" },
    { CR_MAIN_BTN, PAD_TRIGGER_R,      "R (Camera)" },
    { CR_MAIN_BTN, PAD_BUTTON_UP,      "D-pad Up" },
    { CR_MAIN_BTN, PAD_BUTTON_DOWN,    "D-pad Down" },
    { CR_MAIN_BTN, PAD_BUTTON_LEFT,    "D-pad Left" },
    { CR_MAIN_BTN, PAD_BUTTON_RIGHT,   "D-pad Right" },
    { CR_GC_AXIS,  PCG_AXIS_MAIN_X,    "Main X" },
    { CR_GC_AXIS,  PCG_AXIS_MAIN_Y,    "Main Y" },
    { CR_GC_AXIS,  PCG_AXIS_CSTICK_X,  "C-stick X" },
    { CR_GC_AXIS,  PCG_AXIS_CSTICK_Y,  "C-stick Y" },
};
static const PCControlsRow s_controls_nes_rows[] = {
    { CR_NES_BTN,   NES_BIT_A,      "A" },
    { CR_NES_BTN,   NES_BIT_B,      "B" },
    { CR_NES_BTN,   NES_BIT_SELECT, "Select" },
    { CR_NES_BTN,   NES_BIT_START,  "Start" },
    { CR_NES_BTN,   NES_BIT_UP,     "Up" },
    { CR_NES_BTN,   NES_BIT_DOWN,   "Down" },
    { CR_NES_BTN,   NES_BIT_LEFT,   "Left" },
    { CR_NES_BTN,   NES_BIT_RIGHT,  "Right" },
    { CR_NES_TURBO, NES_TURBO_A,    "Turbo A" },
    { CR_NES_TURBO, NES_TURBO_B,    "Turbo B" },
};

#define N_GAME_ROWS ((int)(sizeof(s_controls_game_rows) / sizeof(s_controls_game_rows[0])))
#define N_NES_ROWS  ((int)(sizeof(s_controls_nes_rows)  / sizeof(s_controls_nes_rows[0])))
// 9 rows fit between the sub-tab bar (~y=79) and the footer (y=208) with
// room for the scroll indicators above and below.
#define PC_CONTROLS_VISIBLE 9

static int s_controls_subpage = PC_SUBPAGE_GAME;

static const PCControlsRow* aAL_pc_controls_rows(int* out_count) {
    if (s_controls_subpage == PC_SUBPAGE_NES) {
        if (out_count) *out_count = N_NES_ROWS;
        return s_controls_nes_rows;
    }
    if (out_count) *out_count = N_GAME_ROWS;
    return s_controls_game_rows;
}

// PCV_LSTICK_UP etc. -> PCA_LSTICK_Y etc. -1 for non-stick buttons.
// Used by the GC_AXIS capture so pushing a stick direction binds the
// corresponding analog axis.
static int aAL_pcv_to_pca(int pcv) {
    switch (pcv) {
        case PCV_LSTICK_UP:    case PCV_LSTICK_DOWN:  return PCA_LSTICK_Y;
        case PCV_LSTICK_LEFT:  case PCV_LSTICK_RIGHT: return PCA_LSTICK_X;
        case PCV_RSTICK_UP:    case PCV_RSTICK_DOWN:  return PCA_RSTICK_Y;
        case PCV_RSTICK_LEFT:  case PCV_RSTICK_RIGHT: return PCA_RSTICK_X;
        default: return -1;
    }
}

// Walk over CR_HEADER rows in `dir` so the cursor never lands on one.
static int aAL_pc_skip_headers(int sel, int dir) {
    int total = 0;
    const PCControlsRow* rows = aAL_pc_controls_rows(&total);
    int p = sel;
    while (p >= 0 && p < total && rows[p].kind == CR_HEADER) p += dir;
    return p;
}
static int aAL_pc_first_selectable(int page) {
    if (page != PC_PAGE_CONTROLS) return 0;
    return aAL_pc_skip_headers(0, +1);
}

// per-page item count. Graphics gains/loses the Banner row based on aspect.
static int aAL_pc_page_item_count(int page) {
    if (page == PC_PAGE_GRAPHICS) return g_pc_settings.aspect_mode ? 7 : 6; // Res, Aspect, [Banner], MSAA, Tex Pack, NES Aspect, BootLogo
    if (page == PC_PAGE_GAMEPLAY) return 4; // Resetti, TimeSync, FreeCam, TextSpeed
    if (page == PC_PAGE_SAVE)     return 3; // SaveSlot, AutoSave, DeleteTown
    if (page == PC_PAGE_CONTROLS) {
        int count = 0;
        aAL_pc_controls_rows(&count);
        return count;
    }
    return 0;
}

// Remap capture window. Stays open for the full timeout so the user can
// press multiple Vita buttons; at commit the binding is REPLACED with
// the captured set (empty set -> "none").
static int s_remap_active = 0;
static int s_remap_apply_kind = 0;       // CR_MAIN_BTN/CR_GC_AXIS/CR_NES_BTN/CR_NES_TURBO
static int s_remap_bit = 0;              // bit/axis to assign in the target map
static int s_remap_timeout = 0;          // frames left until commit
static uint8_t s_remap_prev[PCV_COUNT];  // last frame's pressed state for edge detect
static unsigned int s_remap_captured;    // bit i = PCV i captured
#endif

static void aAL_pc_game_start_wait(ANIMAL_LOGO_ACTOR* actor, GAME* game) {
  GAME_PLAY* play = (GAME_PLAY*)game;
  u16 on_btn = gamePT->pads[PAD0].on.button;
  s8 stick_y = gamePT->pads[PAD0].now.stick_y;

  // no blinking in menu
  actor->press_start_opacity = 255.0f;

  if (play->fb_fade_type == FADE_TYPE_SELECT_END) {
    aAL_setupAction(actor, game, aAL_ACTION_6);
    return;
  }

  // debounce
  if (actor->pc_cursor_cooldown > 0) {
    actor->pc_cursor_cooldown--;
  }

  if (actor->pc_options_open) {
    s8 stick_x = gamePT->pads[PAD0].now.stick_x;

#ifdef TARGET_VITA
    // Remap capture has to intercept every press; otherwise the button
    // the user is binding would also fire the menu action under it.
    if (s_remap_active) {
      for (int i = 0; i < PCV_COUNT; i++) {
        if (g_pc_vita_pressed[i] && !s_remap_prev[i]) {
          s_remap_captured |= (1u << i);
        }
      }
      memcpy(s_remap_prev, g_pc_vita_pressed, sizeof(s_remap_prev));

      if (--s_remap_timeout <= 0) {
        // Clear the bit from all Vita slots, then OR it onto each captured
        // slot. Target map depends on the row kind.
        if (s_remap_apply_kind == CR_NES_BTN) {
          for (int j = 0; j < PCV_COUNT; j++) g_pc_controls.nes_map[j] &= ~(uint8_t)s_remap_bit;
          for (int i = 0; i < PCV_COUNT; i++) {
            if (s_remap_captured & (1u << i)) g_pc_controls.nes_map[i] |= (uint8_t)s_remap_bit;
          }
        } else if (s_remap_apply_kind == CR_NES_TURBO) {
          for (int j = 0; j < PCV_COUNT; j++) g_pc_controls.nes_turbo_map[j] &= ~(uint8_t)s_remap_bit;
          for (int i = 0; i < PCV_COUNT; i++) {
            if (s_remap_captured & (1u << i)) g_pc_controls.nes_turbo_map[i] |= (uint8_t)s_remap_bit;
          }
        } else if (s_remap_apply_kind == CR_GC_AXIS) {
          // Stick-direction presses translate to PCA axes. invert resets
          // to +1; for negative-direction bindings edit controls.ini.
          uint8_t captured_pca = 0;
          for (int i = 0; i < PCV_COUNT; i++) {
            if (!(s_remap_captured & (1u << i))) continue;
            int p = aAL_pcv_to_pca(i);
            if (p >= 0) captured_pca |= (1u << p);
          }
          for (int p = 0; p < PCA_COUNT; p++) {
            if (g_pc_controls.main_axis_map[p].target == (PCGCAxis)s_remap_bit) {
              g_pc_controls.main_axis_map[p].target = PCG_AXIS_NONE;
              g_pc_controls.main_axis_map[p].invert = +1;
            }
          }
          for (int p = 0; p < PCA_COUNT; p++) {
            if (captured_pca & (1u << p)) {
              g_pc_controls.main_axis_map[p].target = (PCGCAxis)s_remap_bit;
              g_pc_controls.main_axis_map[p].invert = +1;
            }
          }
        } else {
          // CR_MAIN_BTN
          for (int j = 0; j < PCV_COUNT; j++) g_pc_controls.main_map[j] &= ~(uint16_t)s_remap_bit;
          for (int i = 0; i < PCV_COUNT; i++) {
            if (s_remap_captured & (1u << i)) g_pc_controls.main_map[i] |= (uint16_t)s_remap_bit;
          }
          pc_controls_enforce_confirm_cancel_distinct((uint16_t)s_remap_bit);
        }
        s_remap_active = 0;
        actor->pc_cursor_cooldown = 14;
      }
      return;
    }
#endif

    // Vita restarts the process on save so the new settings take effect
    // without requiring in-place re-init of render scale, MSAA, etc.
    if (on_btn & BUTTON_START) {
      pc_settings_save();
#ifdef TARGET_VITA
      pc_controls_save();
      aAL_pc_restart_app();
#else
      pc_settings_apply();
      g_pc_options_baseline = g_pc_settings;
      actor->pc_options_open = 0;
      g_aAL_options_menu_open = 0;
      actor->pc_cursor_cooldown = 10;
#endif
      return;
    }

    // B reverts to the open-time snapshot. Nothing was written to disk
    // before this point, so no reload needed.
    if (on_btn & BUTTON_B) {
      g_pc_settings = g_pc_options_baseline;
#ifdef TARGET_VITA
      g_pc_controls = g_pc_controls_baseline;
#endif
      actor->pc_options_open = 0;
      g_aAL_options_menu_open = 0;
      actor->pc_cursor_cooldown = 10;
      return;
    }

#ifdef TARGET_VITA
    // Delete Town modal: L/R picks No/Yes, A commits, B cancels.
    if (s_pc_confirm_delete) {
      if (actor->pc_cursor_cooldown == 0) {
        s8 stick_x = gamePT->pads[PAD0].now.stick_x;
        if (stick_x > 30 || stick_x < -30 || (on_btn & (BUTTON_DLEFT | BUTTON_DRIGHT))) {
          s_pc_confirm_delete = (s_pc_confirm_delete == 1) ? 2 : 1;
          actor->pc_cursor_cooldown = 8;
        }
      }
      if (on_btn & BUTTON_A) {
        if (s_pc_confirm_delete == 2) {
          pc_save_delete_current_town();
          aAL_pc_restart_app();
        }
        s_pc_confirm_delete = 0;
        actor->pc_cursor_cooldown = 10;
      } else if (on_btn & BUTTON_B) {
        s_pc_confirm_delete = 0;
        actor->pc_cursor_cooldown = 10;
      }
      return;
    }

    // focus levels: 0=list, 1=main tab bar, 2=sub tab bar (Controls only)
    {
      int page = actor->pc_options_page;
      int item_count = aAL_pc_page_item_count(page);
      if (item_count <= 0) item_count = 1;
      if (actor->pc_options_sel >= item_count) actor->pc_options_sel = item_count - 1;
      if (actor->pc_options_sel < 0) actor->pc_options_sel = 0;

      s8 stick_x = gamePT->pads[PAD0].now.stick_x;
      int has_subtab = (page == PC_PAGE_CONTROLS);

      if (actor->pc_options_focus == 1) {
        if (actor->pc_cursor_cooldown == 0) {
          if (stick_y < -30 || (on_btn & BUTTON_DDOWN)) {
            // drop into sub tab on Controls, else into the list
            actor->pc_options_focus = has_subtab ? 2 : 0;
            if (!has_subtab) actor->pc_options_sel = aAL_pc_first_selectable(actor->pc_options_page);
            actor->pc_cursor_cooldown = 10;
          } else if (stick_x < -30 || (on_btn & BUTTON_DLEFT)) {
            if (actor->pc_options_page > 0) actor->pc_options_page--;
            else actor->pc_options_page = PC_PAGE_COUNT - 1;
            actor->pc_options_sel = aAL_pc_first_selectable(actor->pc_options_page);
            actor->pc_cursor_cooldown = 10;
          } else if (stick_x > 30 || (on_btn & BUTTON_DRIGHT)) {
            actor->pc_options_page = (actor->pc_options_page + 1) % PC_PAGE_COUNT;
            actor->pc_options_sel = aAL_pc_first_selectable(actor->pc_options_page);
            actor->pc_cursor_cooldown = 10;
          }
        }
        if (on_btn & BUTTON_A) {
          actor->pc_options_focus = has_subtab ? 2 : 0;
          if (!has_subtab) actor->pc_options_sel = aAL_pc_first_selectable(actor->pc_options_page);
          actor->pc_cursor_cooldown = 10;
        }
        return;
      }

      if (actor->pc_options_focus == 2) {
        if (actor->pc_cursor_cooldown == 0) {
          if (stick_y < -30 || (on_btn & BUTTON_DDOWN)) {
            actor->pc_options_focus = 0;
            actor->pc_options_sel = aAL_pc_first_selectable(actor->pc_options_page);
            actor->pc_cursor_cooldown = 10;
          } else if (stick_y > 30 || (on_btn & BUTTON_DUP)) {
            actor->pc_options_focus = 1;
            actor->pc_cursor_cooldown = 10;
          } else if (stick_x < -30 || (on_btn & BUTTON_DLEFT)) {
            s_controls_subpage = (s_controls_subpage + PC_SUBPAGE_COUNT - 1) % PC_SUBPAGE_COUNT;
            actor->pc_options_sel = aAL_pc_first_selectable(actor->pc_options_page);
            actor->pc_cursor_cooldown = 10;
          } else if (stick_x > 30 || (on_btn & BUTTON_DRIGHT)) {
            s_controls_subpage = (s_controls_subpage + 1) % PC_SUBPAGE_COUNT;
            actor->pc_options_sel = aAL_pc_first_selectable(actor->pc_options_page);
            actor->pc_cursor_cooldown = 10;
          }
        }
        if (on_btn & BUTTON_A) {
          actor->pc_options_focus = 0;
          actor->pc_options_sel = aAL_pc_first_selectable(actor->pc_options_page);
          actor->pc_cursor_cooldown = 10;
        }
        return;
      }

      // list. Controls page skips header rows so sel never lands on one.
      if (actor->pc_cursor_cooldown == 0) {
        if (stick_y > 30 || (on_btn & BUTTON_DUP)) {
          int next_sel = actor->pc_options_sel - 1;
          if (page == PC_PAGE_CONTROLS) next_sel = aAL_pc_skip_headers(next_sel, -1);
          if (next_sel >= 0) {
            actor->pc_options_sel = next_sel;
            actor->pc_cursor_cooldown = 8;
          } else {
            // off the top: jump to nearest tab bar
            actor->pc_options_focus = has_subtab ? 2 : 1;
            actor->pc_cursor_cooldown = 10;
          }
        } else if (stick_y < -30 || (on_btn & BUTTON_DDOWN)) {
          int next_sel = actor->pc_options_sel + 1;
          if (page == PC_PAGE_CONTROLS) next_sel = aAL_pc_skip_headers(next_sel, +1);
          if (next_sel < item_count) {
            actor->pc_options_sel = next_sel;
            actor->pc_cursor_cooldown = 8;
          }
        }
      }

      int do_right = (stick_x > 30 || (on_btn & BUTTON_DRIGHT));
      int do_left  = !do_right && (stick_x < -30 || (on_btn & BUTTON_DLEFT));
      int do_a     = (on_btn & BUTTON_A) ? 1 : 0;

      if ((do_right || do_left || do_a) && actor->pc_cursor_cooldown == 0) {
        actor->pc_cursor_cooldown = 8;
        int s = actor->pc_options_sel;

        if (page == PC_PAGE_GRAPHICS) {
          int has_banner = g_pc_settings.aspect_mode;
          int banner_idx = has_banner ? 2 : -1;
          int msaa_idx = has_banner ? 3 : 2;
          int texpack_idx = msaa_idx + 1;
          int nesasp_idx = texpack_idx + 1;
          int bootlogo_idx = nesasp_idx + 1;

          if (s == 0) { // Resolution
            if (do_right) {
              if (g_pc_settings.render_scale == 50) g_pc_settings.render_scale = 75;
              else if (g_pc_settings.render_scale == 75) g_pc_settings.render_scale = 100;
            } else if (do_left) {
              if (g_pc_settings.render_scale == 100) g_pc_settings.render_scale = 75;
              else if (g_pc_settings.render_scale == 75) g_pc_settings.render_scale = 50;
            }
          } else if (s == 1) { // Aspect
            if (do_right || do_left || do_a) {
              g_pc_settings.aspect_mode = !g_pc_settings.aspect_mode;
              // banner list was scanned on menu open; no need to re-scan
              // (it'd PNG-decode every banner via stb_image again).
            }
          } else if (s == banner_idx) { // Banner
            if (g_banner_count) {
              int cur = banner_find_index(g_pc_settings.banner_name);
              if (do_right) {
                if (cur + 1 >= g_banner_count) g_pc_settings.banner_name[0] = '\0';
                else strcpy(g_pc_settings.banner_name, g_banner_list[cur + 1].name);
              } else if (do_left) {
                if (cur <= 0) {
                  if (cur == 0) g_pc_settings.banner_name[0] = '\0';
                  else strcpy(g_pc_settings.banner_name, g_banner_list[g_banner_count - 1].name);
                } else {
                  strcpy(g_pc_settings.banner_name, g_banner_list[cur - 1].name);
                }
              }
            }
          } else if (s == msaa_idx) { // MSAA
            if (do_right) {
              if (g_pc_settings.msaa == 0) g_pc_settings.msaa = 2;
              else if (g_pc_settings.msaa == 2) g_pc_settings.msaa = 4;
            } else if (do_left) {
              if (g_pc_settings.msaa == 4) g_pc_settings.msaa = 2;
              else if (g_pc_settings.msaa == 2) g_pc_settings.msaa = 0;
            }
          } else if (s == texpack_idx) { // Texture Pack
            if (g_texpack_count > 0) {
              int cur = texpack_find_index(g_pc_settings.texture_pack);
              if (do_right) {
                if (cur + 1 >= g_texpack_count) g_pc_settings.texture_pack[0] = '\0';
                else strcpy(g_pc_settings.texture_pack, g_texpack_list[cur + 1].name);
              } else if (do_left) {
                if (cur <= 0) {
                  if (cur == 0) g_pc_settings.texture_pack[0] = '\0';
                  else strcpy(g_pc_settings.texture_pack, g_texpack_list[g_texpack_count - 1].name);
                } else {
                  strcpy(g_pc_settings.texture_pack, g_texpack_list[cur - 1].name);
                }
              }
            }
          } else if (s == nesasp_idx) { // NES Aspect
            if (do_right || do_left || do_a) g_pc_settings.nes_aspect = !g_pc_settings.nes_aspect;
          } else if (s == bootlogo_idx) { // Boot Logo
            if (do_right || do_left || do_a) g_pc_settings.boot_logo = !g_pc_settings.boot_logo;
          }
        } else if (page == PC_PAGE_GAMEPLAY) {
          if (s == 0) { // Disable Resetti
            if (do_right || do_left || do_a) g_pc_settings.disable_resetti = !g_pc_settings.disable_resetti;
          } else if (s == 1) { // Time Sync
            if (do_right || do_left || do_a) g_pc_settings.time_sync = !g_pc_settings.time_sync;
          } else if (s == 2) { // Free Cam
            if (do_right || do_left || do_a) g_pc_settings.free_cam = !g_pc_settings.free_cam;
          } else if (s == 3) { // Text Speed
            if (do_right) {
              if (g_pc_settings.text_speed < 2) g_pc_settings.text_speed++;
            } else if (do_left) {
              if (g_pc_settings.text_speed > 0) g_pc_settings.text_speed--;
            }
          }
        } else if (page == PC_PAGE_SAVE) {
          if (s == 0) { // Save Slot
            if (do_right || do_left || do_a) g_pc_settings.save_slot = !g_pc_settings.save_slot;
          } else if (s == 1) { // Auto Save
            if (do_right || do_left || do_a) g_pc_settings.auto_save = !g_pc_settings.auto_save;
          } else if (s == 2) { // Delete Town
            if (do_a) {
              s_pc_confirm_delete = 1; // No selected by default
              actor->pc_cursor_cooldown = 12;
            }
          }
        } else if (page == PC_PAGE_CONTROLS) {
          int row_count = 0;
          const PCControlsRow* rows = aAL_pc_controls_rows(&row_count);
          if (s >= 0 && s < row_count && do_a) {
            const PCControlsRow* row = &rows[s];
            if (row->kind == CR_MAIN_BTN || row->kind == CR_NES_BTN ||
                row->kind == CR_NES_TURBO || row->kind == CR_GC_AXIS) {
              s_remap_active = 1;
              s_remap_apply_kind = row->kind;
              s_remap_bit = row->bit;
              s_remap_timeout = 300; // 5 sec @ 60fps
              s_remap_captured = 0;
              memcpy(s_remap_prev, g_pc_vita_pressed, sizeof(s_remap_prev));
              actor->pc_cursor_cooldown = 14;
            }
          }
        }
      }
    }
#else
    // PC options: 0=res, 1=fs, 2=vsync, 3=msaa, 4=textures
    if (actor->pc_cursor_cooldown == 0) {
      int changed = 0;
      if (stick_y > 30 || (on_btn & BUTTON_DUP)) {
        if (actor->pc_options_sel > 0) { actor->pc_options_sel--; actor->pc_cursor_cooldown = 8; }
      } else if (stick_y < -30 || (on_btn & BUTTON_DDOWN)) {
        if (actor->pc_options_sel < 4) { actor->pc_options_sel++; actor->pc_cursor_cooldown = 8; }
      }

      {
        static const int res_w[] = { 640, 960, 1280, 1600, 1920, 2560, 3840 };
        static const int res_h[] = { 480, 720,  720,  900, 1080, 1440, 2160 };
        enum { RES_COUNT = 7 };

      if (stick_x > 30 || (on_btn & BUTTON_DRIGHT)) {
        changed = 1; actor->pc_cursor_cooldown = 8;
        switch (actor->pc_options_sel) {
          case 0: {
            int i;
            for (i = 0; i < RES_COUNT - 1; i++) {
              if (g_pc_settings.window_width <= res_w[i]) break;
            }
            if (i < RES_COUNT - 1) i++;
            g_pc_settings.window_width = res_w[i];
            g_pc_settings.window_height = res_h[i];
          } break;
          case 1: g_pc_settings.fullscreen = (g_pc_settings.fullscreen + 1) % 3; break;
          case 2: g_pc_settings.vsync = !g_pc_settings.vsync; break;
          case 3:
            if (g_pc_settings.msaa == 0) g_pc_settings.msaa = 2;
            else if (g_pc_settings.msaa < 8) g_pc_settings.msaa *= 2;
            break;
          case 4:
            if (g_pc_settings.preload_textures < 2) g_pc_settings.preload_textures++;
            break;
        }
      } else if (stick_x < -30 || (on_btn & BUTTON_DLEFT)) {
        changed = 1; actor->pc_cursor_cooldown = 8;
        switch (actor->pc_options_sel) {
          case 0: {
            int i;
            for (i = RES_COUNT - 1; i > 0; i--) {
              if (g_pc_settings.window_width >= res_w[i]) break;
            }
            if (i > 0) i--;
            g_pc_settings.window_width = res_w[i];
            g_pc_settings.window_height = res_h[i];
          } break;
          case 1: g_pc_settings.fullscreen = (g_pc_settings.fullscreen + 2) % 3; break;
          case 2: g_pc_settings.vsync = !g_pc_settings.vsync; break;
          case 3:
            if (g_pc_settings.msaa > 2) g_pc_settings.msaa /= 2;
            else g_pc_settings.msaa = 0;
            break;
          case 4:
            if (g_pc_settings.preload_textures > 0) g_pc_settings.preload_textures--;
            break;
        }
      }
      }
      (void)changed;
    }
#endif
    return;
  }

  // main menu navigation
  if (actor->pc_cursor_cooldown == 0) {
    if (stick_y > 30 || (on_btn & BUTTON_DUP)) {
      if (actor->pc_menu_sel > 0) {
        actor->pc_menu_sel--;
        actor->pc_cursor_cooldown = 10;
      }
    } else if (stick_y < -30 || (on_btn & BUTTON_DDOWN)) {
      if (actor->pc_menu_sel < 1) {
        actor->pc_menu_sel++;
        actor->pc_cursor_cooldown = 10;
      }
    }
  }

  // select
  if (on_btn & (BUTTON_A | BUTTON_START)) {
    if (actor->pc_menu_sel == 0) {
      // start game
      if (mLd_CheckStartFlag() == TRUE &&
          aAL_wipe_end_check(game) == TRUE &&
          mTD_tdemo_button_ok_check()) {
        aAL_setupAction(actor, game, aAL_ACTION_FADE_OUT_START);
      }
    } else {
      // options
      actor->pc_options_open = 1;
      g_aAL_options_menu_open = 1;
      actor->pc_cursor_cooldown = 10;
      actor->pc_options_page = 0;
      actor->pc_options_focus = 0;
      actor->pc_options_sel = aAL_pc_first_selectable(actor->pc_options_page);
      g_pc_options_baseline = g_pc_settings;
#ifdef TARGET_VITA
      g_pc_controls_baseline = g_pc_controls;
      s_pc_confirm_delete = 0;
      s_remap_active = 0;
      s_controls_subpage = PC_SUBPAGE_GAME;
      banner_scan_folder();
      texpack_scan_folder();
#endif
    }
  }
}
#endif

static void aAL_setupAction(ANIMAL_LOGO_ACTOR* actor, GAME* game, int action) {
  static const ANIMAL_LOGO_ACTION_PROC init_proc[aAL_ACTION_NUM] = {
    &aAL_logo_in_init,
    &aAL_back_fadein_init,
    &aAL_start_key_chk_start_wait_init,
    (ANIMAL_LOGO_ACTION_PROC)&none_proc1,
    &aAL_fade_out_start_wait_init,
    (ANIMAL_LOGO_ACTION_PROC)&none_proc1,
    (ANIMAL_LOGO_ACTION_PROC)&none_proc1
  };

  static ANIMAL_LOGO_ACTION_PROC process[aAL_ACTION_NUM] = {
    &aAL_logo_in,
    &aAL_back_fadein,
    &aAL_start_key_chk_start_wait,
#ifdef PC_ENHANCEMENTS
    &aAL_pc_game_start_wait,
#else
    &aAL_game_start_wait,
#endif
    &aAL_fade_out_start_wait,
    (ANIMAL_LOGO_ACTION_PROC)&none_proc1,
    (ANIMAL_LOGO_ACTION_PROC)&none_proc1
  };

#ifdef TARGET_PC
  { extern int g_pc_verbose; if (g_pc_verbose) printf("[LOGO] aAL_setupAction: %d -> %d\n", actor->action, action); }
#else
  printf("[LOGO] aAL_setupAction: %d -> %d\n", actor->action, action);
#endif
  (*init_proc[action])(actor, game);
  actor->action = action;
  actor->action_proc = process[action];
}

static void aAL_actor_move(ACTOR* actor, GAME* game) {
  ANIMAL_LOGO_ACTOR* logo_actor = (ANIMAL_LOGO_ACTOR*)actor;

  lbRTC_Sampling();
  if (logo_actor->title_timer > 0) {
    logo_actor->title_timer--;
  }

  (*logo_actor->action_proc)(logo_actor, game);
}

#if VERSION == VER_GAFE01_00
static void aAL_copyright_draw(ANIMAL_LOGO_ACTOR* actor, GRAPH* graph) {
  static const u32 draw_pos_x[3] = { 61, 125, 189 };
  static const u32 draw_pos_y[3] = { 198, 198, 198 };

  Gfx* gfx;

  actor->copyright_opacity += aAL_COPYRIGHT_ALPHA_RATE;
  if (actor->copyright_opacity >= 255) {
    actor->copyright_opacity = 255;
  }

  OPEN_DISP(graph);

  gfx = NOW_FONT_DISP;
  gDPSetPrimColor(gfx++, 0, 255, 40, 40, 45, actor->copyright_opacity);
  gDPSetEnvColor(gfx++, 210, 210, 215, 0);
  gDPSetOtherMode(gfx++, G_AD_DISABLE | G_CD_DISABLE | G_CK_NONE | G_TC_FILT | G_TF_POINT | G_TT_NONE | G_TL_TILE | G_TD_CLAMP | G_TP_NONE | G_CYC_1CYCLE | G_PM_NPRIMITIVE, G_AC_THRESHOLD | G_ZS_PRIM | G_RM_CLD_SURF | G_RM_CLD_SURF2);
  gSPLoadGeometryMode(gfx++, 0);
  gDPSetCombineMode(gfx++, G_CC_TITLE, G_CC_TITLE);

  gDPLoadTextureTile(
    gfx++,
    log_win_nintendo1_tex,
    G_IM_FMT_IA, G_IM_SIZ_8b,
    TITLE_WIDTH, TITLE_HEIGHT,
    0, 0, TITLE_WIDTH - 1, TITLE_HEIGHT - 1,
    0,
    G_TX_WRAP | G_TX_NOMIRROR, G_TX_WRAP | G_TX_NOMIRROR,
    G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD
  );
  gfx = gfx_gSPTextureRectangle1(
    gfx,
    draw_pos_x[0] << 2, draw_pos_y[0] << 2,
    (TITLE_WIDTH + draw_pos_x[0]) << 2, (TITLE_HEIGHT + draw_pos_y[0]) << 2,
    0,
    0 << 5, 0 << 5,
    1 << 10, 1 << 10
  );

  gDPLoadTextureTile(
    gfx++,
    log_win_nintendo2_tex,
    G_IM_FMT_IA, G_IM_SIZ_8b,
    TITLE_WIDTH, TITLE_HEIGHT,
    0, 0, TITLE_WIDTH - 1, TITLE_HEIGHT - 1,
    0,
    G_TX_WRAP | G_TX_NOMIRROR, G_TX_WRAP | G_TX_NOMIRROR,
    G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD
  );
  gfx = gfx_gSPTextureRectangle1(
    gfx,
    draw_pos_x[1] << 2, draw_pos_y[1] << 2,
    (TITLE_WIDTH + draw_pos_x[1]) << 2, (TITLE_HEIGHT + draw_pos_y[1]) << 2,
    0,
    0 << 5, 0 << 5,
    1 << 10, 1 << 10
  );

  gDPLoadTextureTile(
    gfx++,
    log_win_nintendo3_tex,
    G_IM_FMT_IA, G_IM_SIZ_8b,
    TITLE_WIDTH, TITLE_HEIGHT,
    0, 0, TITLE_WIDTH - 1, TITLE_HEIGHT - 1,
    0,
    G_TX_WRAP | G_TX_NOMIRROR, G_TX_WRAP | G_TX_NOMIRROR,
    G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD
  );
  gfx = gfx_gSPTextureRectangle1(
    gfx,
    draw_pos_x[2] << 2, draw_pos_y[2] << 2,
    (TITLE_WIDTH + draw_pos_x[2]) << 2, (TITLE_HEIGHT + draw_pos_y[2]) << 2,
    0,
    0 << 5, 0 << 5,
    1 << 10, 1 << 10
  );

  SET_FONT_DISP(gfx);

  CLOSE_DISP(graph);
}
#elif VERSION == VER_GAFU01_00
extern Gfx logo_nin_copyT_model[];

static void aAL_copyright_draw(ANIMAL_LOGO_ACTOR* actor, GRAPH* graph) {
    // clang-format off
    static const Gfx init_disp[] = {
        gsSPTexture(0, 0, 0, 0, G_ON),
        gsSPLoadGeometryMode(G_CULL_BACK),
        gsDPSetOtherMode(G_AD_DISABLE | G_CD_DISABLE | G_CK_NONE | G_TC_FILT | G_TF_BILERP | G_TT_NONE | G_TL_TILE | G_TD_CLAMP | G_TP_PERSP | G_CYC_1CYCLE | G_PM_NPRIMITIVE, G_AC_NONE | G_ZS_PRIM | G_RM_XLU_SURF | G_RM_XLU_SURF2),
        gsDPSetCombineLERP(0, 0, 0, PRIMITIVE, 0, 0, 0, TEXEL0, 0, 0, 0, PRIMITIVE, 0, 0, 0, TEXEL0),
        gsSPEndDisplayList(),
    };
    // clang-format on

    actor->copyright_opacity += aAL_COPYRIGHT_ALPHA_RATE;
    if (actor->copyright_opacity >= 255) {
        actor->copyright_opacity = 255;
    }

    Matrix_push();

    OPEN_FONT_DISP(graph);

    Matrix_translate(32.0f, -1376.0f, 0.0f, MTX_MULT);
    Matrix_scale(0.16208267f, 0.16208267f, 0.16208267f, MTX_MULT);
    gSPMatrix(FONT_DISP++, _Matrix_to_Mtx_new(graph), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    gDPSetPrimColor(FONT_DISP++, 0, 255, 255, 255, 255, actor->copyright_opacity);
    gSPDisplayList(FONT_DISP++, init_disp);
    gSPDisplayList(FONT_DISP++, logo_nin_copyT_model);

    CLOSE_FONT_DISP(graph);

    Matrix_pull();
}
#endif

static void aAL_tm_draw(GRAPH* graph) {
  static const Gfx init_disp[] = {
    gsSPLoadGeometryMode(G_CULL_BACK),
    gsDPSetOtherMode(G_AD_DISABLE | G_CD_DISABLE | G_CK_NONE | G_TC_FILT | G_TF_BILERP | G_TT_NONE | G_TL_TILE | G_TD_CLAMP | G_TP_PERSP | G_CYC_1CYCLE | G_PM_NPRIMITIVE, G_AC_NONE | G_ZS_PRIM | G_RM_XLU_SURF | G_RM_XLU_SURF2),
    gsDPSetCombineMode(G_CC_TM, G_CC_TM),
    gsSPEndDisplayList()
  };

  Gfx* gfx;

  Matrix_push();
  Matrix_translate(1530.0f, 690.0f, 0.0f, MTX_MULT);
  Matrix_scale(0.162082675f, 0.162082675f, 0.162082675f, MTX_MULT);

  OPEN_DISP(graph);

  gfx = NOW_FONT_DISP;
  gSPMatrix(gfx++, _Matrix_to_Mtx_new(graph), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
  gSPDisplayList(gfx++, init_disp);
  gSPDisplayList(gfx++, logo_us_tm_model);
  SET_FONT_DISP(gfx);
  
  CLOSE_DISP(graph);

  Matrix_pull();
}

static void aAL_back_draw(GRAPH* graph, ANIMAL_LOGO_ACTOR* actor) {
  static const Gfx init_disp[] = {
    gsSPTexture(0, 0, 0, 0, G_ON),
    gsSPLoadGeometryMode(G_CULL_BACK),
    gsDPSetOtherMode(G_AD_DISABLE | G_CD_DISABLE | G_CK_NONE | G_TC_FILT | G_TF_BILERP | G_TT_NONE | G_TL_TILE | G_TD_CLAMP | G_TP_PERSP | G_CYC_1CYCLE | G_PM_NPRIMITIVE, G_AC_NONE | G_ZS_PRIM | G_RM_XLU_SURF | G_RM_XLU_SURF2),
    gsDPSetCombineMode(G_CC_BACK, G_CC_BACK),
    gsSPEndDisplayList()
  };

  Gfx* gfx;

  Matrix_push();
  Matrix_translate(0.0f, 730.0f, 0.0f, MTX_MULT);
  Matrix_scale(0.135f, 0.135f, 0.135f, MTX_MULT);

  OPEN_DISP(graph);

  gfx = NOW_FONT_DISP;
  gSPMatrix(gfx++, _Matrix_to_Mtx_new(graph), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
  gDPSetPrimColor(gfx++, 0, 255, 80, 60, 0, actor->back_opacity);
  gSPDisplayList(gfx++, init_disp);
  gSPDisplayList(gfx++, logo_us_backA_model);
  gSPDisplayList(gfx++, logo_us_backB_model);
  gSPDisplayList(gfx++, logo_us_backC_model);
  gSPDisplayList(gfx++, logo_us_backD_model);
  SET_FONT_DISP(gfx);

  CLOSE_DISP(graph);

  Matrix_pull();
}

static void aAL_press_start_draw(ANIMAL_LOGO_ACTOR* actor, GRAPH* graph) {
  static const u32 draw_pos_x[2] = { 96, 160 };
  static const u32 draw_pos_y[2] = { 159, 159 };

  static const u32 ps_prim_r[5] = { 70, 60, 60, 40, 40 };
  static const u32 ps_prim_g[5] = { 40, 50, 40, 50, 50 };
  static const u32 ps_prim_b[5] = { 40, 30, 60, 70, 60 };
  
  static const u32 ps_env_r[5] = { 255, 255, 255, 120, 165 };
  static const u32 ps_env_g[5] = {  90, 135, 100, 205, 245 };
  static const u32 ps_env_b[5] = {  30,   0, 255, 245,   0 };

  Gfx* gfx;
  int titledemo_no;
  f32 alpha;
  titledemo_no = actor->titledemo_no;
  alpha = actor->press_start_opacity;

  OPEN_DISP(graph);

  gfx = NOW_FONT_DISP;
  gDPSetPrimColor(gfx++, 0, 255, ps_prim_r[titledemo_no], ps_prim_g[titledemo_no], ps_prim_b[titledemo_no], (u32)alpha);
  gDPSetEnvColor(gfx++, ps_env_r[titledemo_no], ps_env_g[titledemo_no], ps_env_b[titledemo_no], 0);
  gDPSetOtherMode(gfx++, G_AD_DISABLE | G_CD_DISABLE | G_CK_NONE | G_TC_FILT | G_TF_POINT | G_TT_NONE | G_TL_TILE | G_TD_CLAMP | G_TP_NONE | G_CYC_1CYCLE | G_PM_NPRIMITIVE, G_AC_THRESHOLD | G_ZS_PRIM | G_RM_XLU_SURF | G_RM_XLU_SURF2);
  gSPLoadGeometryMode(gfx++, 0);
  gDPSetCombineMode(gfx++, G_CC_PRESS_START, G_CC_PRESS_START);

  gDPLoadTextureTile(
    gfx++,
    log_win_logo3_tex,
    G_IM_FMT_IA, G_IM_SIZ_8b,
    TITLE_WIDTH, TITLE_HEIGHT,
    0, 0, TITLE_WIDTH - 1, TITLE_HEIGHT - 1,
    0,
    G_TX_WRAP | G_TX_NOMIRROR, G_TX_WRAP | G_TX_NOMIRROR,
    G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD
  );

  gfx = gfx_gSPTextureRectangle1(
    gfx,
    draw_pos_x[0] << 2, draw_pos_y[0] << 2,
    (TITLE_WIDTH + draw_pos_x[0]) << 2, (TITLE_HEIGHT + draw_pos_y[0]) << 2,
    0,
    0 << 5, 0 << 5,
    1 << 10, 1 << 10
  );

  gDPLoadTextureTile(
    gfx++,
    log_win_logo4_tex,
    G_IM_FMT_IA, G_IM_SIZ_8b,
    TITLE_WIDTH, TITLE_HEIGHT,
    0, 0, TITLE_WIDTH - 1, TITLE_HEIGHT - 1,
    0,
    G_TX_WRAP | G_TX_NOMIRROR, G_TX_WRAP | G_TX_NOMIRROR,
    G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD
  );

  gfx = gfx_gSPTextureRectangle1(
    gfx,
    draw_pos_x[1] << 2, draw_pos_y[1] << 2,
    (TITLE_WIDTH + draw_pos_x[1]) << 2, (TITLE_HEIGHT + draw_pos_y[1]) << 2,
    0,
    0 << 5, 0 << 5,
    1 << 10, 1 << 10
  );

  SET_FONT_DISP(gfx);

  CLOSE_DISP(graph);
}

static void aAL_skl_draw(GAME* game, cKF_SkeletonInfo_R_c* skl_keyframe) {
  Mtx* m;

  OPEN_DISP(game->graph);

  m = GRAPH_ALLOC_TYPE(game->graph, Mtx, skl_keyframe->skeleton->num_shown_joints);
  if (m != NULL) {
    cKF_Si3_draw_R_SV(game, skl_keyframe, m, NULL, NULL, NULL);
  }

  CLOSE_DISP(game->graph);
}

static void aAL_title_draw(GAME* game, ANIMAL_LOGO_ACTOR* actor) {
  static const Gfx init_disp[] = {
    gsDPSetOtherMode(G_AD_NOTPATTERN | G_CD_MAGICSQ | G_CK_NONE | G_TC_FILT | G_TF_BILERP | G_TT_RGBA16 | G_TL_TILE | G_TD_CLAMP | G_TP_PERSP | G_CYC_1CYCLE | G_PM_NPRIMITIVE, G_AC_NONE | G_ZS_PRIM | G_RM_XLU_SURF | G_RM_XLU_SURF2),
    gsDPSetCombineMode(G_CC_DECALRGBA, G_CC_DECALRGBA),
    gsSPLoadGeometryMode(G_CULL_BACK),
    gsSPTexture(0, 0, 0, 0, G_ON),
    gsSPEndDisplayList()
  };

  Gfx* poly_save;
  GRAPH* graph = game->graph;

  Matrix_push();
  Matrix_translate(0.0f, 730.0f, 0.0f, MTX_MULT);
  Matrix_scale(0.135f, 0.135f, 0.135f, MTX_MULT);

  OPEN_DISP(graph);

  // we need to save the opaque polygon gfx buffer and swap with font because cKF utilizes opaque polygon gfx,
  // but we want this on the font gfx buffer
  poly_save = NOW_POLY_OPA_DISP;
  SET_POLY_OPA_DISP(NOW_FONT_DISP);
  gSPMatrix(NOW_POLY_OPA_DISP++, _Matrix_to_Mtx_new(graph), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
  gSPDisplayList(NOW_POLY_OPA_DISP++, init_disp);
  
  aAL_skl_draw(game, &actor->animal.skeleton);
  aAL_skl_draw(game, &actor->cros.skeleton);
  aAL_skl_draw(game, &actor->sing.skeleton);

  SET_FONT_DISP(NOW_POLY_OPA_DISP);
  SET_POLY_OPA_DISP(poly_save);

  CLOSE_DISP(graph);

  Matrix_pull();
}

#ifdef PC_ENHANCEMENTS
/* Shared cursor glyph used by both the main title menu and the options overlay. */
static u8 str_arrow[] = ">";

static void aAL_pc_options_draw(ANIMAL_LOGO_ACTOR* actor, GAME* game) {
  GRAPH* graph = game->graph;
  char buf[48];
  int len;
  // labels left-aligned at x, values right-aligned to right_x (computed
  // below). 40 / 35 px margins on a 320-wide screen.
  f32 x = 40.0f;
  f32 val_x = 175.0f;
#ifdef TARGET_VITA
  f32 y = 22.0f;
  f32 line_h = 13.0f;
#else
  f32 y = 36.0f;
  f32 line_h = 14.0f;
#endif

  // fill rect (not texrect): texrect inherits tile 0 from the prior
  // font glyph, which caused rare alpha=0 flashes.
  {
    Gfx* gfx;
    OPEN_DISP(graph);
    gfx = NOW_FONT_DISP;
    gDPPipeSync(gfx++);
#ifdef TARGET_PC
    gDPNoOpTag(gfx++, PC_NOOP_WIDESCREEN_STRETCH);
#endif
    gDPSetOtherMode(gfx++,
      G_AD_DISABLE | G_CD_MAGICSQ | G_CK_NONE | G_TC_FILT |
      G_TF_POINT | G_TT_NONE | G_TL_TILE | G_TD_CLAMP |
      G_TP_NONE | G_CYC_1CYCLE | G_PM_NPRIMITIVE,
      G_AC_NONE | G_ZS_PRIM | G_RM_XLU_SURF | G_RM_XLU_SURF2);
    gDPSetCombineMode(gfx++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);
    gDPSetPrimColor(gfx++, 0, 0, 0, 0, 0, 200);
    gDPFillRectangle(gfx++, 0, 0, SCREEN_WIDTH * 2, SCREEN_HEIGHT * 2);
    gDPPipeSync(gfx++);
#ifdef TARGET_PC
    gDPNoOpTag(gfx++, PC_NOOP_WIDESCREEN_STRETCH_OFF);
#endif
    SET_FONT_DISP(gfx);
    CLOSE_DISP(graph);
  }

  int sel = actor->pc_options_sel;
  int item = 0;

  f32 right_x = 285.0f;     // 35 px from the right edge
  f32 val_min_x = 145.0f;   // value shrinks if it would start left of this

  // title
  {
    static u8 str_title[] = "- Options -";
    f32 tw = (f32)mFont_GetStringWidth(str_title, sizeof(str_title) - 1, TRUE);
    mFont_SetLineStrings(game, str_title, sizeof(str_title) - 1,
      (SCREEN_WIDTH_F - tw) * 0.5f, y,
      255, 255, 255, 255, FALSE, TRUE, 1.0f, 1.0f, mFont_MODE_FONT);
  }
  y += line_h * 1.4f;

// right-align to right_x, shrinking to fit past val_min_x. 0.55 is the
// readability floor; below that we accept a hair of overflow.
#define DRAW_VALUE_RIGHT(text_ptr, text_len, r_, g_, b_, a_, yy) do {           \
    f32 _vw = (f32)mFont_GetStringWidth((u8*)(text_ptr), (text_len), TRUE);     \
    f32 _vscale = 1.0f;                                                          \
    f32 _max_w = right_x - val_min_x;                                            \
    if (_vw > _max_w) _vscale = _max_w / _vw;                                    \
    if (_vscale < 0.55f) _vscale = 0.55f;                                        \
    f32 _vx = right_x - _vw * _vscale;                                           \
    mFont_SetLineStrings(game, (u8*)(text_ptr), (text_len), _vx, (yy),           \
      (r_), (g_), (b_), (a_), FALSE, TRUE, _vscale, _vscale, mFont_MODE_FONT);   \
  } while(0)

// Label + value row. dirty=1 prefixes an asterisk. list_active=0 dims the
// selected row (focus is on a tab bar, cursor lives there). val_x is kept
// only for source-compat with existing call sites.
#define DRAW_OPT_ROW_F(lbl_arr, val_x_unused, dirty, list_active) do { \
    (void)(val_x_unused); \
    int is_sel = ((sel) == item) && (list_active); \
    int bright = is_sel ? 255 : 180; \
    int alpha  = is_sel ? 255 : 160; \
    if (dirty) { \
      static u8 mark[] = { '*' }; \
      mFont_SetLineStrings(game, mark, 1, x - 8.0f, y, \
        255, 220, 80, alpha, FALSE, TRUE, 1.0f, 1.0f, mFont_MODE_FONT); \
    } \
    mFont_SetLineStrings(game, lbl_arr, sizeof(lbl_arr), x, y, \
      bright, bright, bright, alpha, FALSE, TRUE, 1.0f, 1.0f, mFont_MODE_FONT); \
    DRAW_VALUE_RIGHT(buf, len, bright, bright, bright, alpha, y); \
    if (is_sel) mFont_SetLineStrings(game, str_arrow, 1, x - 18.0f, y, \
      255, 255, 255, 255, FALSE, TRUE, 1.0f, 1.0f, mFont_MODE_FONT); \
    item++; y += line_h; \
  } while(0)

// PC build has no tab bar focus state.
#define DRAW_OPT_ROW(lbl_arr, val_x, dirty) DRAW_OPT_ROW_F(lbl_arr, val_x, dirty, 1)

#ifdef TARGET_VITA
  {
    PCSettings* B = &g_pc_options_baseline;
    int page = actor->pc_options_page;
    int list_active = (actor->pc_options_focus == 0);

    // main tab bar: uniform gap between tabs, whole bar centered. Active
    // tab is bright; chevrons hug it when the tab bar has focus.
    {
      static u8 lbl_graphics[] = { 'G', 'r', 'a', 'p', 'h', 'i', 'c', 's' };
      static u8 lbl_gameplay[] = { 'G', 'a', 'm', 'e', 'p', 'l', 'a', 'y' };
      static u8 lbl_save[]     = { 'S', 'a', 'v', 'e' };
      static u8 lbl_controls[] = { 'C', 'o', 'n', 't', 'r', 'o', 'l', 's' };
      static u8 lcursor[] = { '<' };
      static u8 rcursor[] = { '>' };

      struct { u8* lbl; int len; } tabs[PC_PAGE_COUNT] = {
        { lbl_graphics, sizeof(lbl_graphics) },
        { lbl_gameplay, sizeof(lbl_gameplay) },
        { lbl_save,     sizeof(lbl_save) },
        { lbl_controls, sizeof(lbl_controls) },
      };

      f32 widths[PC_PAGE_COUNT];
      f32 gap = 24.0f;
      f32 total_w = gap * (PC_PAGE_COUNT - 1);
      for (int i = 0; i < PC_PAGE_COUNT; i++) {
        widths[i] = (f32)mFont_GetStringWidth(tabs[i].lbl, tabs[i].len, TRUE);
        total_w += widths[i];
      }

      f32 tx = (SCREEN_WIDTH_F - total_w) * 0.5f;
      for (int i = 0; i < PC_PAGE_COUNT; i++) {
        int is_active = (i == page);
        int r = is_active ? 255 : 130;
        int g = is_active ? 255 : 130;
        int b = is_active ? 255 : 130;
        int a = is_active ? 255 : 180;
        mFont_SetLineStrings(game, tabs[i].lbl, tabs[i].len, tx, y,
          r, g, b, a, FALSE, TRUE, 1.0f, 1.0f, mFont_MODE_FONT);
        if (is_active && !list_active) {
          mFont_SetLineStrings(game, lcursor, 1, tx - 10.0f, y,
            255, 255, 255, 255, FALSE, TRUE, 1.0f, 1.0f, mFont_MODE_FONT);
          mFont_SetLineStrings(game, rcursor, 1, tx + widths[i] + 4.0f, y,
            255, 255, 255, 255, FALSE, TRUE, 1.0f, 1.0f, mFont_MODE_FONT);
        }
        tx += widths[i] + gap;
      }
    }
    y += line_h * 1.6f;

    // sub tab bar (Game / NES) - Controls page only.
    if (page == PC_PAGE_CONTROLS) {
      static u8 sub_lbl_game[] = { 'G','a','m','e' };
      static u8 sub_lbl_nes[]  = { 'N','E','S' };
      static u8 sub_lcursor[]  = { '<' };
      static u8 sub_rcursor[]  = { '>' };

      struct { u8* lbl; int len; } subs[PC_SUBPAGE_COUNT] = {
        { sub_lbl_game, sizeof(sub_lbl_game) },
        { sub_lbl_nes,  sizeof(sub_lbl_nes)  },
      };

      f32 sub_widths[PC_SUBPAGE_COUNT];
      f32 sub_gap = 20.0f;
      f32 sub_total = sub_gap * (PC_SUBPAGE_COUNT - 1);
      for (int i = 0; i < PC_SUBPAGE_COUNT; i++) {
        sub_widths[i] = (f32)mFont_GetStringWidth(subs[i].lbl, subs[i].len, TRUE);
        sub_total += sub_widths[i];
      }

      int subtab_focused = (actor->pc_options_focus == 2);
      f32 stx = (SCREEN_WIDTH_F - sub_total) * 0.5f;
      for (int i = 0; i < PC_SUBPAGE_COUNT; i++) {
        int is_active = (i == s_controls_subpage);
        int r = is_active ? 220 : 110;
        int g = is_active ? 220 : 110;
        int b = is_active ? 220 : 110;
        int a = is_active ? 220 : 160;
        mFont_SetLineStrings(game, subs[i].lbl, subs[i].len, stx, y,
          r, g, b, a, FALSE, TRUE, 1.0f, 1.0f, mFont_MODE_FONT);
        if (is_active && subtab_focused) {
          mFont_SetLineStrings(game, sub_lcursor, 1, stx - 9.0f, y,
            255, 255, 255, 255, FALSE, TRUE, 1.0f, 1.0f, mFont_MODE_FONT);
          mFont_SetLineStrings(game, sub_rcursor, 1, stx + sub_widths[i] + 3.0f, y,
            255, 255, 255, 255, FALSE, TRUE, 1.0f, 1.0f, mFont_MODE_FONT);
        }
        stx += sub_widths[i] + sub_gap;
      }
      // small extra gap so the up scroll indicator has room
      y += line_h * 1.4f + 4.0f;
    }

    if (page == PC_PAGE_GRAPHICS) {
      // Resolution
      {
        const char* rs = g_pc_settings.render_scale == 100 ? "< 960x544 >" :
                         g_pc_settings.render_scale == 75  ? "< 720x408 >" : "< 480x272 >";
        len = sprintf(buf, "%s", rs);
      }
      { static u8 lbl[] = { 'R', 'e', 's', 'o', 'l', 'u', 't', 'i', 'o', 'n' };
        DRAW_OPT_ROW_F(lbl, val_x, g_pc_settings.render_scale != B->render_scale, list_active); }

      // Aspect Ratio
      len = sprintf(buf, "< %s >", g_pc_settings.aspect_mode ? "Original 4:3" : "Widescreen");
      { static u8 lbl[] = { 'A', 's', 'p', 'e', 'c', 't' };
        DRAW_OPT_ROW_F(lbl, val_x, g_pc_settings.aspect_mode != B->aspect_mode, list_active); }

      // Banner (only in 4:3 mode)
      if (g_pc_settings.aspect_mode) {
        if (!g_pc_settings.banner_name[0] || g_banner_count == 0) {
          len = sprintf(buf, "< None >");
        } else {
          char display[32];
          strncpy(display, g_pc_settings.banner_name, sizeof(display) - 1);
          display[sizeof(display) - 1] = '\0';
          int dlen = (int)strlen(display);
          if (dlen > 4 && (strcmp(display + dlen - 4, ".png") == 0 || strcmp(display + dlen - 4, ".PNG") == 0))
            display[dlen - 4] = '\0';
          len = sprintf(buf, "< %s >", display);
        }

        int bidx = banner_find_index(g_pc_settings.banner_name);
        int is_invalid = (bidx >= 0 && !g_banner_list[bidx].valid);
        int dirty = strcmp(g_pc_settings.banner_name, B->banner_name) != 0;

        {
          static u8 lbl[] = { 'B', 'a', 'n', 'n', 'e', 'r' };
          int is_sel = (sel == item) && list_active;
          int bright = is_sel ? 255 : 180;
          int alpha  = is_sel ? 255 : 160;
          int r = is_invalid ? 255 : bright;
          int g_c = is_invalid ? 80 : bright;
          int b = is_invalid ? 80 : bright;
          if (dirty) {
            static u8 mark[] = { '*' };
            mFont_SetLineStrings(game, mark, 1, x - 8.0f, y,
              255, 220, 80, alpha, FALSE, TRUE, 1.0f, 1.0f, mFont_MODE_FONT);
          }
          mFont_SetLineStrings(game, lbl, sizeof(lbl), x, y,
            r, g_c, b, alpha, FALSE, TRUE, 1.0f, 1.0f, mFont_MODE_FONT);
          DRAW_VALUE_RIGHT(buf, len, r, g_c, b, alpha, y);
          if (is_sel) mFont_SetLineStrings(game, str_arrow, 1, x - 18.0f, y,
            255, 255, 255, 255, FALSE, TRUE, 1.0f, 1.0f, mFont_MODE_FONT);
          item++; y += line_h;
        }
      }

      // MSAA
      if (g_pc_settings.msaa > 0)
        len = sprintf(buf, "< %dx >", g_pc_settings.msaa);
      else
        len = sprintf(buf, "< Off >");
      { static u8 lbl[] = { 'M', 'S', 'A', 'A' };
        DRAW_OPT_ROW_F(lbl, val_x, g_pc_settings.msaa != B->msaa, list_active); }

      // Texture Pack
      if (!g_pc_settings.texture_pack[0] || g_texpack_count == 0) {
        len = sprintf(buf, "< None >");
      } else {
        len = sprintf(buf, "< %s >", g_pc_settings.texture_pack);
      }
      { static u8 lbl[] = { 'T', 'e', 'x', ' ', 'P', 'a', 'c', 'k' };
        DRAW_OPT_ROW_F(lbl, val_x, strcmp(g_pc_settings.texture_pack, B->texture_pack) != 0, list_active); }

      // NES Aspect
      len = sprintf(buf, "< %s >", g_pc_settings.nes_aspect ? "4:3" : "Stretch");
      { static u8 lbl[] = { 'N', 'E', 'S', ' ', 'A', 's', 'p', 'e', 'c', 't' };
        DRAW_OPT_ROW_F(lbl, val_x, g_pc_settings.nes_aspect != B->nes_aspect, list_active); }

      // Boot Logo
      len = sprintf(buf, "< %s >", g_pc_settings.boot_logo ? "On" : "Off");
      { static u8 lbl[] = { 'B', 'o', 'o', 't', ' ', 'L', 'o', 'g', 'o' };
        DRAW_OPT_ROW_F(lbl, val_x, g_pc_settings.boot_logo != B->boot_logo, list_active); }

    } else if (page == PC_PAGE_GAMEPLAY) {
      // Disable Resetti
      len = sprintf(buf, "< %s >", g_pc_settings.disable_resetti ? "On" : "Off");
      { static u8 lbl[] = { 'N', 'o', ' ', 'R', 'e', 's', 'e', 't', 't', 'i' };
        DRAW_OPT_ROW_F(lbl, val_x, g_pc_settings.disable_resetti != B->disable_resetti, list_active); }

      // Time Sync
      len = sprintf(buf, "< %s >", g_pc_settings.time_sync ? "On" : "Off");
      { static u8 lbl[] = { 'T', 'i', 'm', 'e', ' ', 'S', 'y', 'n', 'c' };
        DRAW_OPT_ROW_F(lbl, val_x, g_pc_settings.time_sync != B->time_sync, list_active); }

      // Free Cam
      len = sprintf(buf, "< %s >", g_pc_settings.free_cam ? "On" : "Off");
      { static u8 lbl[] = { 'F', 'r', 'e', 'e', ' ', 'C', 'a', 'm' };
        DRAW_OPT_ROW_F(lbl, val_x, g_pc_settings.free_cam != B->free_cam, list_active); }

      // Text Speed
      {
        const char* ts = g_pc_settings.text_speed == 0 ? "Slow" :
                         g_pc_settings.text_speed == 1 ? "Normal" : "Fast";
        len = sprintf(buf, "< %s >", ts);
      }
      { static u8 lbl[] = { 'T', 'e', 'x', 't', ' ', 'S', 'p', 'e', 'e', 'd' };
        DRAW_OPT_ROW_F(lbl, val_x, g_pc_settings.text_speed != B->text_speed, list_active); }

    } else if (page == PC_PAGE_SAVE) {
      // Save Slot
      len = sprintf(buf, "< Slot %s >", g_pc_settings.save_slot ? "B" : "A");
      { static u8 lbl[] = { 'S', 'a', 'v', 'e', ' ', 'S', 'l', 'o', 't' };
        DRAW_OPT_ROW_F(lbl, val_x, g_pc_settings.save_slot != B->save_slot, list_active); }

      // Auto Save
      len = sprintf(buf, "< %s >", g_pc_settings.auto_save ? "On" : "Off");
      { static u8 lbl[] = { 'A', 'u', 't', 'o', ' ', 'S', 'a', 'v', 'e' };
        DRAW_OPT_ROW_F(lbl, val_x, g_pc_settings.auto_save != B->auto_save, list_active); }

      // Delete Town: action row, no toggle value
      {
        static u8 lbl[]    = { 'D', 'e', 'l', 'e', 't', 'e', ' ', 'T', 'o', 'w', 'n' };
        static u8 action[] = { '(', 'p', 'r', 'e', 's', 's', ' ', 'A', ')' };
        int is_sel = (sel == item) && list_active;
        int r = is_sel ? 255 : 200;
        int g = is_sel ? 100 : 110;
        int b = is_sel ? 100 : 110;
        int a = is_sel ? 255 : 180;
        mFont_SetLineStrings(game, lbl, sizeof(lbl), x, y,
          r, g, b, a, FALSE, TRUE, 1.0f, 1.0f, mFont_MODE_FONT);
        DRAW_VALUE_RIGHT(action, sizeof(action), r, g, b, a, y);
        if (is_sel) mFont_SetLineStrings(game, str_arrow, 1, x - 18.0f, y,
          255, 255, 255, 255, FALSE, TRUE, 1.0f, 1.0f, mFont_MODE_FONT);
        item++; y += line_h;
      }
    } else if (page == PC_PAGE_CONTROLS) {
      int n_rows = 0;
      const PCControlsRow* rows = aAL_pc_controls_rows(&n_rows);

      // keep the selected row centered so context above and below stays visible
      int visible = PC_CONTROLS_VISIBLE;
      if (visible > n_rows) visible = n_rows;
      int scroll = sel - visible / 2;
      if (scroll > n_rows - visible) scroll = n_rows - visible;
      if (scroll < 0) scroll = 0;

      f32 ctrl_line_h = 12.5f;
      f32 list_start_y = y;

      // scaled-down dots fit between sub-tab bar and first row without
      // overlap. "..." since the font has no caret glyph.
      if (scroll > 0) {
        static u8 up_dots[] = { '.', '.', '.' };
        f32 ud_scale = 0.7f;
        f32 ud_w = (f32)mFont_GetStringWidth(up_dots, sizeof(up_dots), TRUE) * ud_scale;
        mFont_SetLineStrings(game, up_dots, sizeof(up_dots),
          (SCREEN_WIDTH_F - ud_w) * 0.5f, list_start_y - 6.0f,
          210, 210, 210, 230, FALSE, TRUE, ud_scale, ud_scale, mFont_MODE_FONT);
      }

      static const char* axis_names[] = { "none", "main_x", "main_y", "cstick_x", "cstick_y" };

      for (int row = scroll; row < scroll + visible && row < n_rows; row++) {
        const PCControlsRow* r = &rows[row];
        int rlen = (int)strlen(r->label);

        if (r->kind == CR_HEADER) {
          // current tables don't use headers; keep the branch for the case
          // they come back
          y += ctrl_line_h;
          continue;
        }

        int is_sel = (sel == row) && list_active;
        int is_remap_target = is_sel && s_remap_active;
        int bright = is_sel ? 255 : 180;
        int alpha  = is_sel ? 255 : 160;

        mFont_SetLineStrings(game, (u8*)r->label, rlen, x, y,
          bright, bright, bright, alpha, FALSE, TRUE, 1.0f, 1.0f, mFont_MODE_FONT);

        if (is_remap_target) {
          // captured Vita buttons joined with " or ", plus countdown
          int secs = (s_remap_timeout + 59) / 60;
          if (secs < 0) secs = 0;
          char captured_str[64];
          int csz = 0;
          captured_str[0] = '\0';
          for (int i = 0; i < PCV_COUNT; i++) {
            if (!(s_remap_captured & (1u << i))) continue;
            const char* nm = pc_controls_vita_name(i);
            int n = snprintf(captured_str + csz, sizeof(captured_str) - csz,
                             "%s%s", (csz > 0) ? " or " : "", nm);
            if (n > 0 && csz + n < (int)sizeof(captured_str)) csz += n;
          }
          if (csz == 0) snprintf(captured_str, sizeof(captured_str), "none");

          char rline[96];
          int rl = snprintf(rline, sizeof(rline), "< %s (%ds) >", captured_str, secs);
          DRAW_VALUE_RIGHT(rline, rl, 255, 230, 100, 255, y);
        } else {
          char vbuf[80];
          int vlen = 0;

          if (r->kind == CR_MAIN_BTN || r->kind == CR_NES_BTN || r->kind == CR_NES_TURBO) {
            // reverse-lookup the right map for any Vita button bound here
            char binding_str[64];
            int bsz = 0;
            binding_str[0] = '\0';
            for (int i = 0; i < PCV_COUNT; i++) {
              int hit = 0;
              if (r->kind == CR_MAIN_BTN)        hit = (g_pc_controls.main_map[i]      & (uint16_t)r->bit) != 0;
              else if (r->kind == CR_NES_BTN)    hit = (g_pc_controls.nes_map[i]       & (uint8_t)r->bit)  != 0;
              else /* CR_NES_TURBO */            hit = (g_pc_controls.nes_turbo_map[i] & (uint8_t)r->bit)  != 0;
              if (!hit) continue;
              const char* nm = pc_controls_vita_name(i);
              int max = (int)sizeof(binding_str) - bsz - 4;
              if (max <= 0) { snprintf(binding_str + bsz, sizeof(binding_str) - bsz, "..."); bsz += 3; break; }
              int n = snprintf(binding_str + bsz, sizeof(binding_str) - bsz,
                               "%s%s", (bsz > 0) ? " or " : "", nm);
              if (n <= 0) break;
              bsz += n;
            }
            if (bsz == 0) snprintf(binding_str, sizeof(binding_str), "none");
            vlen = snprintf(vbuf, sizeof(vbuf), "%s", binding_str);
          } else if (r->kind == CR_GC_AXIS) {
            // Vita axes targeting this GC axis, "-" prefix when inverted
            char binding_str[64];
            int bsz = 0;
            binding_str[0] = '\0';
            static const char* pca_names[] = { "lstick_x", "lstick_y", "rstick_x", "rstick_y" };
            for (int p = 0; p < PCA_COUNT; p++) {
              if (g_pc_controls.main_axis_map[p].target != (PCGCAxis)r->bit) continue;
              const char* prefix = (g_pc_controls.main_axis_map[p].invert < 0) ? "-" : "";
              int max = (int)sizeof(binding_str) - bsz - 4;
              if (max <= 0) { snprintf(binding_str + bsz, sizeof(binding_str) - bsz, "..."); bsz += 3; break; }
              int n = snprintf(binding_str + bsz, sizeof(binding_str) - bsz,
                               "%s%s%s", (bsz > 0) ? " or " : "", prefix, pca_names[p]);
              if (n <= 0) break;
              bsz += n;
            }
            if (bsz == 0) snprintf(binding_str, sizeof(binding_str), "none");
            vlen = snprintf(vbuf, sizeof(vbuf), "%s", binding_str);
            (void)axis_names; // suppress unused if path not taken
          }

          DRAW_VALUE_RIGHT(vbuf, vlen, bright, bright, bright, alpha, y);
        }

        if (is_sel && !s_remap_active) {
          mFont_SetLineStrings(game, str_arrow, 1, x - 18.0f, y,
            255, 255, 255, 255, FALSE, TRUE, 1.0f, 1.0f, mFont_MODE_FONT);
        }
        item++; y += ctrl_line_h;
      }

      if (scroll + visible < n_rows) {
        static u8 dn_dots[] = { '.', '.', '.' };
        f32 dd_scale = 0.7f;
        f32 dd_w = (f32)mFont_GetStringWidth(dn_dots, sizeof(dn_dots), TRUE) * dd_scale;
        mFont_SetLineStrings(game, dn_dots, sizeof(dn_dots),
          (SCREEN_WIDTH_F - dd_w) * 0.5f, y - 4.0f,
          210, 210, 210, 230, FALSE, TRUE, dd_scale, dd_scale, mFont_MODE_FONT);
      }
    }

    // footer pinned to the bottom so different-length pages still line up.
    // hidden behind the delete-town modal.
    if (!s_pc_confirm_delete) {
      f32 footer_y = 208.0f;
      {
        static u8 str_save[] = { 'S', 'T', 'A', 'R', 'T', ':', ' ', 'S', 'a', 'v', 'e' };
        static u8 str_back[] = { 'B', ':', ' ', 'C', 'a', 'n', 'c', 'e', 'l' };
        f32 bw = (f32)mFont_GetStringWidth(str_back, sizeof(str_back), TRUE);
        mFont_SetLineStrings(game, str_save, sizeof(str_save), x, footer_y,
          255, 255, 255, 200, FALSE, TRUE, 1.0f, 1.0f, mFont_MODE_FONT);
        mFont_SetLineStrings(game, str_back, sizeof(str_back), right_x - bw, footer_y,
          255, 255, 255, 200, FALSE, TRUE, 1.0f, 1.0f, mFont_MODE_FONT);
      }
      footer_y += line_h + 4.0f;
      {
#ifdef TARGET_VITA
        // Vita restarts on save so the brief black screen doesn't look like a crash.
        static u8 str_unsaved[] = "* = unsaved change.  Save restarts game.";
#else
        static u8 str_unsaved[] = "* = unsaved change";
#endif
        f32 uscale = 0.85f;
        f32 uw = (f32)mFont_GetStringWidth(str_unsaved, sizeof(str_unsaved) - 1, TRUE) * uscale;
        mFont_SetLineStrings(game, str_unsaved, sizeof(str_unsaved) - 1,
          (SCREEN_WIDTH_F - uw) * 0.5f, footer_y,
          170, 170, 170, 140, FALSE, TRUE, uscale, uscale, mFont_MODE_FONT);
      }
    }

    // Delete Town modal overlay.
    if (s_pc_confirm_delete) {
      // fill rect not texrect: see aAL_pc_options_draw above.
      Gfx* gfx2;
      OPEN_DISP(graph);
      gfx2 = NOW_FONT_DISP;
      gDPPipeSync(gfx2++);
#ifdef TARGET_PC
      gDPNoOpTag(gfx2++, PC_NOOP_WIDESCREEN_STRETCH);
#endif
      gDPSetOtherMode(gfx2++,
        G_AD_DISABLE | G_CD_MAGICSQ | G_CK_NONE | G_TC_FILT |
        G_TF_POINT | G_TT_NONE | G_TL_TILE | G_TD_CLAMP |
        G_TP_NONE | G_CYC_1CYCLE | G_PM_NPRIMITIVE,
        G_AC_NONE | G_ZS_PRIM | G_RM_XLU_SURF | G_RM_XLU_SURF2);
      gDPSetCombineMode(gfx2++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);
      gDPSetPrimColor(gfx2++, 0, 0, 0, 0, 0, 180);
      gDPFillRectangle(gfx2++, 0, 0, SCREEN_WIDTH * 2, SCREEN_HEIGHT * 2);
      gDPPipeSync(gfx2++);
#ifdef TARGET_PC
      gDPNoOpTag(gfx2++, PC_NOOP_WIDESCREEN_STRETCH_OFF);
#endif
      SET_FONT_DISP(gfx2);
      CLOSE_DISP(graph);

      f32 dy = 90.0f;
      static u8 hdr[]   = { 'D','e','l','e','t','e',' ','t','h','i','s',' ','t','o','w','n','?' };
      static u8 warn1[] = { 'A','l','l',' ','p','r','o','g','r','e','s','s',' ','i','n',' ','t','h','i','s',' ','s','l','o','t' };
      static u8 warn2[] = { 'w','i','l','l',' ','b','e',' ','l','o','s','t',' ','f','o','r','e','v','e','r','.' };
      static u8 no_lbl[]  = { 'N', 'o' };
      static u8 yes_lbl[] = { 'Y', 'e', 's' };
      static u8 hint[]  = { 'A',':',' ','c','o','n','f','i','r','m','/','c','a','n','c','e','l',',',' ','L','/','R',':',' ','t','o','g','g','l','e' };

      f32 hw = (f32)mFont_GetStringWidth(hdr, sizeof(hdr), TRUE);
      mFont_SetLineStrings(game, hdr, sizeof(hdr),
        (SCREEN_WIDTH_F - hw) * 0.5f, dy,
        255, 230, 100, 255, FALSE, TRUE, 1.0f, 1.0f, mFont_MODE_FONT);
      f32 w1 = (f32)mFont_GetStringWidth(warn1, sizeof(warn1), TRUE);
      mFont_SetLineStrings(game, warn1, sizeof(warn1),
        (SCREEN_WIDTH_F - w1) * 0.5f, dy + 18.0f,
        220, 220, 220, 220, FALSE, TRUE, 1.0f, 1.0f, mFont_MODE_FONT);
      f32 w2 = (f32)mFont_GetStringWidth(warn2, sizeof(warn2), TRUE);
      mFont_SetLineStrings(game, warn2, sizeof(warn2),
        (SCREEN_WIDTH_F - w2) * 0.5f, dy + 32.0f,
        220, 220, 220, 220, FALSE, TRUE, 1.0f, 1.0f, mFont_MODE_FONT);

      // s_pc_confirm_delete: 1=No, 2=Yes
      f32 btn_y = dy + 60.0f;
      f32 no_x  = SCREEN_WIDTH_F * 0.5f - 50.0f;
      f32 yes_x = SCREEN_WIDTH_F * 0.5f + 30.0f;
      int no_sel  = (s_pc_confirm_delete == 1);
      int yes_sel = (s_pc_confirm_delete == 2);
      mFont_SetLineStrings(game, no_lbl, sizeof(no_lbl), no_x, btn_y,
        no_sel ? 255 : 150, no_sel ? 255 : 150, no_sel ? 255 : 150,
        no_sel ? 255 : 180, FALSE, TRUE, 1.0f, 1.0f, mFont_MODE_FONT);
      mFont_SetLineStrings(game, yes_lbl, sizeof(yes_lbl), yes_x, btn_y,
        yes_sel ? 255 : 150, yes_sel ? 80 : 150, yes_sel ? 80 : 150,
        yes_sel ? 255 : 180, FALSE, TRUE, 1.0f, 1.0f, mFont_MODE_FONT);
      if (no_sel)  mFont_SetLineStrings(game, str_arrow, 1, no_x - 12.0f, btn_y,
        255, 255, 255, 255, FALSE, TRUE, 1.0f, 1.0f, mFont_MODE_FONT);
      if (yes_sel) mFont_SetLineStrings(game, str_arrow, 1, yes_x - 12.0f, btn_y,
        255, 255, 255, 255, FALSE, TRUE, 1.0f, 1.0f, mFont_MODE_FONT);

      f32 hint_w = (f32)mFont_GetStringWidth(hint, sizeof(hint), TRUE);
      mFont_SetLineStrings(game, hint, sizeof(hint),
        (SCREEN_WIDTH_F - hint_w) * 0.5f, btn_y + 22.0f,
        200, 200, 200, 160, FALSE, TRUE, 1.0f, 1.0f, mFont_MODE_FONT);
    }
  }

#else // PC
  {
    PCSettings* B = &g_pc_options_baseline;

    // Resolution
    len = sprintf(buf, "< %dx%d >", g_pc_settings.window_width, g_pc_settings.window_height);
    { static u8 lbl[] = { 'R', 'e', 's', 'o', 'l', 'u', 't', 'i', 'o', 'n' };
      DRAW_OPT_ROW(lbl, val_x, g_pc_settings.window_width != B->window_width || g_pc_settings.window_height != B->window_height); }

    // Fullscreen
    {
      const char* fs = g_pc_settings.fullscreen == 0 ? "< Windowed >" :
                       g_pc_settings.fullscreen == 1 ? "< Fullscreen >" : "< Borderless >";
      len = sprintf(buf, "%s", fs);
    }
    { static u8 lbl[] = { 'D', 'i', 's', 'p', 'l', 'a', 'y' };
      DRAW_OPT_ROW(lbl, val_x, g_pc_settings.fullscreen != B->fullscreen); }

    // VSync
    len = sprintf(buf, "< %s >", g_pc_settings.vsync ? "On" : "Off");
    { static u8 lbl[] = { 'V', 'S', 'y', 'n', 'c' };
      DRAW_OPT_ROW(lbl, val_x, g_pc_settings.vsync != B->vsync); }

    // MSAA
    if (g_pc_settings.msaa > 0)
      len = sprintf(buf, "< %dx >", g_pc_settings.msaa);
    else
      len = sprintf(buf, "< Off >");
    { static u8 lbl[] = { 'M', 'S', 'A', 'A' };
      DRAW_OPT_ROW(lbl, val_x, g_pc_settings.msaa != B->msaa); }

    // Textures
    {
      const char* tp = g_pc_settings.preload_textures == 0 ? "< On Demand >" :
                       g_pc_settings.preload_textures == 1 ? "< Preload >" : "< Preload&Cache >";
      len = sprintf(buf, "%s", tp);
    }
    { static u8 lbl[] = { 'T', 'e', 'x', 't', 'u', 'r', 'e', 's' };
      DRAW_OPT_ROW(lbl, val_x, g_pc_settings.preload_textures != B->preload_textures); }

    y += line_h * 0.5f;

    // hints
    {
      static u8 str_save[] = "START: Save";
      static u8 str_back[] = "B: Back";
      mFont_SetLineStrings(game, str_save, sizeof(str_save) - 1, x, y, 255, 255, 255, 160, FALSE, TRUE, 1.0f, 1.0f, mFont_MODE_FONT);
      mFont_SetLineStrings(game, str_back, sizeof(str_back) - 1, 190.0f, y, 255, 255, 255, 160, FALSE, TRUE, 1.0f, 1.0f, mFont_MODE_FONT);
    }
  }
#endif

#undef DRAW_OPT_ROW
}

static void aAL_pc_menu_draw(ANIMAL_LOGO_ACTOR* actor, GAME* game) {
  GRAPH* graph = game->graph;
  int td = actor->titledemo_no;

  // reset modelview to identity (title_draw leaves it transformed)
  {
    Gfx* gfx;
    OPEN_DISP(graph);
    gfx = NOW_FONT_DISP;
    gSPMatrix(gfx++, &Mtx_clear, G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
    SET_FONT_DISP(gfx);
    CLOSE_DISP(graph);
  }

  static const u32 sel_r[5] = { 255, 255, 255, 120, 165 };
  static const u32 sel_g[5] = {  90, 135, 100, 205, 245 };
  static const u32 sel_b[5] = {  30,   0, 255, 245,   0 };
  static const u32 dim_r[5] = {  70,  60,  60,  40,  40 };
  static const u32 dim_g[5] = {  40,  50,  40,  50,  50 };
  static const u32 dim_b[5] = {  40,  30,  60,  70,  60 };

  static u8 str_start[] = "Start Game";
  static u8 str_options[] = "Options";

  f32 start_w = (f32)mFont_GetStringWidth(str_start, sizeof(str_start) - 1, TRUE);
  f32 opt_w = (f32)mFont_GetStringWidth(str_options, sizeof(str_options) - 1, TRUE);
  f32 start_x = (SCREEN_WIDTH_F - start_w) * 0.5f;
  f32 opt_x = (SCREEN_WIDTH_F - opt_w) * 0.5f;
  f32 y0 = 135.0f;
  f32 y1 = 153.0f;
  int sel = actor->pc_menu_sel;

  // "Start Game"
  mFont_SetLineStrings(game, str_start, sizeof(str_start) - 1, start_x, y0,
    sel == 0 ? sel_r[td] : dim_r[td],
    sel == 0 ? sel_g[td] : dim_g[td],
    sel == 0 ? sel_b[td] : dim_b[td],
    sel == 0 ? 255 : 160,
    FALSE, TRUE, 1.0f, 1.0f, mFont_MODE_FONT);

  // "Options"
  mFont_SetLineStrings(game, str_options, sizeof(str_options) - 1, opt_x, y1,
    sel == 1 ? sel_r[td] : dim_r[td],
    sel == 1 ? sel_g[td] : dim_g[td],
    sel == 1 ? sel_b[td] : dim_b[td],
    sel == 1 ? 255 : 160,
    FALSE, TRUE, 1.0f, 1.0f, mFont_MODE_FONT);

  // cursor
  {
    f32 arrow_x = (sel == 0 ? start_x : opt_x) - 14.0f;
    f32 arrow_y = sel == 0 ? y0 : y1;
    mFont_SetLineStrings(game, str_arrow, 1, arrow_x, arrow_y,
      sel_r[td], sel_g[td], sel_b[td], 255,
      FALSE, TRUE, 1.0f, 1.0f, mFont_MODE_FONT);
  }

  // options sub-menu overlay
  if (actor->pc_options_open) {
    aAL_pc_options_draw(actor, game);
  }
}
#endif

static int aAL_draw_log_counter = 0;
static void aAL_actor_draw(ACTOR* actor, GAME* game) {
  ANIMAL_LOGO_ACTOR* logo_actor = (ANIMAL_LOGO_ACTOR*)actor;
  GRAPH* graph = game->graph;
  int pad_connected = padmgr_isConnectedController(PAD0);

#ifdef TARGET_PC
  { extern int g_pc_verbose; if (g_pc_verbose && (aAL_draw_log_counter % 60) == 0) {
    printf("[LOGO] draw: action=%d pad_connected=%d back_opacity=%d copyright_opacity=%d press_start_opacity=%.0f\n",
           logo_actor->action, pad_connected, logo_actor->back_opacity, logo_actor->copyright_opacity, logo_actor->press_start_opacity);
  }}
#else
  if ((aAL_draw_log_counter % 60) == 0) {
    printf("[LOGO] draw: action=%d pad_connected=%d back_opacity=%d copyright_opacity=%d press_start_opacity=%.0f\n",
           logo_actor->action, pad_connected, logo_actor->back_opacity, logo_actor->copyright_opacity, logo_actor->press_start_opacity);
  }
#endif
  aAL_draw_log_counter++;

  mFont_SetMatrix(graph, mFont_MODE_FONT);

  if (logo_actor->action >= aAL_ACTION_BACK_FADE_IN) {
    aAL_back_draw(graph, logo_actor);
  }

  aAL_title_draw(game, logo_actor);

  if (logo_actor->action >= aAL_ACTION_START_KEY_CHK_START) {
    aAL_copyright_draw(logo_actor, graph);
    aAL_tm_draw(graph);
  }

  mFont_SetMode(graph, mFont_MODE_FONT);
  if (pad_connected) {
    switch (logo_actor->action) {
      case aAL_ACTION_GAME_START:
      case aAL_ACTION_FADE_OUT_START:
      case aAL_ACTION_OUT:
#ifdef PC_ENHANCEMENTS
        aAL_pc_menu_draw(logo_actor, game);
#else
        aAL_press_start_draw(logo_actor, graph);
#endif
        break;
    }
  }

  mFont_UnSetMatrix(graph, mFont_MODE_FONT);
  game_debug_draw_last(game, graph);
  game_draw_last(graph);
}
