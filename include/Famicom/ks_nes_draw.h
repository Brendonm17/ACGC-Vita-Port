#ifndef KS_NES_DRAW_H
#define KS_NES_DRAW_H

#include "types.h"
#include "Famicom/ks_nes_common.h"

#ifdef __cplusplus
extern "C" {
#endif

extern void ksNesDrawInit(ksNesCommonWorkObj* wp);
extern void ksNesDraw(ksNesCommonWorkObj* wp, ksNesStateObj* sp);
extern void ksNesDrawEnd();

extern u8 ksNesPaletteNormal[];

#ifdef __cplusplus
}
#endif

#endif
