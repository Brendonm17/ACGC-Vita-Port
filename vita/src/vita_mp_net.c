// vita_mp_net.c - network stack bring-up and the UDP transport for multiplayer
#include "pc_mp.h"

#ifdef VITA_MP

#include "pc_mp_link.h"

#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/rng.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <errno.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MP_NET_POOL_SIZE               (1024 * 1024)
#define MP_NETCTL_ERROR_ALREADY_INITED 0x80412102
#define MP_WIFI_WAIT_MS                20000 // a fresh access point association takes 10-15 s

static int s_stack_up = 0;
static SceUID s_watch = -1;
static void* s_net_pool = NULL;
static unsigned int s_wifi_wait_ms; // when the wait for an association began; 0 when not waiting

unsigned int pc_mp_now_ms(void) {
    return (unsigned int)(sceKernelGetProcessTimeWide() / 1000);
}

extern unsigned int _newlib_heap_size_user;

// what malloc could still hand out: heap never claimed plus free chunks inside it
unsigned int vita_mp_heap_free(void) {
    struct mallinfo mi = mallinfo();
    unsigned int arena = (unsigned int)mi.arena;

    return (arena < _newlib_heap_size_user ? _newlib_heap_size_user - arena : 0) + (unsigned int)mi.fordblks;
}

void vita_mp_sleep_ms(int ms) {
    sceKernelDelayThread((SceUInt)ms * 1000);
}

unsigned int pc_mp_random32(void) {
    unsigned int v = 0;

    sceKernelGetRandomNumber(&v, sizeof(v));
    return v;
}

// multiplayer's own memory: with an HD texture pack the game's heap has nothing to spare, so a
// system block taken the first time the network is used holds the stack's pool and every session buffer
#define MP_ARENA_SIZE (3 * 1024 * 1024)

typedef struct mp_blk {
    struct mp_blk* next; // every block, in address order and back to back
    unsigned int size;   // bytes after this header
    unsigned int used;
    unsigned int pad; // payloads stay 16-aligned
} mp_blk_t;

static int s_arena_tried;
static unsigned char* s_arena; // NULL: everything comes from the game's heap as before
static mp_blk_t* s_arena_first;
static volatile char s_arena_lock;

static void mp_arena_lock(void) {
    while (__atomic_test_and_set(&s_arena_lock, __ATOMIC_ACQUIRE)) {
        sceKernelDelayThread(100);
    }
}

static void mp_arena_unlock(void) {
    __atomic_clear(&s_arena_lock, __ATOMIC_RELEASE);
}

static void mp_arena_init(void) {
    SceUID uid;
    void* base = NULL;

    if (s_arena_tried) {
        return;
    }
    s_arena_tried = 1;
    uid = sceKernelAllocMemBlock("ac_mp", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, MP_ARENA_SIZE, NULL);
    if (uid < 0 || sceKernelGetMemBlockBase(uid, &base) < 0 || base == NULL) {
        pc_mp_log("[MP] no system memory for multiplayer (0x%08X), game heap free %u KB", (unsigned)uid,
                  vita_mp_heap_free() / 1024);
        return;
    }
    s_arena = (unsigned char*)base;
    // the stack's pool is the front of the block; the rest is handed out below
    s_arena_first = (mp_blk_t*)(s_arena + MP_NET_POOL_SIZE);
    s_arena_first->next = NULL;
    s_arena_first->size = MP_ARENA_SIZE - MP_NET_POOL_SIZE - sizeof(mp_blk_t);
    s_arena_first->used = 0;
}

static int mp_in_arena(const void* p) {
    return s_arena != NULL && (const unsigned char*)p >= s_arena && (const unsigned char*)p < s_arena + MP_ARENA_SIZE;
}

// first fit; the game's heap only when the block is full
void* mp_alloc(unsigned int n) {
    mp_blk_t* b;
    void* p = NULL;

    mp_arena_init();
    if (s_arena != NULL) {
        n = (n + 15u) & ~15u;
        if (n == 0) {
            n = 16;
        }
        mp_arena_lock();
        for (b = s_arena_first; b != NULL; b = b->next) {
            if (b->used || b->size < n) {
                continue;
            }
            if (b->size >= n + sizeof(mp_blk_t) + 16) {
                mp_blk_t* rest = (mp_blk_t*)((unsigned char*)(b + 1) + n);

                rest->next = b->next;
                rest->size = b->size - n - sizeof(mp_blk_t);
                rest->used = 0;
                b->next = rest;
                b->size = n;
            }
            b->used = 1;
            p = b + 1;
            break;
        }
        mp_arena_unlock();
    }
    return p != NULL ? p : malloc(n);
}

void* mp_calloc(unsigned int count, unsigned int size) {
    void* p = mp_alloc(count * size);

    if (p != NULL) {
        memset(p, 0, count * size);
    }
    return p;
}

void mp_free(void* p) {
    mp_blk_t* b;

    if (p == NULL) {
        return;
    }
    if (!mp_in_arena(p)) {
        free(p);
        return;
    }
    mp_arena_lock();
    ((mp_blk_t*)p - 1)->used = 0;
    for (b = s_arena_first; b != NULL; b = b->next) {
        while (!b->used && b->next != NULL && !b->next->used) {
            b->size += sizeof(mp_blk_t) + b->next->size;
            b->next = b->next->next;
        }
    }
    mp_arena_unlock();
}

void* mp_zalloc(void* opaque, unsigned int items, unsigned int size) {
    (void)opaque;
    return mp_alloc(items * size);
}

void mp_zfree(void* opaque, void* p) {
    (void)opaque;
    mp_free(p);
}

// what a new visitor could still get
unsigned int mp_mem_room(void) {
    mp_blk_t* b;
    unsigned int room = 0;

    mp_arena_init();
    if (s_arena == NULL) {
        return vita_mp_heap_free();
    }
    mp_arena_lock();
    for (b = s_arena_first; b != NULL; b = b->next) {
        if (!b->used) {
            room += b->size;
        }
    }
    mp_arena_unlock();
    return room;
}

// stall watchdog: frames that stop while a line is open leave the game thread's last crumb in
// error.log, written through sceIo so it never waits on a lock the stuck thread may hold
#define MP_STALL_MS   3000
#define MP_STALL_PATH "ux0:data/AnimalCrossing/error.log"

static void mp_stall_note(const char* a, const char* b, unsigned int ms) {
    static const char tag[] = "[MP] ";
    char line[104];
    char digits[12];
    int n = 0;
    int k = 0;
    SceUID fd;

    while (tag[n] != '\0') {
        line[n] = tag[n];
        n++;
    }
    while (*a != '\0' && n < 65) {
        line[n++] = *a++;
    }
    while (b != NULL && *b != '\0' && n < 85) {
        line[n++] = *b++;
    }
    line[n++] = ' ';
    do {
        digits[k++] = (char)('0' + ms % 10);
        ms /= 10;
    } while (ms != 0);
    while (k > 0) {
        line[n++] = digits[--k];
    }
    line[n++] = ' ';
    line[n++] = 'm';
    line[n++] = 's';
    line[n++] = '\n';
    fd = sceIoOpen(MP_STALL_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd >= 0) {
        sceIoWrite(fd, line, n);
        sceIoClose(fd);
    }
}

static int mp_watch_thread(SceSize args, void* argp) {
    unsigned int seen = g_mp_frame;
    unsigned int since = pc_mp_now_ms();
    unsigned int last = since;
    int noted = 0;

    (void)args;
    (void)argp;
    for (;;) {
        unsigned int now;

        sceKernelDelayThread(250 * 1000);
        now = pc_mp_now_ms();
        // moving, no line open, or the whole app was asleep
        if (g_mp_frame != seen || !mp_link_running() || now - last > 1000) {
            if (noted && g_mp_frame != seen) {
                mp_stall_note("moving again after", NULL, now - since);
            }
            seen = g_mp_frame;
            since = now;
            noted = 0;
        } else if (!noted && now - since >= MP_STALL_MS) {
            mp_stall_note("frames stopped at ", g_mp_crumb, now - since);
            noted = 1;
        }
        last = now;
    }
    return 0;
}

// must run before any newlib socket call, which would init a tiny pool of its own
int vita_mp_stack_up(void) {
    SceNetInitParam param;
    int ret;

    if (s_stack_up) {
        return 0;
    }
    if (s_watch < 0) {
        s_watch = sceKernelCreateThread("ac_mp_watch", mp_watch_thread, 0x10000120, 0x2000, 0,
                                        SCE_KERNEL_CPU_MASK_USER_2, NULL);
        if (s_watch >= 0) {
            sceKernelStartThread(s_watch, 0, NULL);
        }
    }

    // an error here can mean the module is already resident; sceNetInit decides
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);

    if (s_net_pool == NULL) {
        mp_arena_init();
        s_net_pool = (s_arena != NULL) ? (void*)s_arena : memalign(64, MP_NET_POOL_SIZE);
        if (s_net_pool == NULL) {
            pc_mp_log("[MP] net: no memory for the stack's pool");
            return -1;
        }
    }

    param.memory = s_net_pool;
    param.size = MP_NET_POOL_SIZE;
    param.flags = 0;
    ret = sceNetInit(&param);
    if (ret < 0) {
        // not taken: already up elsewhere in this process (EBUSY), or a real failure
        if (s_net_pool != (void*)s_arena) {
            free(s_net_pool);
        }
        s_net_pool = NULL;
        if ((unsigned)ret != (unsigned)SCE_NET_ERROR_EBUSY) {
            pc_mp_log("[MP] sceNetInit: 0x%08X", (unsigned)ret);
            return ret;
        }
    }

    // once sceNetInit holds the pool this must not run again, so a netctl error isn't fatal here
    ret = sceNetCtlInit();
    if (ret < 0 && (unsigned)ret != MP_NETCTL_ERROR_ALREADY_INITED) {
        pc_mp_log("[MP] sceNetCtlInit: 0x%08X", (unsigned)ret);
    }

    s_stack_up = 1;
    return 0;
}

int vita_mp_local_ip(char* out, int out_size) {
    SceNetCtlInfo info;
    int ret = sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info);

    if (ret < 0) {
        return ret;
    }
    snprintf(out, out_size, "%s", info.ip_address);
    return 0;
}

int vita_mp_wifi_connected(void) {
    int state = SCE_NETCTL_STATE_DISCONNECTED;

    if (vita_mp_stack_up() < 0 || sceNetCtlInetGetState(&state) < 0) {
        return 0;
    }
    return state == SCE_NETCTL_STATE_CONNECTED;
}

// MP_UI_DONE on Wi-Fi, MP_UI_BUSY while the console associates, MP_UI_NO_WIFI once it won't
int vita_mp_wifi_ready(void) {
    int state = SCE_NETCTL_STATE_DISCONNECTED;
    unsigned int now = pc_mp_now_ms();
    int ret;

    if (vita_mp_stack_up() < 0) {
        return MP_UI_NO_WIFI;
    }
    ret = sceNetCtlInetGetState(&state);
    if (ret < 0) {
        pc_mp_log("[MP] Wi-Fi state unreadable: 0x%08X", (unsigned)ret);
        return MP_UI_NO_WIFI;
    }
    if (state == SCE_NETCTL_STATE_CONNECTED) {
        s_wifi_wait_ms = 0;
        return MP_UI_DONE;
    }
    if (s_wifi_wait_ms == 0) {
        s_wifi_wait_ms = now | 1;
        // an idle netctl never re-associates on its own; restarting it does
        if (state == SCE_NETCTL_STATE_DISCONNECTED) {
            sceNetCtlTerm();
            sceNetCtlInit();
        }
        return MP_UI_BUSY;
    }
    if (now - s_wifi_wait_ms < MP_WIFI_WAIT_MS) {
        return MP_UI_BUSY;
    }
    s_wifi_wait_ms = 0;
    return MP_UI_NO_WIFI;
}

static unsigned int mp_parse_ipv4(const char* s) {
    unsigned int ip = 0;
    int part;

    for (part = 0; part < 4; part++) {
        unsigned int v = 0;

        if (*s < '0' || *s > '9') {
            return 0;
        }
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (unsigned)(*s++ - '0');
        }
        if (v > 255 || (part < 3 && *s++ != '.')) {
            return 0;
        }
        ip = (ip << 8) | v;
    }
    return ip;
}

unsigned int vita_mp_netctl_ipv4(int code) {
    SceNetCtlInfo info;

    if (sceNetCtlInetGetInfo(code, &info) < 0) {
        return 0;
    }
    return mp_parse_ipv4(info.ip_address);
}

unsigned int vita_mp_local_ip_u32(void) {
    return vita_mp_netctl_ipv4(SCE_NETCTL_INFO_GET_IP_ADDRESS);
}

// UDP transport

static int s_sock_game = -1;
static int s_sock_disc = -1;
static unsigned int s_bcast_ip;
static unsigned short s_port_game; // bound ports, taken again after a sleep
static unsigned short s_port_disc;
static int s_udp_open;
static int s_udp_host; // its ports are the ones its ticket and forward name
static int s_reopen;
static unsigned int s_reopen_ms;

static void mp_sockaddr(struct sockaddr_in* a, unsigned int ip, unsigned short port) {
    memset(a, 0, sizeof(*a));
    a->sin_len = sizeof(*a); // sceNetBind rejects a zero length
    a->sin_family = AF_INET;
    a->sin_port = htons(port);
    a->sin_addr.s_addr = htonl(ip);
}

static int mp_udp_socket(unsigned short port) {
    struct sockaddr_in a;
    int one = 1;
    int s = socket(AF_INET, SOCK_DGRAM, 0);

    if (s < 0) {
        pc_mp_log("[MP] socket: errno %d", errno);
        return -1;
    }
    setsockopt(s, SOL_SOCKET, SO_NONBLOCK, &one, sizeof(one));
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    mp_sockaddr(&a, 0, port);
    if (bind(s, (struct sockaddr*)&a, sizeof(a)) < 0) {
        pc_mp_log("[MP] bind port %u: errno %d", (unsigned)port, errno);
        close(s);
        return -1;
    }
    return s;
}

static unsigned short mp_udp_port_of(int s) {
    struct sockaddr_in a;
    socklen_t alen = sizeof(a);

    if (s < 0 || getsockname(s, (struct sockaddr*)&a, &alen) < 0) {
        return 0;
    }
    return ntohs(a.sin_port);
}

static void mp_udp_bcast_refresh(void) {
    unsigned int ip = vita_mp_netctl_ipv4(SCE_NETCTL_INFO_GET_IP_ADDRESS);
    unsigned int mask = vita_mp_netctl_ipv4(SCE_NETCTL_INFO_GET_NETMASK);

    s_bcast_ip = (ip != 0 && mask != 0) ? (ip | ~mask) : 0;
}

static int mp_udp_send(const mp_addr_t* to, const void* data, int len) {
    struct sockaddr_in a;
    int n;

    if (s_sock_game < 0 || to->kind != MP_ADDR_IP) {
        return -1;
    }
    mp_sockaddr(&a, to->ip, to->port);
    MP_CRUMB("sendto");
    n = (int)sendto(s_sock_game, data, len, 0, (struct sockaddr*)&a, sizeof(a));
    MP_CRUMB("sendto done");
    return n;
}

static int mp_udp_recv_on(int s, mp_addr_t* from, void* buf, int cap) {
    struct sockaddr_in a;
    socklen_t alen = sizeof(a);
    int n;

    if (s < 0) {
        return 0;
    }
    MP_CRUMB("recvfrom");
    n = (int)recvfrom(s, buf, cap, 0, (struct sockaddr*)&a, &alen);
    MP_CRUMB("recvfrom done");
    if (n <= 0) {
        return 0;
    }
    memset(from, 0, sizeof(*from));
    from->kind = MP_ADDR_IP;
    from->ip = ntohl(a.sin_addr.s_addr);
    from->port = ntohs(a.sin_port);
    return n;
}

static int mp_udp_recv(mp_addr_t* from, void* buf, int cap) {
    int n = mp_udp_recv_on(s_sock_disc, from, buf, cap);

    return n > 0 ? n : mp_udp_recv_on(s_sock_game, from, buf, cap);
}

// both the limited and the subnet broadcast: routers tend to drop one or the other
static int mp_udp_broadcast(uint16_t port, const void* data, int len) {
    mp_addr_t a;
    int sent = 0;

    memset(&a, 0, sizeof(a));
    a.kind = MP_ADDR_IP;
    a.port = port;
    a.ip = 0xFFFFFFFFu;
    sent += mp_udp_send(&a, data, len) > 0;
    if (s_bcast_ip != 0 && s_bcast_ip != 0xFFFFFFFFu) {
        a.ip = s_bcast_ip;
        sent += mp_udp_send(&a, data, len) > 0;
    }
    return sent > 0 ? len : -1;
}

static const mp_transport_t s_udp_transport = { mp_udp_send, mp_udp_recv, mp_udp_broadcast };

void vita_mp_udp_close(void) {
    if (s_sock_game >= 0) {
        close(s_sock_game);
    }
    if (s_sock_disc >= 0) {
        close(s_sock_disc);
    }
    s_sock_game = -1;
    s_sock_disc = -1;
    s_udp_open = 0;
    s_udp_host = 0;
    s_reopen = 0;
}

// a sleep leaves the sockets dead: the same ports on fresh ones once Wi-Fi is back
void vita_mp_udp_reopen(void) {
    if (s_udp_open) {
        s_reopen = 1;
        s_reopen_ms = pc_mp_now_ms() - 1000;
        vita_mp_udp_tick(pc_mp_now_ms());
    }
}

void vita_mp_udp_tick(unsigned int now_ms) {
    if (!s_reopen || now_ms - s_reopen_ms < 1000 || !vita_mp_wifi_connected()) {
        return;
    }
    s_reopen_ms = now_ms;
    if (s_sock_game >= 0) {
        close(s_sock_game);
    }
    if (s_sock_disc >= 0) {
        close(s_sock_disc);
    }
    // a visitor takes any free port (the Vita never gives an app its old one back from the automatic range)
    s_sock_game = mp_udp_socket(s_udp_host ? s_port_game : 0);
    s_sock_disc = (s_port_disc != 0) ? mp_udp_socket(s_port_disc) : -1;
    if (s_sock_game < 0) {
        return;
    }
    mp_udp_bcast_refresh();
    s_reopen = 0;
}

// host: discovery on disc_port plus the first free game port; guest: any port
const mp_transport_t* vita_mp_udp_open(int host, unsigned short disc_port, unsigned short game_port, int* port_idx) {
    int i;

    vita_mp_udp_close();
    if (vita_mp_stack_up() < 0) {
        return NULL;
    }
    mp_udp_bcast_refresh();

    if (!host) {
        s_sock_game = mp_udp_socket(0);
        s_port_game = mp_udp_port_of(s_sock_game);
        s_port_disc = 0;
        s_udp_open = s_sock_game >= 0;
        return s_sock_game >= 0 ? &s_udp_transport : NULL;
    }
    s_udp_host = 1;

    // ticket play still works when another app holds the discovery port
    s_sock_disc = mp_udp_socket(disc_port);
    for (i = 0; i < 4 && s_sock_game < 0; i++) {
        s_sock_game = mp_udp_socket((unsigned short)(game_port + i));
        if (s_sock_game >= 0 && port_idx != NULL) {
            *port_idx = i;
        }
    }
    if (s_sock_game < 0) {
        vita_mp_udp_close();
        return NULL;
    }
    s_port_game = mp_udp_port_of(s_sock_game);
    s_port_disc = (s_sock_disc >= 0) ? disc_port : 0;
    s_udp_open = 1;
    return &s_udp_transport;
}

// host: the game port moves to another the router forwards, before any line is open on it
int vita_mp_udp_move_game(unsigned short port) {
    int s;

    if (!s_udp_host || s_sock_game < 0) {
        return 0;
    }
    s = mp_udp_socket(port);
    if (s < 0) {
        return 0;
    }
    close(s_sock_game);
    s_sock_game = s;
    s_port_game = port;
    return 1;
}

#endif
