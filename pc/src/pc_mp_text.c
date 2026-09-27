// pc_mp_text.c - multiplayer dialogue served beside the ROM message archive
#include "pc_mp.h"

#ifdef VITA_MP

#define PC_MP_TEXT_DATA_IMPL
#include "pc_mp_text_data.h"

#include <string.h>

int pc_mp_text_is_msg(int idx) {
    return idx >= MP_VMSG_BASE && idx < MP_VMSG_BASE + MP_VMSG_COUNT;
}

int pc_mp_text_load_msg(unsigned char* dst, int cap, int idx) {
    unsigned int start;
    unsigned int len;

    if (!pc_mp_text_is_msg(idx)) {
        return 0;
    }
    start = mp_vmsg_ofs[idx - MP_VMSG_BASE];
    len = mp_vmsg_ofs[idx - MP_VMSG_BASE + 1] - start;
    if ((int)len > cap) {
        return 0;
    }
    memcpy(dst, mp_vmsg_bytes + start, len);
    return (int)len;
}

// station announcements use the conductor's voice
int pc_mp_text_is_pa(int idx) {
    return pc_mp_text_is_msg(idx) && (mp_vmsg_flags[idx - MP_VMSG_BASE] & 1);
}

int pc_mp_text_is_choice(int idx) {
    return idx >= MP_VCH_BASE && idx < MP_VCH_BASE + MP_VCH_COUNT;
}

void pc_mp_text_load_choice(unsigned char* dst16, int idx) {
    memcpy(dst16, mp_vch_labels[idx - MP_VCH_BASE], sizeof(mp_vch_labels[0]));
}

static int mp_text_find(const unsigned char* p, int size, const unsigned char* pat, int n) {
    int k;

    for (k = 0; k + n <= size; k++) {
        if (memcmp(p + k, pat, n) == 0) {
            return k;
        }
    }
    return -1;
}

// ROM lines a visitor sees differently: Wisp's reward menu without the roof (it has none here to paint), leaving
// weeds or an item
void pc_mp_text_patch(int idx, unsigned char* p, int size) {
    static const unsigned char sel3[] = { 0x7F, 0x17, 0x01, 0xD6, 0x01, 0xD7, 0x01, 0xD8 }; // three labels
    static const unsigned char next1[] = { 0x7F, 0x10, 0x2E, 0xF7 }; // the second: the roof
    static const unsigned char next2[] = { 0x7F, 0x11, 0x2E, 0xF9 }; // the third: the item
    int k;

    if (idx != 0x2EF0 || !mp_visitor_rights() || (k = mp_text_find(p, size, sel3, sizeof(sel3))) < 0) {
        return;
    }
    p[k + 1] = 0x16; // two labels
    p[k + 5] = 0xD8;
    memmove(p + k + 6, p + k + 8, size - (k + 8));
    size -= 2;
    if ((k = mp_text_find(p, size, next1, sizeof(next1))) >= 0) {
        p[k + 3] = 0xF9;
    }
    if ((k = mp_text_find(p, size, next2, sizeof(next2))) >= 0) {
        memmove(p + k, p + k + 4, size - (k + 4));
    }
}

unsigned int pc_mp_text_hash(void) {
    return MP_TEXT_HASH;
}

#endif
