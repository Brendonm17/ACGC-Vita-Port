// vita_frame.h
// per-frame orchestration: buffer swap, worker dispatch, submit, perf logging
#ifndef VITA_FRAME_H
#define VITA_FRAME_H

#ifdef TARGET_VITA

#include "sys_ucode.h"

// run one full frame of the Vita render pipeline
// ucode: 2-entry array (poly_text, sprite_text), gfx_list: the display list to process
void vita_frame_run(ucode_info* ucode, void* gfx_list);

// block until the emu64 worker finishes (call before clearing sys_dynamic)
void vita_frame_wait_worker(void);

#endif // TARGET_VITA
#endif // VITA_FRAME_H
