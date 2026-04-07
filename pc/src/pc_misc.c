/* pc_misc.c - hardware register arrays, EXI/SI stubs, PPC stubs, misc */
#include "pc_platform.h"

/* --- HW register arrays --- */
volatile u16 __VIRegs[59]    = {0};
volatile u32 __PIRegs[12]    = {0};
volatile u16 __MEMRegs[64]   = {0};
volatile u16 __DSPRegs[32]   = {0};
volatile u32 __DIRegs[16]    = {0};
volatile u32 __SIRegs[0x100] = {0};
volatile u32 __EXIRegs[0x40] = {0};
volatile u32 __AIRegs[8]     = {0};

u32 __OSPhysicalMemSize = 24 * 1024 * 1024;
volatile int __OSTVMode = 0;
u32 __OSSimulatedMemSize = 24 * 1024 * 1024;
u32 __OSBusClock = 162000000;
u32 __OSCoreClock = 486000000;
volatile u16 __OSDeviceCode = 0;

/* --- EXI --- */
BOOL EXILock(s32 chan, u32 dev, void* unlockCallback) {
    (void)chan; (void)dev; (void)unlockCallback;
    return TRUE;
}
BOOL EXIUnlock(s32 chan) { (void)chan; return TRUE; }
BOOL EXISelect(s32 chan, u32 dev, u32 freq) { (void)chan; (void)dev; (void)freq; return TRUE; }
BOOL EXIDeselect(s32 chan) { (void)chan; return TRUE; }
BOOL EXIImm(s32 chan, void* data, s32 len, u32 type, void* callback) {
    (void)chan; (void)data; (void)len; (void)type; (void)callback;
    return TRUE;
}
BOOL EXIDma(s32 chan, void* data, s32 len, u32 type, void* callback) {
    (void)chan; (void)data; (void)len; (void)type; (void)callback;
    return TRUE;
}
BOOL EXISync(s32 chan) { (void)chan; return TRUE; }
void EXIInit(void) {}
BOOL EXIAttach(s32 chan, void* extCallback) { (void)chan; (void)extCallback; return TRUE; }
BOOL EXIDetach(s32 chan) { (void)chan; return TRUE; }
BOOL EXIProbe(s32 chan) { (void)chan; return FALSE; }
BOOL EXIProbeEx(s32 chan) { (void)chan; return FALSE; }
s32  EXIGetID(s32 chan, u32 dev, u32* id) { if (id) *id = 0; return 0; }
void EXISetExiCallback(s32 chan, void* cb) { (void)chan; (void)cb; }

/* --- SI --- */
void SIInit(void) {}
u32  SIGetType(s32 chan) { return 0x09000000; /* standard controller */ }
u32  SIGetTypeAsync(s32 chan, void* callback) { return SIGetType(chan); }
BOOL SITransfer(s32 chan, void* output, u32 outputLen, void* input, u32 inputLen,
                void* callback, s64 time) {
    (void)chan; (void)output; (void)outputLen; (void)input; (void)inputLen;
    (void)callback; (void)time;
    return TRUE;
}
u32  SISetXY(u32 x, u32 y) { (void)x; (void)y; return 0; }
u32  SIEnablePolling(u32 poll) { (void)poll; return 0; }
u32  SIDisablePolling(u32 poll) { (void)poll; return 0; }
void SISetSamplingRate(u32 rate) { (void)rate; }
BOOL SIIsChanBusy(s32 chan) { return FALSE; }
void SIRefreshSamplingRate(void) {}
BOOL SIRegisterPollingHandler(void* handler) { (void)handler; return TRUE; }
BOOL SIUnregisterPollingHandler(void* handler) { (void)handler; return TRUE; }

/* --- PPC --- */
u32 PPCMfmsr(void) { return 0; }
void PPCMtmsr(u32 msr) { (void)msr; }
u32 PPCMfhid0(void) { return 0; }
void PPCMthid0(u32 hid0) { (void)hid0; }
u32 PPCMfhid2(void) { return 0; }
void PPCMthid2(u32 hid2) { (void)hid2; }
void PPCHalt(void) { exit(1); }
u32 PPCMfl2cr(void) { return 0; }
void PPCMtl2cr(u32 val) { (void)val; }
void PPCMtdec(u32 val) { (void)val; }
void PPCSync(void) {}
void PPCMtmmcr0(u32 val) { (void)val; }
void PPCMtmmcr1(u32 val) { (void)val; }
void PPCMtpmc1(u32 val) { (void)val; }
void PPCMtpmc2(u32 val) { (void)val; }
void PPCMtpmc3(u32 val) { (void)val; }
void PPCMtpmc4(u32 val) { (void)val; }
u32 PPCMfpmc1(void) { return 0; }
u32 PPCMfpmc2(void) { return 0; }
u32 PPCMfpmc3(void) { return 0; }
u32 PPCMfpmc4(void) { return 0; }

/* --- Debugger --- */
void DBInit(void) {}
BOOL DBIsDebuggerPresent(void) { return FALSE; }
void DBPrintf(const char* fmt, ...) {
    if (!g_pc_verbose) return;
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

/* --- OS reset --- */
typedef BOOL (*OSResetFunction)(BOOL final);
typedef struct OSResetFunctionInfo {
    OSResetFunction func;
    u32 priority;
    struct OSResetFunctionInfo* next;
    struct OSResetFunctionInfo* prev;
} OSResetFunctionInfo;

void OSRegisterResetFunction(OSResetFunctionInfo* info) { (void)info; }
void OSUnregisterResetFunction(OSResetFunctionInfo* info) { (void)info; }

/* --- OS modules --- */
typedef struct OSModuleInfo {
    u32 id;
    void* link_next;
    void* link_prev;
    u32 numSections;
    u32 sectionInfoOffset;
    u32 nameOffset;
    u32 nameSize;
    u32 version;
} OSModuleInfo;

typedef struct OSModuleHeader {
    OSModuleInfo info;
    u32 bssSize;
    u32 relOffset;
    u32 impOffset;
    u32 impSize;
    u8  prologSection;
    u8  epilogSection;
    u8  unresolvedSection;
    u8  bssSection;
    u32 prolog;
    u32 epilog;
    u32 unresolved;
} OSModuleHeader;

OSModuleHeader* BaseModule = NULL;

struct { void* head; void* tail; } __OSModuleList = {0, 0};
void* __OSStringTable = NULL;

u8 DiskID[32] = {0};

BOOL OSLink(void* info, void* bss) { (void)info; (void)bss; return TRUE; }
BOOL OSUnlink(void* info) { (void)info; return TRUE; }

/* --- Misc --- */

/* bzero/bcopy: Windows CRT doesn't have these, Linux glibc does */
#ifdef _WIN32
void bzero(void* s, unsigned int n) { memset(s, 0, n); }
void bcopy(const void* src, void* dst, unsigned int n) { memmove(dst, src, n); }
#endif

u8 GXNtsc480IntDf[64] = {0};
u8 GXNtsc480Int[64] = {0};
u8 GXMpal480IntDf[64] = {0};
u8 GXPal528IntDf[64] = {0};
u8 GXEurgb60Hz480IntDf[64] = {0};

void* __gUnkThread1 = NULL;
void* __gCurrentThread = NULL;
s32 __gUnknown800030C0[2] = {0};
u8 __gUnknown800030E3 = 0;

void OSInitFastCast(void) {}
void __OSSetupFPU(void) {}

void __sync(void) {}
void __isync(void) {}

void InitMetroTRK(void) {}
void InitMetroTRK_BBA(void) {}

/* --- N64 fixed-point trig --- */
#include <math.h>
short sins(unsigned short angle) {
    double rad = (double)angle * (2.0 * PC_PI / 65536.0);
    return (short)(sin(rad) * 32767.0);
}

short coss(unsigned short angle) {
    double rad = (double)angle * (2.0 * PC_PI / 65536.0);
    return (short)(cos(rad) * 32767.0);
}

void __OSUnhandledException(u8 type, void* ctx, u32 dsisr, u32 dar) {
    (void)type; (void)ctx; (void)dsisr; (void)dar;
    fprintf(stderr, "Unhandled exception type %d\n", type);
}

/* --- libc64 malloc replacement (PC uses system malloc) --- */
/* game's own malloc arena -we just wrap system malloc */

static int malloc_initialized = 0;
static void* malloc_arena_base = NULL;
static unsigned long malloc_arena_size = 0;

void MallocInit(void* base, unsigned long size) {
    malloc_initialized = 1;
    malloc_arena_base = base;
    malloc_arena_size = size;
}

void MallocCleanup(void) {
    malloc_initialized = 0;
}

int MallocIsInitalized(void) {
    return malloc_initialized;
}

void GetFreeArena(unsigned long* max, unsigned long* free_size, unsigned long* alloc) {
    if (max) *max = malloc_arena_size;
    if (free_size) *free_size = malloc_arena_size;
    if (alloc) *alloc = 0;
}

void DisplayArena(void) { }

int CheckArena(void) { return 0; }
