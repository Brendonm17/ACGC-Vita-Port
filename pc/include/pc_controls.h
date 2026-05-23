// pc_controls.h
// controller mapping loaded from controls.ini (buttons, sticks, deadzone)

#ifndef PC_CONTROLS_H
#define PC_CONTROLS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Mirrors nofrendo's INP_PAD_* so apply_nes can OR straight into the
// controller state byte.
#define NES_BIT_A      0x01
#define NES_BIT_B      0x02
#define NES_BIT_SELECT 0x04
#define NES_BIT_START  0x08
#define NES_BIT_UP     0x10
#define NES_BIT_DOWN   0x20
#define NES_BIT_LEFT   0x40
#define NES_BIT_RIGHT  0x80
#define NES_TURBO_A    0x01
#define NES_TURBO_B    0x02

typedef enum {
    PCV_CROSS,
    PCV_CIRCLE,
    PCV_SQUARE,
    PCV_TRIANGLE,
    PCV_SELECT,
    PCV_START,
    PCV_L,
    PCV_R,
    PCV_DPAD_UP,
    PCV_DPAD_DOWN,
    PCV_DPAD_LEFT,
    PCV_DPAD_RIGHT,
    // synthesized from stick deflection past digital_threshold
    PCV_LSTICK_UP,
    PCV_LSTICK_DOWN,
    PCV_LSTICK_LEFT,
    PCV_LSTICK_RIGHT,
    PCV_RSTICK_UP,
    PCV_RSTICK_DOWN,
    PCV_RSTICK_LEFT,
    PCV_RSTICK_RIGHT,
    PCV_COUNT
} PCVitaButton;

typedef enum {
    PCA_LSTICK_X,
    PCA_LSTICK_Y,
    PCA_RSTICK_X,
    PCA_RSTICK_Y,
    PCA_COUNT
} PCVitaAxis;

typedef enum {
    PCG_AXIS_NONE = 0,
    PCG_AXIS_MAIN_X,
    PCG_AXIS_MAIN_Y,
    PCG_AXIS_CSTICK_X,
    PCG_AXIS_CSTICK_Y,
} PCGCAxis;

typedef struct {
    PCGCAxis target;
    int8_t   invert; // +1 or -1
} PCAxisBind;

typedef struct {
    uint16_t   main_map[PCV_COUNT];      // PAD_BUTTON_* / PAD_TRIGGER_* bits
    uint8_t    nes_map[PCV_COUNT];       // INP_PAD_* bits
    uint8_t    nes_turbo_map[PCV_COUNT]; // bit0=turbo_a, bit1=turbo_b
    PCAxisBind main_axis_map[PCA_COUNT];
    int        analog_deadzone;
    int        digital_threshold;
} PCControls;

extern PCControls g_pc_controls;

void pc_controls_load(void);
void pc_controls_save(void);

uint16_t pc_controls_apply_main(const uint8_t pressed[PCV_COUNT]);
uint8_t  pc_controls_apply_nes(const uint8_t pressed[PCV_COUNT], int turbo_phase);

// UI helpers for the in-game Controls page.
const char* pc_controls_vita_name(int vbtn);
void        pc_controls_format_main(int vbtn, char* out, int out_size);
void        pc_controls_format_nes(int vbtn, char* out, int out_size);
void        pc_controls_set_main(int vbtn, uint16_t bits);
void        pc_controls_set_nes(int vbtn, uint8_t bits, uint8_t turbo);

int         pc_controls_enforce_confirm_cancel_distinct(uint16_t winning_bit);

#ifdef __cplusplus
}
#endif

#endif
