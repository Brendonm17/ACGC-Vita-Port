// vita_mp_nat.c
// a far-away host's port forward (NAT-PMP, then UPnP) and double NAT check, on a worker thread
#include "pc_mp.h"

#ifdef VITA_MP

#include <psp2/kernel/threadmgr.h>
#include <psp2/net/netctl.h>

#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <errno.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MP_NATPMP_PORT   5351
#define MP_SSDP_IP       0xEFFFFFFAu // 239.255.255.250
#define MP_SSDP_PORT     1900
#define MP_LEASE_SEC     7200
#define MP_RENEW_MS      (3000 * 1000)
#define MP_HTTP_MS       3000
#define MP_HTTP_MAX      16384
#define MP_DESCRIPTION   "Animal Crossing"

enum {
    MP_NAT_IDLE,
    MP_NAT_BUSY,
    MP_NAT_MAPPED, // forwarded; ext_ip is public
    MP_NAT_NOMAP,  // the router wouldn't say; a manual forward may still work
    MP_NAT_DOUBLE, // the router's own outside address is private: someone else's NAT is in front
};

enum {
    MP_NAT_VIA_NONE,
    MP_NAT_VIA_PMP,
    MP_NAT_VIA_UPNP,
};

enum {
    MP_NAT_MAP,    // host: forward the port for as long as the line is open
    MP_NAT_LOOKUP, // guest: just the outside address
};

static struct {
    volatile int state;
    volatile int closing;
    volatile int alive; // a worker is still running
    int mode;
    unsigned short port;  // the one asked for, the same outside and in
    unsigned short bound; // the game port already open here
    unsigned short first; // the game's ports a ticket can name
    int count;
    unsigned int ext_ip;
    int via;
    unsigned int gw;
    unsigned int ctl_ip; // UPnP control point
    unsigned short ctl_port;
    char ctl_path[256];
    char service[96];
    char http[MP_HTTP_MAX];
} s_nat;

// sockets

static void mp_nat_addr(struct sockaddr_in* a, unsigned int ip, unsigned short port) {
    memset(a, 0, sizeof(*a));
    a->sin_len = sizeof(*a);
    a->sin_family = AF_INET;
    a->sin_port = htons(port);
    a->sin_addr.s_addr = htonl(ip);
}

static int mp_nat_udp(void) {
    struct sockaddr_in a;
    int s = socket(AF_INET, SOCK_DGRAM, 0);

    if (s < 0) {
        return -1;
    }
    mp_nat_addr(&a, 0, 0);
    if (bind(s, (struct sockaddr*)&a, sizeof(a)) < 0) {
        close(s);
        return -1;
    }
    return s;
}

static int mp_nat_port_free(unsigned short port) {
    struct sockaddr_in a;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    int ok;

    if (s < 0) {
        return 0;
    }
    mp_nat_addr(&a, 0, port);
    ok = bind(s, (struct sockaddr*)&a, sizeof(a)) == 0;
    close(s);
    return ok;
}

// the k-th port to ask the router for: the one already open, then the game's others still free here
static int mp_nat_try(int k) {
    int seen = 0;
    int i;

    s_nat.port = s_nat.bound;
    if (k == 0) {
        return 1;
    }
    for (i = 0; i < s_nat.count; i++) {
        unsigned short p = (unsigned short)(s_nat.first + i);

        if (p != s_nat.bound && ++seen == k) {
            s_nat.port = p;
            return mp_nat_port_free(p);
        }
    }
    return 0;
}

static int mp_nat_wait(int s, int ms, int for_write) {
    fd_set fds;
    struct timeval tv;

    FD_ZERO(&fds);
    FD_SET(s, &fds);
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return select(s + 1, for_write ? NULL : &fds, for_write ? &fds : NULL, NULL, &tv) > 0;
}

static unsigned int mp_nat_rd32(const unsigned char* p) {
    return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) | ((unsigned int)p[2] << 8) | p[3];
}

static void mp_nat_ip_str(unsigned int ip, char* out, int cap) {
    snprintf(out, cap, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
}

// private, shared-address (carrier NAT) or link-local: not an address the internet can reach
static int mp_nat_private(unsigned int ip) {
    return ip == 0 || (ip >> 24) == 10 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8 || (ip >> 22) == 0x191 ||
           (ip >> 16) == 0xA9FE;
}

// NAT-PMP (RFC 6886)

static int mp_natpmp_ask(const unsigned char* req, int req_len, unsigned char* resp, int resp_len, int op) {
    struct sockaddr_in to;
    int s = mp_nat_udp();
    int wait_ms = 250;
    int tries;
    int ok = 0;

    if (s < 0) {
        return 0;
    }
    mp_nat_addr(&to, s_nat.gw, MP_NATPMP_PORT);
    for (tries = 0; tries < 4 && !ok && !s_nat.closing; tries++, wait_ms *= 2) {
        sendto(s, req, req_len, 0, (struct sockaddr*)&to, sizeof(to));
        while (!ok && mp_nat_wait(s, wait_ms, 0)) {
            struct sockaddr_in from;
            socklen_t flen = sizeof(from);
            int n = (int)recvfrom(s, resp, resp_len, 0, (struct sockaddr*)&from, &flen);

            ok = n >= resp_len && ntohl(from.sin_addr.s_addr) == s_nat.gw && resp[0] == 0 && resp[1] == 128 + op &&
                 resp[2] == 0 && resp[3] == 0;
        }
    }
    close(s);
    return ok;
}

static int mp_natpmp_map(unsigned int lifetime) {
    unsigned char req[12];
    unsigned char resp[16];

    memset(req, 0, sizeof(req));
    req[1] = 1; // UDP
    req[4] = (unsigned char)(s_nat.port >> 8);
    req[5] = (unsigned char)s_nat.port;
    req[6] = lifetime ? req[4] : 0;
    req[7] = lifetime ? req[5] : 0;
    req[8] = (unsigned char)(lifetime >> 24);
    req[9] = (unsigned char)(lifetime >> 16);
    req[10] = (unsigned char)(lifetime >> 8);
    req[11] = (unsigned char)lifetime;
    if (!mp_natpmp_ask(req, sizeof(req), resp, sizeof(resp), 1)) {
        return 0;
    }
    // tickets carry the port, so the outside port has to be ours
    return lifetime == 0 || ((resp[10] << 8) | resp[11]) == s_nat.port;
}

static int mp_natpmp_open(void) {
    unsigned char req[2] = { 0, 0 };
    unsigned char resp[12];
    int k;

    if (s_nat.gw == 0 || !mp_natpmp_ask(req, sizeof(req), resp, sizeof(resp), 0)) {
        return 0;
    }
    s_nat.ext_ip = mp_nat_rd32(resp + 8);
    for (k = 0; k < s_nat.count && !s_nat.closing; k++) {
        if (!mp_nat_try(k)) {
            continue;
        }
        if (mp_natpmp_map(MP_LEASE_SEC)) {
            s_nat.via = MP_NAT_VIA_PMP;
            return 1;
        }
        mp_natpmp_map(0); // (given another outside port: that one goes back)
    }
    return 0;
}

// UPnP IGD

// "http://a.b.c.d:port/path" into its pieces; host names aren't worth a resolver here
static int mp_url_split(const char* url, unsigned int* ip, unsigned short* port, char* path, int path_cap) {
    const char* p = url;
    unsigned int v = 0;
    int part;

    if (strncmp(p, "http://", 7) != 0) {
        return 0;
    }
    p += 7;
    *ip = 0;
    for (part = 0; part < 4; part++) {
        if (*p < '0' || *p > '9') {
            return 0;
        }
        for (v = 0; *p >= '0' && *p <= '9'; p++) {
            v = v * 10 + (unsigned)(*p - '0');
        }
        if (v > 255 || (part < 3 && *p++ != '.')) {
            return 0;
        }
        *ip = (*ip << 8) | v;
    }
    *port = 80;
    if (*p == ':') {
        *port = (unsigned short)strtoul(p + 1, (char**)&p, 10);
    }
    snprintf(path, path_cap, "%s", *p == '/' ? p : "/");
    return 1;
}

static const char* mp_find_ci(const char* hay, const char* needle) {
    size_t n = strlen(needle);

    for (; *hay != '\0'; hay++) {
        if (strncasecmp(hay, needle, n) == 0) {
            return hay;
        }
    }
    return NULL;
}

static int mp_upnp_discover(char* location, int cap) {
    static const char* targets[] = {
        "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
        "urn:schemas-upnp-org:service:WANIPConnection:1",
        "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
    };
    struct sockaddr_in to;
    char buf[1024];
    int s = mp_nat_udp();
    int found = 0;
    int i;

    if (s < 0) {
        return 0;
    }
    for (i = 0; i < 3; i++) {
        int len = snprintf(buf, sizeof(buf),
                           "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\n"
                           "ST: %s\r\n\r\n",
                           targets[i]);

        mp_nat_addr(&to, MP_SSDP_IP, MP_SSDP_PORT);
        sendto(s, buf, len, 0, (struct sockaddr*)&to, sizeof(to));
        // some routers only answer a search sent to them directly
        if (s_nat.gw != 0) {
            mp_nat_addr(&to, s_nat.gw, MP_SSDP_PORT);
            sendto(s, buf, len, 0, (struct sockaddr*)&to, sizeof(to));
        }
    }
    while (!found && !s_nat.closing && mp_nat_wait(s, 2500, 0)) {
        int n = (int)recv(s, buf, sizeof(buf) - 1, 0);
        const char* loc;

        if (n <= 0) {
            continue;
        }
        buf[n] = '\0';
        loc = mp_find_ci(buf, "\nLOCATION:");
        if (loc != NULL) {
            int k = 0;

            loc += 10;
            while (*loc == ' ') {
                loc++;
            }
            while (loc[k] != '\0' && loc[k] != '\r' && loc[k] != '\n' && k < cap - 1) {
                location[k] = loc[k];
                k++;
            }
            location[k] = '\0';
            found = k > 0;
        }
    }
    close(s);
    return found;
}

// one request, whole reply; the router closes the connection when it's done
static int mp_http(unsigned int ip, unsigned short port, const char* req, int req_len) {
    struct sockaddr_in to;
    int one = 1;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int got = 0;
    int err = 0;
    socklen_t elen = sizeof(err);
    int sent = 0;

    if (s < 0) {
        return 0;
    }
    setsockopt(s, SOL_SOCKET, SO_NONBLOCK, &one, sizeof(one));
    mp_nat_addr(&to, ip, port);
    if (connect(s, (struct sockaddr*)&to, sizeof(to)) < 0 && errno != EINPROGRESS) {
        close(s);
        return 0;
    }
    if (!mp_nat_wait(s, MP_HTTP_MS, 1) || getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err != 0) {
        close(s);
        return 0;
    }
    while (sent < req_len && mp_nat_wait(s, MP_HTTP_MS, 1)) {
        int n = (int)send(s, req + sent, req_len - sent, 0);

        if (n <= 0) {
            break;
        }
        sent += n;
    }
    while (sent == req_len && got < MP_HTTP_MAX - 1 && mp_nat_wait(s, MP_HTTP_MS, 0)) {
        int n = (int)recv(s, s_nat.http + got, MP_HTTP_MAX - 1 - got, 0);

        if (n <= 0) {
            break;
        }
        got += n;
    }
    close(s);
    s_nat.http[got] = '\0';
    return got;
}

// the WAN connection service in the description and where to send it commands
static int mp_upnp_control(const char* location) {
    static const char* kinds[] = { "WANIPConnection:", "WANPPPConnection:" };
    unsigned int ip;
    unsigned short port;
    char path[256];
    char req[512];
    int k;

    if (!mp_url_split(location, &ip, &port, path, sizeof(path))) {
        return 0;
    }
    snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: %u.%u.%u.%u:%u\r\nConnection: close\r\n\r\n", path, ip >> 24,
             (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF, port);
    if (mp_http(ip, port, req, (int)strlen(req)) <= 0) {
        return 0;
    }
    for (k = 0; k < 2; k++) {
        const char* kind = strstr(s_nat.http, kinds[k]);
        const char* type_begin;
        const char* ctl;
        const char* ctl_end;
        int n;

        if (kind == NULL) {
            continue;
        }
        // back up to the start of the serviceType text, then take its controlURL
        type_begin = kind;
        while (type_begin > s_nat.http && type_begin[-1] != '>') {
            type_begin--;
        }
        n = 0;
        while (type_begin[n] != '<' && type_begin[n] != '\0' && n < (int)sizeof(s_nat.service) - 1) {
            s_nat.service[n] = type_begin[n];
            n++;
        }
        s_nat.service[n] = '\0';
        ctl = strstr(kind, "<controlURL>");
        if (ctl == NULL) {
            continue;
        }
        ctl += 12;
        ctl_end = strstr(ctl, "</controlURL>");
        if (ctl_end == NULL || ctl_end - ctl >= (int)sizeof(s_nat.ctl_path)) {
            continue;
        }
        s_nat.ctl_ip = ip;
        s_nat.ctl_port = port;
        if (strncmp(ctl, "http://", 7) == 0) {
            char full[256];

            memcpy(full, ctl, (size_t)(ctl_end - ctl));
            full[ctl_end - ctl] = '\0';
            if (!mp_url_split(full, &s_nat.ctl_ip, &s_nat.ctl_port, s_nat.ctl_path, sizeof(s_nat.ctl_path))) {
                continue;
            }
        } else {
            snprintf(s_nat.ctl_path, sizeof(s_nat.ctl_path), "%s%.*s", *ctl == '/' ? "" : "/", (int)(ctl_end - ctl),
                     ctl);
        }
        return 1;
    }
    return 0;
}

static int mp_upnp_soap(const char* action, const char* args) {
    char body[1024];
    char req[1536];
    int body_len;
    int len;

    body_len = snprintf(body, sizeof(body),
                        "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:%s xmlns:u=\"%s\">%s"
                        "</u:%s></s:Body></s:Envelope>",
                        action, s_nat.service, args, action);
    len = snprintf(req, sizeof(req),
                   "POST %s HTTP/1.1\r\nHost: %u.%u.%u.%u:%u\r\nContent-Type: text/xml; charset=\"utf-8\"\r\n"
                   "SOAPAction: \"%s#%s\"\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
                   s_nat.ctl_path, s_nat.ctl_ip >> 24, (s_nat.ctl_ip >> 16) & 0xFF, (s_nat.ctl_ip >> 8) & 0xFF,
                   s_nat.ctl_ip & 0xFF, s_nat.ctl_port, s_nat.service, action, body_len, body);
    if (len <= 0 || len >= (int)sizeof(req) || mp_http(s_nat.ctl_ip, s_nat.ctl_port, req, len) < 12) {
        return 0;
    }
    return strncmp(s_nat.http + 9, "200", 3) == 0;
}

// the router's own record of the port: this Vita's, or someone else's that it kept (a router that won't say is taken
// at its word)
static int mp_upnp_ours(void) {
    char args[256];
    char who[20];
    char url[32];
    char path[4];
    const char* c;
    const char* p;
    unsigned int ip;
    unsigned short port;
    int n = 0;

    snprintf(args, sizeof(args),
             "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort><NewProtocol>UDP</NewProtocol>",
             s_nat.port);
    if (!mp_upnp_soap("GetSpecificPortMappingEntry", args) ||
        (c = mp_find_ci(s_nat.http, "NewInternalClient>")) == NULL) {
        return 1;
    }
    c += 18;
    while (*c == ' ') {
        c++;
    }
    while (c[n] != '<' && c[n] != ' ' && c[n] != '\0' && n < (int)sizeof(who) - 1) {
        who[n] = c[n];
        n++;
    }
    who[n] = '\0';
    p = mp_find_ci(s_nat.http, "NewInternalPort>");
    snprintf(url, sizeof(url), "http://%s", who);
    // (only a plain address other than this Vita's counts as someone else's)
    if (!mp_url_split(url, &ip, &port, path, sizeof(path)) ||
        (ip == vita_mp_local_ip_u32() && (p == NULL || atoi(p + 16) == s_nat.port))) {
        return 1;
    }
    pc_mp_log("[MP] UPnP: port %u goes to %s:%d", s_nat.port, who, p != NULL ? atoi(p + 16) : 0);
    return 0;
}

static int mp_upnp_map(unsigned int lease) {
    char args[512];
    char me[20];
    unsigned int local = vita_mp_local_ip_u32();

    mp_nat_ip_str(local, me, sizeof(me));
    snprintf(args, sizeof(args),
             "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort><NewProtocol>UDP</NewProtocol>"
             "<NewInternalPort>%u</NewInternalPort><NewInternalClient>%s</NewInternalClient><NewEnabled>1</NewEnabled>"
             "<NewPortMappingDescription>" MP_DESCRIPTION "</NewPortMappingDescription>"
             "<NewLeaseDuration>%u</NewLeaseDuration>",
             s_nat.port, s_nat.port, me, lease);
    return mp_upnp_soap("AddPortMapping", args);
}

static int mp_upnp_open(void) {
    char location[256];
    const char* ext;
    int k;

    if (!mp_upnp_discover(location, sizeof(location))) {
        pc_mp_log("[MP] UPnP: no router answered the search");
        return 0;
    }
    if (!mp_upnp_control(location)) {
        pc_mp_log("[MP] UPnP: %.120s has no WAN connection service", location);
        return 0;
    }
    // a port the router refuses, or keeps for another device, gives way to the next
    for (k = 0; k < s_nat.count && !s_nat.closing; k++) {
        if (!mp_nat_try(k)) {
            continue;
        }
        // routers that only keep permanent mappings refuse a lease (error 725)
        if (!mp_upnp_map(MP_LEASE_SEC) && !mp_upnp_map(0)) {
            pc_mp_log("[MP] UPnP: port %u refused: %.80s", s_nat.port, s_nat.http);
            continue;
        }
        if (mp_upnp_ours()) {
            break;
        }
    }
    if (k >= s_nat.count || s_nat.closing) {
        return 0;
    }
    s_nat.via = MP_NAT_VIA_UPNP;
    if (mp_upnp_soap("GetExternalIPAddress", "") && (ext = strstr(s_nat.http, "ExternalIPAddress>")) != NULL) {
        char url[48];
        char path[4];
        unsigned short port;

        snprintf(url, sizeof(url), "http://%.24s", ext + 18);
        if (strchr(url, '<') != NULL) {
            *strchr(url, '<') = '\0';
        }
        mp_url_split(url, &s_nat.ext_ip, &port, path, sizeof(path));
    }
    return 1;
}

static void mp_nat_unmap(void) {
    char args[256];

    if (s_nat.via == MP_NAT_VIA_PMP) {
        mp_natpmp_map(0);
    } else if (s_nat.via == MP_NAT_VIA_UPNP) {
        snprintf(args, sizeof(args),
                 "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort><NewProtocol>UDP</NewProtocol>",
                 s_nat.port);
        mp_upnp_soap("DeletePortMapping", args);
    }
    s_nat.via = MP_NAT_VIA_NONE;
}

// the worker

// the Vita's own view of its public address, from its network test, when it has one
static unsigned int mp_nat_system_mapped(void) {
    SceNetCtlNatInfo info;

    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceNetCtlGetNatInfo(&info) < 0) {
        return 0;
    }
    return ntohl(info.mapped_addr.s_addr);
}

static int mp_nat_done(int state) {
    __atomic_store_n(&s_nat.state, state, __ATOMIC_RELEASE);
    s_nat.alive = 0;
    sceKernelExitDeleteThread(0);
    return 0;
}

static int mp_nat_thread(SceSize args, void* argp) {
    unsigned char req[2] = { 0, 0 };
    unsigned char resp[12];
    unsigned int system_ip;
    unsigned int mapped_ms;

    (void)args;
    (void)argp;
    s_nat.gw = vita_mp_netctl_ipv4(SCE_NETCTL_INFO_GET_DEFAULT_ROUTE);
    if (s_nat.mode == MP_NAT_LOOKUP) {
        if (s_nat.gw != 0 && mp_natpmp_ask(req, sizeof(req), resp, sizeof(resp), 0)) {
            s_nat.ext_ip = mp_nat_rd32(resp + 8);
        } else {
            s_nat.ext_ip = mp_nat_system_mapped();
        }
        return mp_nat_done(MP_NAT_NOMAP);
    }
    if (!mp_natpmp_open() && !mp_upnp_open()) {
        system_ip = mp_nat_system_mapped();
        if (s_nat.ext_ip == 0 && !mp_nat_private(system_ip)) {
            s_nat.ext_ip = system_ip;
        }
        pc_mp_log("[MP] no port mapping (gateway %08X)", s_nat.gw);
        return mp_nat_done(MP_NAT_NOMAP);
    }
    system_ip = mp_nat_system_mapped();
    if (s_nat.ext_ip == 0 && system_ip != 0 && !mp_nat_private(system_ip)) {
        s_nat.ext_ip = system_ip; // the router wouldn't say; the Vita's own network test did
    }
    if (s_nat.ext_ip != 0 && (mp_nat_private(s_nat.ext_ip) ||
                              (system_ip != 0 && !mp_nat_private(system_ip) && system_ip != s_nat.ext_ip))) {
        mp_nat_unmap();
        pc_mp_log("[MP] double NAT: far-away hosting unavailable");
        return mp_nat_done(MP_NAT_DOUBLE);
    }
    __atomic_store_n(&s_nat.state, MP_NAT_MAPPED, __ATOMIC_RELEASE);
    // keep the lease alive while the line is open, give the port back when it closes
    mapped_ms = pc_mp_now_ms();
    while (!s_nat.closing) {
        sceKernelDelayThread(200 * 1000);
        if (pc_mp_now_ms() - mapped_ms >= MP_RENEW_MS) {
            mapped_ms = pc_mp_now_ms();
            if (s_nat.via == MP_NAT_VIA_PMP) {
                mp_natpmp_map(MP_LEASE_SEC);
            } else {
                mp_upnp_map(MP_LEASE_SEC);
            }
        }
    }
    mp_nat_unmap();
    return mp_nat_done(MP_NAT_IDLE);
}

static void mp_nat_start(int mode, unsigned short port, unsigned short first, int count) {
    SceUID thread;

    // a worker still giving back an old port finishes first
    while (s_nat.alive) {
        sceKernelDelayThread(10 * 1000);
    }
    memset(s_nat.ctl_path, 0, sizeof(s_nat.ctl_path));
    s_nat.mode = mode;
    s_nat.port = port;
    s_nat.bound = port;
    s_nat.first = first;
    s_nat.count = count > 0 ? count : 1;
    s_nat.ext_ip = 0;
    s_nat.via = MP_NAT_VIA_NONE;
    s_nat.closing = 0;
    s_nat.state = MP_NAT_BUSY;
    s_nat.alive = 1;
    thread = sceKernelCreateThread("ac_mp_nat", mp_nat_thread, 0x10000110, 0x8000, 0, 0, NULL);
    if (thread < 0 || sceKernelStartThread(thread, 0, NULL) < 0) {
        if (thread >= 0) {
            sceKernelDeleteThread(thread);
        }
        s_nat.alive = 0;
        s_nat.state = MP_NAT_NOMAP;
    }
}

void vita_mp_nat_open(unsigned short port, unsigned short first, int count) {
    mp_nat_start(MP_NAT_MAP, port, first, count);
}

// the port the router forwards, once poll says DONE
unsigned short vita_mp_nat_port(void) {
    return s_nat.port;
}

// guest: the outside address of this house, to spot a ticket from under the same roof
void vita_mp_nat_lookup(void) {
    if (!s_nat.alive) {
        mp_nat_start(MP_NAT_LOOKUP, 0, 0, 0);
    }
}

// MP_UI_BUSY while the router is being asked, then DONE (forwarded), NOPORTMAP or CGNAT;
// ext_ip is the outside address when known
int vita_mp_nat_poll(unsigned int* ext_ip) {
    int state = __atomic_load_n(&s_nat.state, __ATOMIC_ACQUIRE);

    *ext_ip = s_nat.ext_ip;
    switch (state) {
        case MP_NAT_BUSY:
            return MP_UI_BUSY;
        case MP_NAT_MAPPED:
            return MP_UI_DONE;
        case MP_NAT_DOUBLE:
            return MP_UI_CGNAT;
        default:
            return MP_UI_NOPORTMAP;
    }
}

// the line closed: the worker gives the port back on its way out, without holding up the game
void vita_mp_nat_close(void) {
    s_nat.closing = 1;
}

int vita_mp_nat_idle(void) {
    return !s_nat.alive;
}

// app exit: the worker gives the port back before the process ends, within reason
void vita_mp_nat_close_wait(int max_ms) {
    int waited;

    s_nat.closing = 1;
    for (waited = 0; s_nat.alive && waited < max_ms; waited += 10) {
        sceKernelDelayThread(10 * 1000);
    }
}

#endif
