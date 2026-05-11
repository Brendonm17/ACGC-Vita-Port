/* pc_pad.c - GC controller input via SDL gamepad + keyboard */
#include "pc_platform.h"
#include "pc_typing.h"
#include "pc_keybindings.h"
#include "pc_controls.h"
#include <dolphin/pad.h>

// latest raw vita button bitmap, exposed for the nes emulator
uint8_t g_pc_vita_pressed[PCV_COUNT] = {0};

/* analog stick constants */
#define STICK_MAGNITUDE     80
#define TRIGGER_THRESHOLD   100
#define RUMBLE_DURATION_MS  200

static SDL_GameController* g_controller = NULL;

BOOL PADInit(void) {
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) {
            g_controller = SDL_GameControllerOpen(i);
            if (g_controller) {
                break;
            }
        }
    }
    return TRUE;
}

u32 PADRead(PADStatus* status) {
    memset(status, 0, sizeof(PADStatus) * 4);
    memset(g_pc_vita_pressed, 0, sizeof(g_pc_vita_pressed));

    const u8* keys = SDL_GetKeyboardState(NULL);
    u32 mouse = SDL_GetMouseState(NULL, NULL);
    u16 buttons = 0;
    s8 stickX = 0, stickY = 0;
    s8 cstickX = 0, cstickY = 0;

    /* Suppress keyboard-to-button mapping when typing into the in-game text editor */
    if (!(g_pc_typing_mode && g_pc_editor_active)) {
        /* helper: check if a PCInputCode is currently pressed */
        #define INPUT_PRESSED(code) \
            (((code) & PC_INPUT_MOUSE_BIT) \
                ? (mouse & SDL_BUTTON((code) & 0xFF)) \
                : keys[(SDL_Scancode)(code)])

        /* buttons (from keybindings.ini) */
        PCKeybindings* kb = &g_pc_keybindings;
        if (INPUT_PRESSED(kb->a))     buttons |= PAD_BUTTON_A;
        if (INPUT_PRESSED(kb->b))     buttons |= PAD_BUTTON_B;
        if (INPUT_PRESSED(kb->x))     buttons |= PAD_BUTTON_X;
        if (INPUT_PRESSED(kb->y))     buttons |= PAD_BUTTON_Y;
        if (INPUT_PRESSED(kb->start)) buttons |= PAD_BUTTON_START;
        if (INPUT_PRESSED(kb->z))     buttons |= PAD_TRIGGER_Z;
        if (INPUT_PRESSED(kb->l))     buttons |= PAD_TRIGGER_L;
        if (INPUT_PRESSED(kb->r))     buttons |= PAD_TRIGGER_R;

        /* main stick */
        if (INPUT_PRESSED(kb->stick_up))    stickY += STICK_MAGNITUDE;
        if (INPUT_PRESSED(kb->stick_down))  stickY -= STICK_MAGNITUDE;
        if (INPUT_PRESSED(kb->stick_left))  stickX -= STICK_MAGNITUDE;
        if (INPUT_PRESSED(kb->stick_right)) stickX += STICK_MAGNITUDE;

        /* C-stick */
        if (INPUT_PRESSED(kb->cstick_up))    cstickY += STICK_MAGNITUDE;
        if (INPUT_PRESSED(kb->cstick_down))  cstickY -= STICK_MAGNITUDE;
        if (INPUT_PRESSED(kb->cstick_left))  cstickX -= STICK_MAGNITUDE;
        if (INPUT_PRESSED(kb->cstick_right)) cstickX += STICK_MAGNITUDE;

        /* D-pad */
        if (INPUT_PRESSED(kb->dpad_up))    buttons |= PAD_BUTTON_UP;
        if (INPUT_PRESSED(kb->dpad_down))  buttons |= PAD_BUTTON_DOWN;
        if (INPUT_PRESSED(kb->dpad_left))  buttons |= PAD_BUTTON_LEFT;
        if (INPUT_PRESSED(kb->dpad_right)) buttons |= PAD_BUTTON_RIGHT;

        #undef INPUT_PRESSED
    }

    /* hotplug */
    if (!g_controller) {
        for (int i = 0; i < SDL_NumJoysticks(); i++) {
            if (SDL_IsGameController(i)) {
                g_controller = SDL_GameControllerOpen(i);
                if (g_controller) break;
            }
        }
    }

    if (g_controller) {
        if (!SDL_GameControllerGetAttached(g_controller)) {
            SDL_GameControllerClose(g_controller);
            g_controller = NULL;
        }
    }
    if (g_controller) {
        uint8_t pressed[PCV_COUNT] = {0};
        pressed[PCV_CROSS]      = SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_A);
        pressed[PCV_CIRCLE]     = SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_B);
        pressed[PCV_SQUARE]     = SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_X);
        pressed[PCV_TRIANGLE]   = SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_Y);
        pressed[PCV_START]      = SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_START);
        pressed[PCV_SELECT]     = SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_BACK);
        pressed[PCV_L]          = SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
        pressed[PCV_R]          = SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
        pressed[PCV_DPAD_UP]    = SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_DPAD_UP);
        pressed[PCV_DPAD_DOWN]  = SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        pressed[PCV_DPAD_LEFT]  = SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
        pressed[PCV_DPAD_RIGHT] = SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);

        s16 sdl_axis[PCA_COUNT];
        sdl_axis[PCA_LSTICK_X] = SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_LEFTX);
        sdl_axis[PCA_LSTICK_Y] = SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_LEFTY);
        sdl_axis[PCA_RSTICK_X] = SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_RIGHTX);
        sdl_axis[PCA_RSTICK_Y] = SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_RIGHTY);

        // synth virtual buttons from stick deflection (sdl Y is positive-down)
        const int dthr = g_pc_controls.digital_threshold;
        pressed[PCV_LSTICK_LEFT]  = (sdl_axis[PCA_LSTICK_X] < -dthr);
        pressed[PCV_LSTICK_RIGHT] = (sdl_axis[PCA_LSTICK_X] >  dthr);
        pressed[PCV_LSTICK_UP]    = (sdl_axis[PCA_LSTICK_Y] < -dthr);
        pressed[PCV_LSTICK_DOWN]  = (sdl_axis[PCA_LSTICK_Y] >  dthr);
        pressed[PCV_RSTICK_LEFT]  = (sdl_axis[PCA_RSTICK_X] < -dthr);
        pressed[PCV_RSTICK_RIGHT] = (sdl_axis[PCA_RSTICK_X] >  dthr);
        pressed[PCV_RSTICK_UP]    = (sdl_axis[PCA_RSTICK_Y] < -dthr);
        pressed[PCV_RSTICK_DOWN]  = (sdl_axis[PCA_RSTICK_Y] >  dthr);

        memcpy(g_pc_vita_pressed, pressed, sizeof(pressed));
        buttons |= pc_controls_apply_main(pressed);

        const int adz = g_pc_controls.analog_deadzone;
        s8 gc_axis_out[4] = {0, 0, 0, 0}; // main_x, main_y, cstick_x, cstick_y
        for (int i = 0; i < PCA_COUNT; i++) {
            const PCAxisBind* b = &g_pc_controls.main_axis_map[i];
            if (b->target == PCG_AXIS_NONE) continue;
            s16 raw = sdl_axis[i];
            if (abs(raw) <= adz) continue;
            int v = raw >> 8;
            // sdl Y is positive-down, GC is positive-up
            if (i == PCA_LSTICK_Y || i == PCA_RSTICK_Y) v = -v;
            if (b->invert < 0) v = -v;
            if (v > 127) v = 127; else if (v < -128) v = -128;
            gc_axis_out[b->target - PCG_AXIS_MAIN_X] = (s8)v;
        }
        stickX  = gc_axis_out[0];
        stickY  = gc_axis_out[1];
        cstickX = gc_axis_out[2];
        cstickY = gc_axis_out[3];

        u8 lt = (u8)(SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7);
        u8 rt = (u8)(SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);
        if (lt > TRIGGER_THRESHOLD) buttons |= PAD_TRIGGER_L;
        if (rt > TRIGGER_THRESHOLD) buttons |= PAD_TRIGGER_R;
        status[0].triggerLeft = lt;
        status[0].triggerRight = rt;
    }

    status[0].button = buttons;
    status[0].stickX = stickX;
    status[0].stickY = stickY;
    status[0].substickX = cstickX;
    status[0].substickY = cstickY;
    status[0].err = 0; /* PAD_ERR_NONE */

    return PAD_CHAN0_BIT; /* Controller 1 connected */
}

void PADControlMotor(s32 chan, u32 command) {
    if (g_controller && chan == 0) {
        u16 intensity = (command == 1) ? 0xFFFF : 0;
        SDL_GameControllerRumble(g_controller, intensity, intensity, RUMBLE_DURATION_MS);
    }
}

void PADControlAllMotors(const u32* commands) {
    PADControlMotor(0, commands[0]);
}

void PADCleanup(void) {
    if (g_controller) {
        SDL_GameControllerClose(g_controller);
        g_controller = NULL;
    }
}

BOOL PADReset(u32 mask) { (void)mask; return TRUE; }
BOOL PADRecalibrate(u32 mask) { (void)mask; return TRUE; }
BOOL PADSync(void) { return TRUE; }
void PADSetSpec(u32 spec) { (void)spec; }
void PADSetAnalogMode(u32 mode) { (void)mode; }
/* PADClamp compiled from decomp: src/static/dolphin/pad/Padclamp.c */
BOOL PADGetType(s32 chan, u32* type) { if (type) *type = 0x09000000; return TRUE; }
