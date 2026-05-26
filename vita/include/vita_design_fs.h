// vita_design_fs.h
// scan / import / export of design images under ux0:data/AnimalCrossing/designs/
#ifndef VITA_DESIGN_FS_H
#define VITA_DESIGN_FS_H

#include "vita_design_convert.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DESIGN_FS_MAX_FILES 256
#define DESIGN_FS_NAME_MAX  64

// Rescan the designs folder for importable images (png/bmp/jpg/tga).
void        design_fs_scan(void);
int         design_fs_count(void);
const char* design_fs_filename(int i); // raw filename, "" if out of range

// Decode file i and convert it to a design. name_ascii receives the sanitized
// stem. Returns 0 on success, negative on bad index / decode / convert failure.
int design_fs_import(int i, int dither, vdc_design_t* out, char* name_ascii, int name_cap, long* score);

// Write a design as a PNG into the designs folder (auto-suffixes on collision). Returns 0 on success.
int design_fs_export(const uint8_t* tex, int palette, const char* name_ascii);

#ifdef __cplusplus
}
#endif

#endif
