// vita_mp_adhoc.c
// "Right beside me!": the system ad hoc dialog joins both Vitas, then PDP by MAC (rsc-c order)
#include "pc_mp.h"

#ifdef VITA_MP

#include "pc_mp_link.h"

#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/netcheck_dialog.h>
#include <psp2/pspnet_adhoc.h>
#include <psp2/pspnet_adhocctl.h>
#include <psp2/sysmodule.h>

#include <string.h>

#define MP_ADHOC_ID       "ACGC00001" // scopes the ad hoc network to this game
#define MP_ADHOC_PDP_BUF  0x2000
#define MP_WIFI_RETURN_MS 20000 // the Vita needs this long to find its access point again

enum {
    MP_AH_OFF,
    MP_AH_DIALOG, // the system's connection dialog is up
    MP_AH_READY,  // on the ad hoc network
};

static struct {
    int state;
    int apputil_up;
    int lib_up;
    int ctl_up;
    int pdp_game;
    int pdp_disc;
    SceNetAdhocctlGroupName group; // must outlive the dialog
    SceNpCommunicationId np_id;
    unsigned int left_ms;
    int left;
} s_ah = { MP_AH_OFF, 0, 0, 0, -1, -1 };

int vita_mp_dialog_active(void) {
    return s_ah.state == MP_AH_DIALOG;
}

// leaving ad hoc: Wi-Fi comes back on its own, just not instantly
int vita_mp_wifi_recovering(void) {
    return s_ah.left && pc_mp_now_ms() - s_ah.left_ms < MP_WIFI_RETURN_MS && !vita_mp_wifi_connected();
}

static void mp_ah_teardown(void) {
    if (s_ah.state == MP_AH_DIALOG) {
        if (sceNetCheckDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_RUNNING) {
            sceNetCheckDialogAbort();
        }
        sceNetCheckDialogTerm();
    }
    if (s_ah.pdp_game >= 0) {
        sceNetAdhocPdpDelete(s_ah.pdp_game, 0);
    }
    if (s_ah.pdp_disc >= 0) {
        sceNetAdhocPdpDelete(s_ah.pdp_disc, 0);
    }
    s_ah.pdp_game = -1;
    s_ah.pdp_disc = -1;
    if (s_ah.lib_up) {
        // leave the system's ad hoc network too, or the radio stays in ad hoc after we quit
        sceNetCtlAdhocDisconnect();
        if (s_ah.ctl_up) {
            sceNetAdhocctlTerm();
        }
        sceNetAdhocTerm();
        // netctl parks the infrastructure side; re-init starts finding the access point again
        sceNetCtlTerm();
        sceNetCtlInit();
        s_ah.left = 1;
        s_ah.left_ms = pc_mp_now_ms();
    }
    s_ah.lib_up = 0;
    s_ah.ctl_up = 0;
    s_ah.state = MP_AH_OFF;
}

static int mp_ah_begin(void) {
    SceAppUtilInitParam init_param;
    SceAppUtilBootParam boot_param;
    SceNetAdhocctlAdhocId adhoc_id;
    SceNetCheckDialogParam param;
    int ret;

    if (vita_mp_stack_up() < 0) {
        return MP_UI_NOBIND;
    }
    // the connection dialog lives in the app-utility subsystem (else C2-2001-3)
    if (!s_ah.apputil_up) {
        sceSysmoduleLoadModule(SCE_SYSMODULE_APPUTIL);
        memset(&init_param, 0, sizeof(init_param));
        memset(&boot_param, 0, sizeof(boot_param));
        sceAppUtilInit(&init_param, &boot_param);
        s_ah.apputil_up = 1;
    }
    if (sceSysmoduleLoadModule(SCE_SYSMODULE_PSPNET_ADHOC) < 0 || sceNetAdhocInit() < 0) {
        pc_mp_log("[MP] ad hoc init failed");
        return MP_UI_NOBIND;
    }
    s_ah.lib_up = 1;
    memset(&adhoc_id, 0, sizeof(adhoc_id));
    adhoc_id.type = SCE_NET_ADHOCCTL_ADHOCTYPE_RESERVED;
    memcpy(adhoc_id.data, MP_ADHOC_ID, SCE_NET_ADHOCCTL_ADHOCID_LEN);
    ret = sceNetAdhocctlInit(&adhoc_id);
    if (ret < 0) {
        pc_mp_log("[MP] adhocctl init 0x%08X", (unsigned)ret);
        mp_ah_teardown();
        return MP_UI_NOBIND;
    }
    s_ah.ctl_up = 1;

    // CONN makes the system create or join the one network, the same on both Vitas
    memset(&s_ah.group, 0, sizeof(s_ah.group));
    memset(&s_ah.np_id, 0, sizeof(s_ah.np_id));
    memcpy(s_ah.np_id.data, MP_ADHOC_ID, 9);
    sceNetCheckDialogParamInit(&param);
    param.groupName = &s_ah.group;
    param.npCommunicationId = s_ah.np_id;
    param.mode = SCE_NETCHECK_DIALOG_MODE_PSP_ADHOC_CONN;
    param.timeoutUs = 0;
    ret = sceNetCheckDialogInit(&param);
    if (ret < 0) {
        pc_mp_log("[MP] ad hoc dialog 0x%08X", (unsigned)ret);
        mp_ah_teardown();
        return MP_UI_NOBIND;
    }
    s_ah.state = MP_AH_DIALOG;
    return MP_UI_BUSY;
}

// start or advance joining the ad hoc network: BUSY while the dialog is up, then DONE,
// ADHOC_CANCEL when the player backs out, or NOBIND when the system can't
int vita_mp_adhoc_connect(void) {
    SceNetCheckDialogResult result;
    int ret;

    switch (s_ah.state) {
        case MP_AH_READY:
            return MP_UI_DONE;
        case MP_AH_OFF:
            return mp_ah_begin();
        default:
            break;
    }
    if (sceNetCheckDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
        return MP_UI_BUSY;
    }
    memset(&result, 0, sizeof(result));
    ret = sceNetCheckDialogGetResult(&result);
    sceNetCheckDialogTerm();
    s_ah.state = MP_AH_OFF; // the dialog is gone either way
    if (ret < 0 || result.result != 0) {
        mp_ah_teardown();
        return MP_UI_ADHOC_CANCEL;
    }
    s_ah.state = MP_AH_READY;
    return MP_UI_DONE;
}

// PDP transport

static int mp_ah_pdp(unsigned short port) {
    SceNetEtherAddr mac;

    // the adapter's MAC is only right once the dialog has joined the network: read it fresh
    memset(&mac, 0, sizeof(mac));
    sceNetAdhocctlGetEtherAddr(&mac);
    return sceNetAdhocPdpCreate(&mac, port, MP_ADHOC_PDP_BUF, 0);
}

static int mp_ah_send(const mp_addr_t* to, const void* data, int len) {
    int ret;

    if (s_ah.pdp_game < 0 || to->kind != MP_ADDR_MAC) {
        return -1;
    }
    ret = sceNetAdhocPdpSend(s_ah.pdp_game, (const SceNetEtherAddr*)to->mac, to->port, data, len, 0,
                             SCE_NET_ADHOC_F_NONBLOCK);
    return ret < 0 ? -1 : len;
}

static int mp_ah_recv_on(int id, mp_addr_t* from, void* buf, int cap) {
    SceNetEtherAddr mac;
    SceUShort16 port = 0;
    int len = cap;

    if (id < 0 || sceNetAdhocPdpRecv(id, &mac, &port, buf, &len, 0, SCE_NET_ADHOC_F_NONBLOCK) < 0 || len <= 0) {
        return 0;
    }
    memset(from, 0, sizeof(*from));
    from->kind = MP_ADDR_MAC;
    memcpy(from->mac, &mac, 6);
    from->port = port;
    return len;
}

static int mp_ah_recv(mp_addr_t* from, void* buf, int cap) {
    int n = mp_ah_recv_on(s_ah.pdp_disc, from, buf, cap);

    return n > 0 ? n : mp_ah_recv_on(s_ah.pdp_game, from, buf, cap);
}

static int mp_ah_broadcast(uint16_t port, const void* data, int len) {
    mp_addr_t all;

    memset(&all, 0, sizeof(all));
    all.kind = MP_ADDR_MAC;
    memset(all.mac, 0xFF, 6);
    all.port = port;
    return mp_ah_send(&all, data, len);
}

static const mp_transport_t s_ah_transport = { mp_ah_send, mp_ah_recv, mp_ah_broadcast };

// once on the network: the host also answers station queries on the discovery port
const mp_transport_t* vita_mp_adhoc_open(int host, unsigned short disc_port, unsigned short game_port) {
    if (s_ah.state != MP_AH_READY) {
        return NULL;
    }
    if (s_ah.pdp_game < 0) {
        s_ah.pdp_game = mp_ah_pdp(game_port);
    }
    if (host && s_ah.pdp_disc < 0) {
        s_ah.pdp_disc = mp_ah_pdp(disc_port);
    }
    if (s_ah.pdp_game < 0 || (host && s_ah.pdp_disc < 0)) {
        pc_mp_log("[MP] PDP create failed (%d, %d)", s_ah.pdp_game, s_ah.pdp_disc);
        return NULL;
    }
    return &s_ah_transport;
}

void vita_mp_adhoc_close(void) {
    mp_ah_teardown();
}

#endif
