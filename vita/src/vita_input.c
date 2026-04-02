// vita_input.c - vita-specific input setup
// SDL2 handles most of it via pc_pad.c, this is just SceCtrl/touch init
#ifdef TARGET_VITA

#include "pc_platform.h"
#include <psp2/ctrl.h>
#include <psp2/touch.h>

void vita_input_init(void) {
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
}


#endif // TARGET_VITA
