// vita_main.c - PS Vita entry point
#ifdef TARGET_VITA

#include "pc_platform.h"
#include "pc_gx_internal.h"
#include "pc_settings.h"
#include "pc_keybindings.h"
#include "pc_texture_pack.h"
#include "pc_assets.h"
#include "pc_disc.h"
#include "vita_banner.h"

#include <psp2/kernel/processmgr.h>

extern void ac_entry(void);
extern int boot_main(int argc, const char** argv);
extern void vita_input_init(void);

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    vita_init();
    setvbuf(stdout, NULL, _IONBF, 0);
    g_pc_verbose = 0;

    printf("[VITA] Animal Crossing Vita port starting...\n");

    pc_settings_load();
    banner_scan_folder();
    pc_keybindings_load();
    pc_platform_init();
    pc_texture_pack_init();
    extern void vita_vtc_io_init(void);
    vita_vtc_io_init();
    vita_input_init();

    pc_disc_init();
    pc_assets_init();

    printf("[VITA] Initialization complete, entering game...\n");

    ac_entry();
    boot_main(0, NULL);

    extern void vita_vtc_io_shutdown(void);
    vita_vtc_io_shutdown();
    pc_disc_shutdown();
    pc_platform_shutdown();
    return 0;
}

#endif // TARGET_VITA
