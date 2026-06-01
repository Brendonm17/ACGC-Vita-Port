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
#include "vita_trophy.h"

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
    pc_controls_load();
    pc_platform_init();

    // surface any fatal recorded by vita_init (runs before vitaGL) now that
    // the dialog can render.
    {
        extern void vita_fatal_dialog_and_exit(const char* msg);
        extern const char* vita_get_early_fatal(void);
        const char* early_fatal = vita_get_early_fatal();
        if (early_fatal) vita_fatal_dialog_and_exit(early_fatal);
    }

    pc_texture_pack_init();
    extern void vita_vtc_io_init(void);
    vita_vtc_io_init();
    vita_input_init();

    // surface the missing-rom case up front; pc_assets_init crashes later
    // without a disc image and the user has no idea why.
    if (!pc_disc_init()) {
        extern void vita_fatal_dialog_and_exit(const char* msg);
        vita_fatal_dialog_and_exit(
            "Animal Crossing ROM not found.\n\n"
            "Please place an Animal Crossing (USA) disc image\n"
            "(.iso, .ciso, or .gcm) at:\n\n"
            "ux0:data/AnimalCrossing/rom/");
    }
    pc_assets_init();

    vita_trophy_init();

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
