#!/bin/bash
# build + deploy to Vita via VitaCompanion
# usage: ./deploy.sh [full|deploy|build|kill|launch]
set -e

VITA_IP="${VITA_IP:-192.168.0.148}"
VITA_FTP_PORT=1337
VITA_CMD_PORT=1338
TITLE_ID="ACGC00001"

VITASDK=/usr/local/vitasdk
export VITASDK
export PATH=$VITASDK/bin:$PATH

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WIN_SRC="${WIN_SRC:-$(cd "$SCRIPT_DIR/.." && pwd)}"
EXT4_SRC="${EXT4_SRC:-$HOME/ac_vita_build/ACGC-PC-Port}"
EXT4_BUILD="$EXT4_SRC/vita/build"
NPROC=$(nproc)

export CCACHE_DIR="$HOME/.ccache_vita"
export CCACHE_MAXSIZE="4G"
export CCACHE_COMPRESS=1
export CCACHE_COMPRESSLEVEL=6

check_vita_ip() {
    if [[ "$VITA_IP" == *"XXX"* ]]; then
        echo "ERROR: Set VITA_IP (env var or edit this script)"
        exit 1
    fi
}

vita_cmd() {
    local response
    response=$(echo "$1" | nc -w 3 "${VITA_IP}" "${VITA_CMD_PORT}" 2>&1) || {
        echo "WARNING: Can't reach Vita at ${VITA_IP}:${VITA_CMD_PORT}"
        return 1
    }
    echo "$response"
}

vita_kill() {
    echo "Killing app..."
    vita_cmd "destroy" || true
    sleep 1
}

vita_launch() {
    echo "Launching $TITLE_ID..."
    vita_cmd "launch ${TITLE_ID}"
}

ftp_upload() {
    local local_file="$1"
    local remote_path="$2"
    local filesize
    filesize=$(stat -c%s "$local_file" 2>/dev/null || stat -f%z "$local_file" 2>/dev/null)
    local size_mb
    size_mb=$(echo "scale=2; $filesize / 1048576" | bc 2>/dev/null || echo "?")

    echo "  Uploading $(basename "$local_file") (${size_mb} MB)..."
    curl -s --connect-timeout 5 --max-time 120 \
        -T "$local_file" \
        "ftp://${VITA_IP}:${VITA_FTP_PORT}/${remote_path}" || {
        echo "ERROR: FTP upload failed"
        return 1
    }
}

sync_sources() {
    echo "Syncing to ext4..."
    mkdir -p "$EXT4_SRC"
    rsync -a --delete \
        --exclude='vita/build/' --exclude='pc/build*/' --exclude='.git/' \
        --exclude='*.o' --exclude='*.obj' --exclude='*.vpk' \
        --exclude='*.elf' --exclude='*.velf' --exclude='*.bin' \
        "$WIN_SRC/" "$EXT4_SRC/"
}

do_build() {
    sync_sources
    mkdir -p "$EXT4_BUILD"
    cd "$EXT4_BUILD"
    [ ! -f Makefile ] && cmake .. -DCMAKE_BUILD_TYPE=Release
    echo "Building ($NPROC cores)..."
    time make -j$NPROC 2>&1
    ls -lh "$EXT4_BUILD/eboot.bin" 2>/dev/null
}

deploy_eboot() {
    check_vita_ip
    local eboot="$EXT4_BUILD/eboot.bin"
    [ ! -f "$eboot" ] && echo "ERROR: eboot.bin not found" && exit 1
    vita_kill
    ftp_upload "$eboot" "ux0:/app/${TITLE_ID}/eboot.bin"
    vita_launch
}

deploy_vpk() {
    check_vita_ip
    local vpk="$EXT4_BUILD/ac_vita.vpk"
    [ ! -f "$vpk" ] && echo "ERROR: VPK not found" && exit 1
    vita_kill
    ftp_upload "$vpk" "ux0:/data/${TITLE_ID}.vpk"
    echo "VPK uploaded. Install via VitaShell."
}

case "${1:-default}" in
    default|"")  do_build; deploy_eboot ;;
    full)        do_build; deploy_vpk ;;
    deploy)      deploy_eboot ;;
    build)       do_build ;;
    kill)        check_vita_ip; vita_kill ;;
    launch)      check_vita_ip; vita_launch ;;
    *)           echo "Usage: $0 [full|deploy|build|kill|launch]"; exit 1 ;;
esac

ccache -s 2>/dev/null | grep -E "Hits|Misses|cache size" || true
